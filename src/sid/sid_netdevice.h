/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_netdevice.h
 * A backend that is a client of the Network SID Device protocol.
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

#pragma once
#ifndef _US_SID_SID_NETDEVICE_H_
#define _US_SID_SID_NETDEVICE_H_

#include "sid_backend.h"
#include "sidfile.h"
#include "types.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace usbsid {

/**
 * @brief Sends register writes to a Network SID Device v4 or v5 server over TCP.
 *
 * Client of the same protocol USBSID-Pico's firmware speaks as a server
 * (repo/src/nsd.c, `-DENABLE_NET=1`), so this backend plays through a
 * USBSID-Pico reachable over the network, or any other compliant server
 * (e.g. WilfredC64's `sid-device`). Wire format (4 byte header, big endian
 * length/cycle fields, SID chip folded into the register byte for
 * TRY_WRITE/TRY_READ) matches repo/src/nsd.c and is confirmed by the
 * protocol author.
 *
 * All socket I/O runs on a dedicated sender thread; write()/flush() only
 * enqueue, with delay fields computed from emulation cycles at enqueue
 * time so wire latency never affects correctness (mirrors UsbSidBackend's
 * own threaded ring). One combined stream for all chips, matching
 * repo/src/net_ring.c's single FIFO (no per-chip separation), batched by
 * accumulated cycles rather than by video frame.
 */
class NetworkSidBackend final : public SidBackend
{
  public:
    NetworkSidBackend(void);
    ~NetworkSidBackend(void) override;

    /**
     * @brief Connect, negotiate the protocol version, and start the sender
     * thread. False on any failure; never leaves the backend half open.
     */
    bool connect(const char * host, uint16_t port);
    void disconnect(void);
    bool is_connected(void) const { return connected_; }

    uint8_t protocol_version(void) const { return version_; }

    /**
     * @brief TRY_SET_SID_COUNT (1..kMaxWireChips, clamped). Call once after
     * connect(), before the tune's init writes. Bookkeeping only on the
     * server; actual chip addressing is governed separately by sid_count()
     * and TRY_WRITE vs TRY_WRITE_EX.
     */
    void set_sid_count(uint8_t count);
    uint8_t sid_count(void) const { return sid_count_; }

    /**
     * @brief TRY_SET_FM_OPL (21, v5 only). Enables FM OPL on chip `sid`
     * (1-4) or disables it when `enable` is false. No-op against a pre-v5
     * server.
     */
    void set_fm_opl(bool enable, uint8_t sid);

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override;
    void reset(void) override;

    /** @brief Requests still waiting for the sender thread. Diagnostic. */
    size_t queue_depth(void) const;

  private:
    /* Largest chip index TRY_WRITE's register byte can fold in (chip 0-7).
     * A chip at or above this uses TRY_WRITE_EX instead - see write(). */
    static constexpr int kMaxWireChips = 8;
    /* sid_count_'s ceiling: kMaxSids (15, sidfile.h). */
    static constexpr int kMaxChips = kMaxSids;
    /* Largest quad count per TRY_WRITE (255 = 1020 B, under NSD_MAX_PAYLOAD). */
    static constexpr size_t kMaxQuadsPerPacket = 255;
    /* Buffered cycle threshold before write()/wait() force a send, tuned
     * under repo/src/nsd.c's NSD_CYCLES_HIGH BUSY threshold. */
    static constexpr uint32_t kFlushCyclesThreshold = 45000;
    /* Upper bound on how long something stays buffered below the cycle
     * threshold above (~60ms PAL). */
    static constexpr uint16_t kMaxFramesBetweenFlushes = 3;

    /* One outgoing request, queued between write()/flush() and the sender
     * thread sending it. */
    struct QueuedOp {
      uint8_t cmd;
      uint8_t sid_number;
      std::vector<uint8_t> payload;
    };

    /* Sender thread only, past connect() handing the socket over. */
    void sender_loop(void);
    bool send_request(uint8_t cmd, uint8_t sid_number,
                       const uint8_t * payload, uint16_t payload_len);
    bool recv_exact(uint8_t * buf, size_t len);
    /** @brief Send one request, retrying on BUSY. */
    bool request_ok(uint8_t cmd, uint8_t sid_number,
                     const uint8_t * payload, uint16_t payload_len);
    /** @brief Any send/recv failure: the connection is dead, stop touching it. */
    void fail(void);

    /* Emulation thread only. */
    /** @brief Append one request to the queue for the sender thread. */
    void enqueue(uint8_t cmd, uint8_t sid_number,
                 const uint8_t * payload, uint16_t payload_len);
    /**
     * @brief Bring `total` under 0x10000 by appending dummy delay quads
     * (register $1E, open bus, delay 0xffff each) to pending_quads_, then
     * return what's left. `for_ex` says which stream the caller's entry
     * joins next; the other stream is flushed first to preserve order.
     */
    uint16_t append_delay_quads(uint32_t total, bool for_ex);
    /** @brief Send whatever is buffered, if anything is. */
    void do_flush(void);
    /** @brief v5 only: send whatever is buffered in the TRY_WRITE_EX stream. */
    void do_flush_ex(void);
    /**
     * @brief Route an FM/OPL "parked" write onto TRY_WRITE_EX's
     * $df00-$dfff space, when the server is v5+. The index port
     * (reg & 0x1f == 0) holds the OPL register number until the matching
     * data-port write (reg & 0x1f == 0x10) completes the pair.
     */
    void handle_fmopl_write(addr_t reg, data_t value, uint16_t cycles);

    int      sock_ = -1;
    /* Written by the sender thread on failure, read by the emulation
     * thread before enqueueing. */
    std::atomic<bool> connected_{false};
    uint8_t  version_ = 0;
    uint8_t  sid_count_ = 1;

    std::thread             sender_thread_;
    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedOp>    queue_;
    std::atomic<bool>       running_{false};

    /* Emulation thread only, no lock needed. extra_cycles_ carries a gap
     * forward to whatever needs it next; pending_quads_ is the one shared
     * stream every chip's writes go into. */
    uint32_t extra_cycles_ = 0;
    std::vector<uint8_t> pending_quads_;
    uint32_t buffered_cycles_ = 0;
    uint16_t frames_since_flush_ = 0;

    /* v5 TRY_WRITE_EX stream (FM/OPL only). pending_quads_ and pending_ex_
     * are never both non-empty: whichever a write is about to join flushes
     * the other first, preserving TCP order without two accumulators.
     * opl_pending_reg_/_valid_ hold the index-port half of the pair. */
    bool     opl_pending_valid_ = false;
    uint8_t  opl_pending_reg_ = 0;
    std::vector<uint8_t> pending_ex_;
    static constexpr size_t kMaxExEntriesPerPacket = 204; /* 5*204 = 1020 B, under NSD_MAX_PAYLOAD */
};

} /* namespace usbsid */

#endif /* _US_SID_SID_NETDEVICE_H_ */
