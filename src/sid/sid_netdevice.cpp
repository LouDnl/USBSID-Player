/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_netdevice.cpp
 *
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2026 LouD
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "sid_netdevice.h"

#include <cstdio>
#include <cstring>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace usbsid {

namespace {

/* Command opcodes and response codes, only the subset this client uses.
 * Defined locally rather than pulled from the firmware's nsd.h: this is a
 * wire protocol shared with third party servers, not an internal API.
 * Cross checked against repo/src/nsd.h. */
constexpr uint8_t kCmdFlush           = 0;
constexpr uint8_t kCmdTrySetSidCount  = 1;
constexpr uint8_t kCmdTryReset        = 3;
constexpr uint8_t kCmdTryWrite        = 5;
constexpr uint8_t kCmdGetVersion      = 7;
/* v5: 16-bit register address, used for FM/OPL ($df00-$dfff) and chip 8+
 * (TRY_WRITE's byte-folded register only reaches chip 0-7). */
constexpr uint8_t kCmdTryWriteEx      = 19;
/* v5: enable/disable FM OPL on the server for this connection, payload one
 * byte {0,1} - see set_fm_opl(). */
constexpr uint8_t kCmdTrySetFmOpl     = 21;

/* Register addresses at or above this are the "unclaimed FM/OPL" park
 * (kMaxSids * 0x20, mos6581_8580.cpp), routed via handle_fmopl_write(). */
constexpr addr_t kFmOplRange = static_cast<addr_t>(kMaxSids * 0x20);
/* Register addresses at or above this no longer fit TRY_WRITE's 8 bit byte
 * and go out as TRY_WRITE_EX instead (addr = reg verbatim). */
constexpr addr_t kWireByteRange = 0x0100;

/* Open-bus register on a real 6581/8580, used for a dummy quad that just
 * advances the ring's delay clock past one quad's 16 bit field limit. */
constexpr uint8_t kDummyDelayReg = 0x1e;

constexpr uint8_t kRespOk      = 0;
constexpr uint8_t kRespBusy    = 1;
constexpr uint8_t kRespError   = 2;
constexpr uint8_t kRespRead    = 3;
constexpr uint8_t kRespVersion = 4;

/* ~1ms retry delay per spec guidance, bounded to ~2s total so a server
 * stuck BUSY forever reads as dead rather than a hang. Sender thread only. */
constexpr int kMaxBusyRetries = 2000;

} /* namespace */

NetworkSidBackend::NetworkSidBackend(void) {}

NetworkSidBackend::~NetworkSidBackend(void)
{
  disconnect();
}

/* ==================== Sender thread only ==================== */

void NetworkSidBackend::sender_loop(void)
{
  while (true) {
    QueuedOp op;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
      if (!running_) return; /* disconnect() wants out; it already cleared the queue */
      if (!connected_) return; /* a prior request already failed and closed the socket */
      op = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!request_ok(op.cmd, op.sid_number, op.payload.data(),
                     static_cast<uint16_t>(op.payload.size())) && !connected_) {
      /* request_ok() returns false both when the link actually died (it
       * called fail(), which cleared connected_) and when the server merely
       * rejected this one request with ERROR (link still fine - see its own
       * comment). Only the former should stop this thread; otherwise one
       * ERROR'd request would silently deafen every request after it while
       * is_connected() kept reporting true. */
      return;
    }
  }
}

bool NetworkSidBackend::send_request(uint8_t cmd, uint8_t sid_number,
                                      const uint8_t * payload, uint16_t payload_len)
{
  const uint8_t header[4] = {
    cmd, sid_number,
    static_cast<uint8_t>(payload_len >> 8),
    static_cast<uint8_t>(payload_len & 0xff)
  };
  if (::send(sock_, header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header))) {
    return false;
  }
  size_t sent = 0;
  while (sent < payload_len) {
    const ssize_t n = ::send(sock_, payload + sent, payload_len - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool NetworkSidBackend::recv_exact(uint8_t * buf, size_t len)
{
  size_t got = 0;
  while (got < len) {
    const ssize_t n = ::recv(sock_, buf + got, len - got, 0);
    if (n <= 0) return false; /* closed, error, or the receive timeout firing */
    got += static_cast<size_t>(n);
  }
  return true;
}

void NetworkSidBackend::drain_extra(void)
{
  /* Best effort: a compliant server appending bytes to ERROR (or, in
   * principle, anything past what this client otherwise consumes in full)
   * sends them as part of the same write() as the status byte, so they are
   * typically already sitting in the socket's receive buffer by the time
   * request_ok() gets here. A short non-blocking drain clears them before
   * the next request is sent, so they can never be misread as the next
   * response's own header. Not exhaustive against a slow or fragmented
   * sender, but resolves the desync risk against any server that actually
   * uses the optional message - this firmware's own (repo/src/nsd.c) never
   * does, so this path is untested against it. */
  uint8_t discard[256];
  for (int i = 0; i < 8; i++) {
    const ssize_t n = ::recv(sock_, discard, sizeof(discard), MSG_DONTWAIT);
    if (n <= 0) break;
  }
}

bool NetworkSidBackend::request_ok(uint8_t cmd, uint8_t sid_number,
                                    const uint8_t * payload, uint16_t payload_len)
{
  for (int attempt = 0; attempt < kMaxBusyRetries; attempt++) {
    if (!send_request(cmd, sid_number, payload, payload_len)) { fail(); return false; }
    uint8_t resp = 0;
    if (!recv_exact(&resp, 1)) { fail(); return false; }
    if (resp == kRespOk) return true;
    if (resp == kRespBusy) { usleep(1000); continue; }
    if (resp == kRespRead) {
      /* "one byte value follows" (spec) - always exactly one, whether or
       * not this client asked for it. Discard: this client does not issue
       * TRY_READ/TRY_READ_EX today, so an unsolicited READ here is not one
       * of its own requests being answered. */
      uint8_t discard = 0;
      recv_exact(&discard, 1);
    } else if (resp == kRespError) {
      drain_extra();
    }
    /* ERROR, or anything else this client does not expect here: the server
     * rejected this one request outright, not a sign the link is dead. */
    return false;
  }
  fail(); /* never drained in a bounded time - treat like a dead connection */
  return false;
}

void NetworkSidBackend::fail(void)
{
  if (sock_ >= 0) ::close(sock_);
  sock_ = -1;
  connected_ = false;
}

/* ==================== Emulation thread only ==================== */

void NetworkSidBackend::enqueue(uint8_t cmd, uint8_t sid_number,
                                 const uint8_t * payload, uint16_t payload_len)
{
  QueuedOp op;
  op.cmd = cmd;
  op.sid_number = sid_number;
  if (payload_len > 0) op.payload.assign(payload, payload + payload_len);
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(std::move(op));
  }
  queue_cv_.notify_one();
}

uint16_t NetworkSidBackend::append_delay_quads(uint32_t total, bool for_ex)
{
  /* Flush the other stream first so TCP's strict request/response order
   * stays this backend's true chronological order too. */
  if (for_ex) { if (!pending_quads_.empty()) do_flush(); }
  else        { if (!pending_ex_.empty())    do_flush_ex(); }

  while (total > 0xffff) {
    pending_quads_.push_back(0xff);
    pending_quads_.push_back(0xff);
    pending_quads_.push_back(kDummyDelayReg);
    pending_quads_.push_back(0x00);
    buffered_cycles_ += 0xffff;
    total -= 0xffff;
    if (pending_quads_.size() >= kMaxQuadsPerPacket * 4) do_flush();
  }
  /* Dummy quads always land in pending_quads_ - flush them now if the
   * caller's entry is headed for pending_ex_ instead. */
  if (for_ex && !pending_quads_.empty()) do_flush();
  return static_cast<uint16_t>(total);
}

void NetworkSidBackend::do_flush(void)
{
  if (pending_quads_.empty()) return;
  /* sid_number is 0: the chip rides in each quad's own register byte. */
  enqueue(kCmdTryWrite, 0, pending_quads_.data(), static_cast<uint16_t>(pending_quads_.size()));
  pending_quads_.clear();
  buffered_cycles_ = 0;
  frames_since_flush_ = 0;
}

void NetworkSidBackend::do_flush_ex(void)
{
  if (pending_ex_.empty()) return;
  enqueue(kCmdTryWriteEx, 0, pending_ex_.data(), static_cast<uint16_t>(pending_ex_.size()));
  pending_ex_.clear();
  buffered_cycles_ = 0;
  frames_since_flush_ = 0;
}

void NetworkSidBackend::handle_fmopl_write(addr_t reg, data_t value, uint16_t cycles)
{
  /* reg & 0x1f: 0 is the index port, 0x10 is the data port. */
  if ((reg & 0x1f) == 0x00) {
    /* Index half: remember the OPL register number, nothing on the wire
     * yet. cycles_since_last_event() keeps the clock moving regardless. */
    opl_pending_reg_ = value;
    opl_pending_valid_ = true;
    return;
  }
  /* Data half. A data write with no preceding index write shouldn't
   * happen, but be safe. */
  if (!opl_pending_valid_) return;
  opl_pending_valid_ = false;

  uint32_t total = extra_cycles_ + cycles;
  extra_cycles_ = 0;
  const uint16_t delay = append_delay_quads(total, /* for_ex = */ true);

  const uint16_t addr = static_cast<uint16_t>(0xdf00 + opl_pending_reg_);
  pending_ex_.push_back(static_cast<uint8_t>(delay >> 8));
  pending_ex_.push_back(static_cast<uint8_t>(delay & 0xff));
  pending_ex_.push_back(static_cast<uint8_t>(addr >> 8));
  pending_ex_.push_back(static_cast<uint8_t>(addr & 0xff));
  pending_ex_.push_back(value);
  buffered_cycles_ += delay;

  if (pending_ex_.size() >= kMaxExEntriesPerPacket * 5 || buffered_cycles_ >= kFlushCyclesThreshold) {
    do_flush_ex();
  }
}

bool NetworkSidBackend::connect(const char * host, uint16_t port)
{
  disconnect();

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  char port_str[6];
  std::snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo * res = nullptr;
  if (::getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) return false;

  int sock = -1;
  for (struct addrinfo * p = res; p != nullptr; p = p->ai_next) {
    sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sock < 0) continue;
    if (::connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
    ::close(sock);
    sock = -1;
  }
  ::freeaddrinfo(res);
  if (sock < 0) return false;

  /* Request/response protocol: Nagle would add up to 40ms per round trip. */
  const int one = 1;
  ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  /* A dead/wedged server must not hang the sender thread forever. */
  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sock_ = sock;
  connected_ = true;

  /* GET_VERSION runs synchronously here, before the sender thread exists -
   * a one-time startup cost. */
  uint8_t resp[2];
  if (!send_request(kCmdGetVersion, 0, nullptr, 0) || !recv_exact(resp, sizeof(resp))
      || resp[0] != kRespVersion || resp[1] < 1) {
    fail();
    return false;
  }
  version_ = resp[1];

  running_ = true;
  sender_thread_ = std::thread(&NetworkSidBackend::sender_loop, this);
  return true;
}

void NetworkSidBackend::disconnect(void)
{
  /* Stop the sender thread and drop whatever it had not sent yet - a
   * teardown should not wait for a slow link. */
  running_ = false;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
  }
  queue_cv_.notify_all();
  if (sender_thread_.joinable()) sender_thread_.join();

  if (connected_) {
    /* Sender thread is gone, socket is ours again. Best effort, not
     * retried: FLUSH stops the server continuing to make noise. */
    send_request(kCmdFlush, 0, nullptr, 0);
  }
  if (sock_ >= 0) ::close(sock_);
  sock_ = -1;
  connected_ = false;
  version_ = 0;

  extra_cycles_ = 0;
  pending_quads_.clear();
  pending_ex_.clear();
  opl_pending_valid_ = false;
  buffered_cycles_ = 0;
  frames_since_flush_ = 0;
}

void NetworkSidBackend::set_sid_count(uint8_t count)
{
  if (count < 1) count = 1;
  if (count > kMaxChips) count = kMaxChips;
  sid_count_ = count;
  if (!connected_) return;
  /* TRY_SET_SID_COUNT's header SID number field carries the count itself,
   * not a chip index. Clamped to kMaxWireChips (8), the server's own
   * ceiling for this command, separate from write()'s TRY_WRITE_EX range. */
  const uint8_t wire_count = (sid_count_ > kMaxWireChips)
    ? static_cast<uint8_t>(kMaxWireChips) : sid_count_;
  enqueue(kCmdTrySetSidCount, wire_count, nullptr, 0);
}

void NetworkSidBackend::set_fm_opl(bool enable)
{
  if (!connected_ || version_ < 5) return;
  const uint8_t payload[1] = { static_cast<uint8_t>(enable ? 1 : 0) };
  enqueue(kCmdTrySetFmOpl, 0, payload, 1);
}

void NetworkSidBackend::write(addr_t reg, data_t value, uint16_t cycles)
{
  if (!connected_) return;
  /* kFmOplRange and above are FM/OPL addresses no SID claims. A pre-v5
   * server can't represent these; a v5+ server gets them via TRY_WRITE_EX's
   * $df00-$dfff space. */
  if (reg >= kFmOplRange) {
    if (version_ >= 5) handle_fmopl_write(reg, value, cycles);
    return;
  }
  const uint8_t chip = static_cast<uint8_t>(reg >> 5);
  if (chip >= sid_count_) return;

  /* Chip 0-7 fits TRY_WRITE's byte-folded register; chip 8+ needs
   * TRY_WRITE_EX, which only a v5+ server understands. */
  const bool for_ex = (reg >= kWireByteRange);
  if (for_ex && version_ < 5) return; /* no way to represent this chip at all */

  uint32_t total = extra_cycles_ + cycles;
  extra_cycles_ = 0;
  const uint16_t delay = append_delay_quads(total, for_ex);

  if (for_ex) {
    pending_ex_.push_back(static_cast<uint8_t>(delay >> 8));
    pending_ex_.push_back(static_cast<uint8_t>(delay & 0xff));
    pending_ex_.push_back(static_cast<uint8_t>(reg >> 8));
    pending_ex_.push_back(static_cast<uint8_t>(reg & 0xff));
    pending_ex_.push_back(value);
    buffered_cycles_ += delay;
    if (pending_ex_.size() >= kMaxExEntriesPerPacket * 5 || buffered_cycles_ >= kFlushCyclesThreshold) {
      do_flush_ex();
    }
    return;
  }

  pending_quads_.push_back(static_cast<uint8_t>(delay >> 8));
  pending_quads_.push_back(static_cast<uint8_t>(delay & 0xff));
  pending_quads_.push_back(static_cast<uint8_t>(reg));
  pending_quads_.push_back(value);
  buffered_cycles_ += delay;

  /* Send early on either cap: byte limit, or buffered tune time. */
  if (pending_quads_.size() >= kMaxQuadsPerPacket * 4 || buffered_cycles_ >= kFlushCyclesThreshold) {
    do_flush();
  }
}

/** @brief More than $ffff cycles passed with nothing to write. */
void NetworkSidBackend::wait(uint16_t cycles)
{
  if (!connected_) return;
  const uint32_t total = extra_cycles_ + cycles;
  extra_cycles_ = append_delay_quads(total, /* for_ex = */ false);
  if (pending_quads_.size() >= kMaxQuadsPerPacket * 4 || buffered_cycles_ >= kFlushCyclesThreshold) {
    do_flush();
  }
}

/**
 * @brief End of frame. NOT a forced send - only flushes once buffered
 * content has sat through kMaxFramesBetweenFlushes frames.
 */
void NetworkSidBackend::flush(void)
{
  if (!connected_) return;
  if (pending_quads_.empty() && pending_ex_.empty()) return;
  /* At most one of the two is ever non-empty; both calls below are
   * harmless no-ops on whichever one isn't. */
  if (++frames_since_flush_ >= kMaxFramesBetweenFlushes) {
    do_flush();
    do_flush_ex();
  }
}

void NetworkSidBackend::reset(void)
{
  if (!connected_) return;
  /* Drop everything buffered client side first - those writes belong to a
   * tune being abandoned. */
  extra_cycles_ = 0;
  pending_quads_.clear();
  pending_ex_.clear();
  opl_pending_valid_ = false;
  buffered_cycles_ = 0;
  frames_since_flush_ = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
  }
  /* FLUSH clears the server's queue and mutes it; TRY_RESET below follows
   * per spec. Queued, not sent directly: only the sender thread touches
   * the socket. */
  enqueue(kCmdFlush, 0, nullptr, 0);
  const uint8_t volume = 0x0f; /* full volume, filter off: a real reset state */
  enqueue(kCmdTryReset, 0, &volume, 1);
}

size_t NetworkSidBackend::queue_depth(void) const
{
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return queue_.size();
}

} /* namespace usbsid */
