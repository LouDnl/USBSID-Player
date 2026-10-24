/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_netdevice.cpp
 * The Network SID Device client backend against a loopback NSD v5 server.
 *
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2026 LouD
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "net_compat.h"
#include "sid_netdevice.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;

namespace {

/* Loopback NSD v5 server posing as USBSID-Pico (or as `name`): answers every
 * request OK, records each TRY_WRITE (5) and TRY_WRITE_EX (19) payload in
 * `packets` and every request in `all`. */
struct FakeNsd {
  struct Packet { uint8_t cmd; uint8_t sid; std::vector<uint8_t> payload; };
  const char * name = "USBSID-Pico";
  net_sock_t listen_fd = kNetInvalidSock;
  uint16_t port = 0;
  std::thread th;
  std::mutex mu;
  std::vector<Packet> packets;
  std::vector<Packet> all;
  uint8_t max_sids = 255; /* TRY_SET_SID_COUNT above this answers ERROR (2) */

  bool start(void)
  {
    if (!net_startup()) return false;
    listen_fd = net_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == kNetInvalidSock) return false;
    if (!net_listen_loopback(listen_fd, &port)) return false;
    th = std::thread([this] { serve(); });
    return true;
  }
  static bool rd(net_sock_t fd, uint8_t * b, size_t n)
  {
    size_t got = 0;
    while (got < n) {
      const net_ssize_t r = net_recv(fd, b + got, n - got);
      if (r <= 0) return false;
      got += static_cast<size_t>(r);
    }
    return true;
  }
  void serve(void)
  {
    const net_sock_t fd = net_accept(listen_fd);
    if (fd == kNetInvalidSock) return;
    net_nosigpipe(fd);
    for (;;) {
      uint8_t h[4];
      if (!rd(fd, h, 4)) break;
      std::vector<uint8_t> pl(static_cast<size_t>((h[2] << 8) | h[3]));
      if (!pl.empty() && !rd(fd, pl.data(), pl.size())) break;
      /* Assign temporaries, not `resp = { ... }`: GCC 16 -O3 flags that
       * assign() into an empty vector with a false -Wnonnull. */
      using Bytes = std::vector<uint8_t>;
      Bytes resp;
      switch (h[0]) {
        case 7:  resp = Bytes{ 4, 5 }; break;                  /* version 5 */
        case 10: resp = Bytes{ 5, 1 }; break;                  /* one config */
        case 11: resp = Bytes{ 6, 0 };                         /* model, name */
                 for (const char * c = name; *c; c++) resp.push_back(static_cast<uint8_t>(*c));
                 resp.push_back(0);
                 break;
        case 1:  resp = Bytes{ static_cast<uint8_t>(h[1] > max_sids ? 2 : 0) }; break;
        default: resp = Bytes{ 0 }; break;
      }
      {
        std::lock_guard<std::mutex> lock(mu);
        if (h[0] == 5 || h[0] == 19) packets.push_back({ h[0], h[1], pl });
        all.push_back({ h[0], h[1], pl });
      }
      if (net_send(fd, resp.data(), resp.size()) <= 0) break;
    }
    net_close(fd);
  }
  size_t count(void)
  {
    std::lock_guard<std::mutex> lock(mu);
    return packets.size();
  }
  void stop(void)
  {
    net_close(listen_fd);
    if (th.joinable()) th.join();
  }
};

/* Once FM is in use on a v5 hardware server, SID writes join the FM writes in
 * one TRY_WRITE_EX stream and only a gap of 4096+ cycles ends a packet. */
int test_netdevice_ex_stream(void)
{
  FakeNsd srv;
  US_CHECK(srv.start(), "fake server starts");
  NetworkSidBackend be;
  US_CHECK(be.connect("127.0.0.1", srv.port), "connects");
  US_CHECK(be.protocol_version() == 5 && !be.is_software(), "v5 hardware server");
  be.set_sid_count(2);

  be.write(0x0000, 0x11, 10);       /* before FM: plain TRY_WRITE */
  be.write(0x01e0, 0x22, 100);      /* FM index port: arms the stream */
  be.write(0x01f0, 0x33, 100);      /* FM data port */
  be.write(0x0004, 0x41, 1000);     /* SID, below 4096 */
  be.write(0x0020, 0x42, 2000);     /* SID 2, below 4096 */
  be.write(0x0004, 0x43, 5000);     /* big gap: last entry of its packet */
  be.write(0x0004, 0x44, 10);
  be.send_pending();

  for (int i = 0; i < 200 && srv.count() < 4; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  be.disconnect();
  srv.stop();

  std::vector<FakeNsd::Packet> ex, plain;
  for (const FakeNsd::Packet & p : srv.packets) (p.cmd == 19 ? ex : plain).push_back(p);
  US_CHECK_EQ_U(plain.size(), 1, "one TRY_WRITE packet (pre-FM)");
  US_CHECK_EQ_U(ex.size(), 2, "two TRY_WRITE_EX packets");
  if (ex.size() == 2) {
    US_CHECK_EQ_U(ex[0].payload.size(), 4 * 5, "FM pair + 3 SID writes in one packet");
    US_CHECK_EQ_U(ex[1].payload.size(), 1 * 5, "tail after the big gap");
    if (ex[0].payload.size() == 20) {
      const std::vector<uint8_t> & b = ex[0].payload;
      US_CHECK_EQ_U((b[2] << 8) | b[3], 0xdf22, "FM address");
      US_CHECK_EQ_U((b[7] << 8) | b[8], 0x0004, "SID 1 address verbatim");
      US_CHECK_EQ_U((b[12] << 8) | b[13], 0x0020, "SID 2 address verbatim");
      US_CHECK_EQ_U((b[17] << 8) | b[18], 0x0004, "big gap entry address");
      US_CHECK_EQ_U((b[15] << 8) | b[16], 5000, "big gap delay kept");
    }
  }
  return 0;
}

/* Software server, multi-SID: every chip gets a center position and a model
 * after the count (sid-device mixes 2+ chips silent without a position), and
 * chip N's writes keep N in the TRY_WRITE register byte. */
int test_netdevice_software_multisid(void)
{
  FakeNsd srv;
  srv.name = "reSID Device (6581)";
  US_CHECK(srv.start(), "fake server starts");
  NetworkSidBackend be;
  US_CHECK(be.connect("127.0.0.1", srv.port), "connects");
  US_CHECK(be.is_software(), "software server");
  be.set_sid_count(3);
  be.reset();
  be.write(0x0018, 0x0f, 10);       /* chip 0 volume */
  be.write(0x0038, 0x0f, 10);       /* chip 1 volume */
  be.write(0x0058, 0x0f, 10);       /* chip 2 volume */
  be.send_pending();

  for (int i = 0; i < 200 && srv.count() < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  be.disconnect();
  srv.stop();

  /* Last TRY_SET_SID_COUNT before the writes, and what follows it. */
  size_t last_count = srv.all.size();
  for (size_t i = 0; i < srv.all.size(); i++) if (srv.all[i].cmd == 1) last_count = i;
  US_CHECK(last_count < srv.all.size(), "TRY_SET_SID_COUNT sent");
  if (last_count >= srv.all.size()) return 0;
  US_CHECK_EQ_U(srv.all[last_count].sid, 3, "count in sid number");
  unsigned pos_mask = 0, model_mask = 0;
  for (size_t i = last_count + 1; i < srv.all.size(); i++) {
    const FakeNsd::Packet & p = srv.all[i];
    if (p.cmd == 1) break;
    if (p.cmd == 12 && p.payload.size() == 1 && p.payload[0] == 0) pos_mask |= 1u << p.sid;
    if (p.cmd == 14 && p.payload.size() == 1) model_mask |= 1u << p.sid;
  }
  US_CHECK_EQ_U(pos_mask, 0x7, "SET_SID_POSITION center for chips 0-2");
  US_CHECK_EQ_U(model_mask, 0x7, "TRY_SET_SID_MODEL for chips 0-2");
  US_CHECK_EQ_U(srv.packets.size(), 1, "one TRY_WRITE packet");
  if (srv.packets.size() == 1 && srv.packets[0].payload.size() == 12) {
    const std::vector<uint8_t> & b = srv.packets[0].payload;
    US_CHECK_EQ_U(b[2], 0x18, "chip 0 register byte");
    US_CHECK_EQ_U(b[6], 0x38, "chip 1 register byte");
    US_CHECK_EQ_U(b[10], 0x58, "chip 2 register byte");
  } else {
    US_CHECK(false, "TRY_WRITE packet holds 3 quads");
  }
  return 0;
}

/* Server that refuses counts above N: the count steps down 8, 4, 1, sid_count()
 * ends at what was accepted and writes to the refused chips are dropped. */
int test_netdevice_count_fallback(void)
{
  FakeNsd srv;
  srv.name = "reSID Device (6581)";
  srv.max_sids = 4;
  US_CHECK(srv.start(), "fake server starts");
  NetworkSidBackend be;
  US_CHECK(be.connect("127.0.0.1", srv.port), "connects");
  be.set_sid_count(10);
  for (int i = 0; i < 200 && be.sid_count() > 4; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  US_CHECK_EQ_U(be.sid_count(), 4, "sid_count dropped to the accepted 4");
  be.write(0x0018, 0x0f, 10);       /* chip 0 */
  be.write(0x0098, 0x0f, 10);       /* chip 4: dropped */
  be.write(0x0138, 0x0f, 10);       /* chip 9: dropped */
  be.send_pending();
  for (int i = 0; i < 200 && srv.count() < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  be.disconnect();
  srv.stop();

  std::vector<uint8_t> counts;
  unsigned pos_mask = 0;
  for (const FakeNsd::Packet & p : srv.all) {
    if (p.cmd == 1 && p.sid > 1) counts.push_back(p.sid); /* skip connect()'s own count of 1 */
    if (p.cmd == 12) pos_mask |= 1u << p.sid;
  }
  US_CHECK_EQ_U(counts.size(), 3, "count tried 10, 8, 4");
  if (counts.size() == 3) {
    US_CHECK_EQ_U(counts[0], 10, "first try the real count");
    US_CHECK_EQ_U(counts[1], 8, "then 8");
    US_CHECK_EQ_U(counts[2], 4, "then 4, accepted");
  }
  US_CHECK_EQ_U(pos_mask, 0xf, "positions only for chips 0-3");
  US_CHECK_EQ_U(srv.packets.size(), 1, "only the chip 0 write went out");
  return 0;
}

} /* namespace */

int us_test_netdevice(void)
{
  US_TEST_BEGIN("netdevice");

  test_netdevice_ex_stream();
  test_netdevice_software_multisid();
  test_netdevice_count_fallback();

  US_TEST_END("netdevice");
}

US_TEST_MAIN(us_test_netdevice)
