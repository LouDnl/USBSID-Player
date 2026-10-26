/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_netdevice.h
 * A backend that is a client using the Network SID Device protocol
 *
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
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

#pragma once
#ifndef _US_SID_SID_NETDEVICE_H_
#define _US_SID_SID_NETDEVICE_H_

#include "sid_backend.h"
#include "sidfile.h"
#include "types.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usbsid {

/**
 * @brief Send register writes to a Network SID Device v4 or v5 server over TCP
 *
 * Client of the protocol the USBSID-Pico firmware serves (see USBSID-Pico firmware: nsd.c -DENABLE_NET=1)
 * Plays through a USBSID-Pico on the network or any compliant server (WilfredC64 sid-device)
 * Wire format: 4 byte header, big endian length/cycle fields, SID chip folded into the register byte for TRY_WRITE/TRY_READ
 *
 * Socket I/O runs on a sender thread, write()/flush() only enqueue with delay fields computed from emulation cycles
 * One combined stream for all chips (USBSID-Pico firmware: net_ring.c has a single FIFO), batched by accumulated cycles
 */
class NetworkSidBackend final : public SidBackend
{
  public:
    NetworkSidBackend(void);
    ~NetworkSidBackend(void) override;

    /** @brief Connect, negotiate the protocol version and start the sender thread, false on failure */
    bool connect(const char * host, uint16_t port);
    void disconnect(void);
    bool is_connected(void) const { return connected_; }

    uint8_t protocol_version(void) const { return version_; }

    /**
     * @brief False when the server names itself "USBSID-Pico" in GET_CONFIG_INFO (firmware NSD server)
     *
     * True for software servers such as sid-device, decided in connect()
     * They want small batches, idle time sent and no access overhead
     */
    bool is_software(void) const { return !hardware_; }

    /**
     * @brief Software servers only: store the tune clock and chip model, resend the chip setup (send_chip_setup())
     *
     * Model is the config index whose GET_CONFIG_INFO model matches, 6581 for Unknown/Any
     * Call before the tune init writes. No-op against the firmware server
     */
    void configure_tune(bool ntsc, SidModel model);

    /**
     * @brief TRY_SET_SID_COUNT (1..kMaxChips), on ERROR retry 8, 4, then 1 and sid_count() drops to what was accepted
     *
     * Call after connect() and on every chip count change, before the tune init writes
     * Software server: followed by SET_SID_POSITION, TRY_SET_SID_MODEL for every chip and TRY_SET_CLOCK (send_chip_setup())
     * as the count command empties the chip array and resets the clock
     * Firmware server: bookkeeping only, addressing is set by sid_count() and TRY_WRITE vs TRY_WRITE_EX
     */
    void set_sid_count(uint8_t count);
    uint8_t sid_count(void) const { return sid_count_; }

    /**
     * @brief TRY_SET_FM_OPL (21, v5 only), enable or disable FM OPL for this connection
     *
     * One byte {0,1}, sid number is ignored. The server config decides which SID answers as FM OPL
     * No-op against a pre-v5 server
     */
    void set_fm_opl(bool enable);

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override;
    void reset(void) override;

    /** @brief Requests waiting for the sender thread, diagnostic */
    size_t queue_depth(void) const;

    /**
     * @brief Block while more than kMaxInflightCycles of TRY_WRITE time is queued or unaccepted
     *
     * The server is the clock, its BUSY bounds the lead. Returns early on stop, disconnect or sender failure
     * Polled every 20 ms, a stop flag set without a notify still unblocks
     */
    void wait_for_room(const std::atomic<bool> & stop);
    /** @brief Same, for a plain sig_atomic_t stop flag */
    void wait_for_room(const volatile sig_atomic_t & stop);

    /** @brief Send what is buffered at once, e.g. for a pause's silence */
    void send_pending(void);

  private:
    /* Ceiling of sid_count_, kMaxSids (15, sidfile.h) */
    static constexpr int kMaxChips = kMaxSids;
    /* Largest quad count per TRY_WRITE (255 = 1020 B, under NSD_MAX_PAYLOAD). */
    static constexpr size_t kMaxQuadsPerPacket = 255;
    /* A quad delay at or above this ends its packet (flush_if_big_delay()). */
    static constexpr uint16_t kBigDelay = 256;
    /* Big-delay limit on a USBSID-Pico server, both streams
     * Above the gaps between writes in a frame, below the idle gap */
    static constexpr uint16_t kBigDelayHardware = 4096;
    /* Buffered cycle threshold before write()/wait() force a send, tuned
     * under USBSID-Pico firmware: nsd.c's NSD_CYCLES_HIGH BUSY threshold. */
    static constexpr uint32_t kFlushCyclesThreshold = 45000;
    /* Max time something stays buffered below the cycle threshold (~60ms PAL) */
    static constexpr uint16_t kMaxFramesBetweenFlushes = 3;
    /* Software server (sid-device) reads BUSY past 63*312*10 cycles and starts playback after 63*312*5
     * Batches are half a frame, sent every frame */
    static constexpr uint32_t kSoftFlushCyclesThreshold = 9828;
    static constexpr uint16_t kSoftMaxFramesBetweenFlushes = 1;
    /* GET_CONFIG_INFO is queried for at most this many configurations. */
    static constexpr int kMaxConfigs = 32;
    /* wait_for_room() blocks past this much queued tune time (2 PAL frames), server BUSY bounds the rest */
    static constexpr uint32_t kMaxInflightCycles = 63 * 312 * 2;

    /* One outgoing request, queued between write()/flush() and the sender thread */
    struct QueuedOp {
      uint8_t cmd;
      uint8_t sid_number;
      uint32_t cycles = 0; /* TRY_WRITE time, counted in inflight_cycles_ */
      std::vector<uint8_t> payload;
    };

    /* Sender thread only, after connect() handed over the socket */
    void sender_loop(void);
    bool send_request(uint8_t cmd, uint8_t sid_number,
                       const uint8_t * payload, uint16_t payload_len);
    /** @brief Resolve TRY_SET_SID_COUNT reply, fall back 8, 4, 1 on ERROR */
    bool negotiate_sid_count(uint8_t requested, bool ok);
    bool recv_exact(uint8_t * buf, size_t len);
    /**
     * @brief Drain what a response carries past the status byte and was not read (ERROR message, unrequested READ)
     *
     * These have no length prefix, leftover bytes would be read as the following response header, see request_ok()
     */
    void drain_extra(void);
    /** @brief Send one request, retrying on BUSY. */
    bool request_ok(uint8_t cmd, uint8_t sid_number,
                     const uint8_t * payload, uint16_t payload_len);
    /** @brief Synchronous GET_CONFIG_COUNT/INFO and server type detection. */
    bool query_configs(void);
    /** @brief Drop every queued request and its in-flight cycles. */
    void drop_queued(void);
    /** @brief Common body of both wait_for_room() overloads. */
    template <typename Stop> void wait_room(const Stop & stop);
    /** @brief Send/recv failure, the connection is dead */
    void fail(void);

    /* Emulation thread only. */
    /** @brief Append one request to the queue for the sender thread. */
    void enqueue(uint8_t cmd, uint8_t sid_number,
                 const uint8_t * payload, uint16_t payload_len,
                 uint32_t cycles = 0);
    /**
     * @brief Bring total under 0x10000 with dummy delay quads and return the rest
     *
     * Quads are register $1E (open bus), delay 0xffff each, appended to pending_quads_
     * for_ex says which stream the entry joins, the other is flushed first to keep order
     */
    uint16_t append_delay_quads(uint32_t total, bool for_ex);
    void flush_if_big_delay(uint16_t delay, bool for_ex);
    /** @brief Send whatever is buffered, if anything is. */
    void do_flush(void);
    /** @brief v5 only: send whatever is buffered in the TRY_WRITE_EX stream. */
    void do_flush_ex(void);
    /**
     * @brief Route an FM/OPL parked write to TRY_WRITE_EX on v5+ servers
     *
     * Index port (reg & 0x1f == 0) goes out as {delay, $df40, register}, data port (reg & 0x1f == 0x10) as {delay, $df50, data}
     * Two entries, each with its own delay
     */
    void handle_fmopl_write(addr_t reg, data_t value, uint16_t cycles);

    /**
     * @brief Software servers only: TRY_SET_SID_COUNT, SET_SID_POSITION (center), TRY_SET_SID_MODEL for chips 0..count-1, then TRY_SET_CLOCK
     *
     * A software server (JSidplay2 NetSIDDev, sid-device) has no chip behind a sid number until its model is set
     * TRY_SET_SID_COUNT drops every chip, position and the clock, sid-device mixes 2+ chips silent until positioned
     * Goes out as one block before any TRY_RESET or write (emulation thread only, queued)
     */
    void send_chip_setup(void);
    /** @brief Config index for want_model_, see configure_tune() */
    uint8_t model_index(void) const;

    std::intptr_t sock_ = -1; /* net_sock_t, see net_compat.h */
    /* Written by the sender thread on failure, read by the emulation thread */
    std::atomic<bool> connected_{false};
    uint8_t  version_ = 0;
    std::atomic<uint8_t> sid_count_{1}; /* sender thread may lower it, see negotiate_sid_count() */
    uint8_t  wire_limit_ = 255; /* sender thread: count the server accepted */

    /* Set by query_configs() in connect(), no usable answer counts as software
     * models_[i] is the GET_CONFIG_INFO model byte */
    bool     hardware_ = false;
    uint8_t  config_count_ = 0;
    uint8_t  models_[kMaxConfigs] = { 0 };
    /* Last tune clock and model byte (0 = 6581, 1 = 8580), defaults until
     * configure_tune() runs, resent by send_chip_setup() */
    bool     ntsc_ = false;
    uint8_t  want_model_ = 0;
    uint32_t flush_cycles_ = kFlushCyclesThreshold;
    uint16_t max_frames_ = kMaxFramesBetweenFlushes;
    std::vector<uint8_t> send_buf_; /* header + payload, one send() */

    std::thread             sender_thread_;
    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedOp>    queue_;
    std::atomic<bool>       running_{false};
    /* Cycles of TRY_WRITE(_EX) requests queued or being sent, changed under queue_mutex_ */
    std::atomic<uint32_t>   inflight_cycles_{0};
    std::condition_variable room_cv_;

    /* Emulation thread only. extra_cycles_ carries a gap forward, pending_quads_ is the shared stream of all chips */
    uint32_t extra_cycles_ = 0;
    std::vector<uint8_t> pending_quads_;
    uint32_t buffered_cycles_ = 0;
    uint16_t frames_since_flush_ = 0;

    /* v5 TRY_WRITE_EX stream (FM/OPL only)
     * pending_quads_ and pending_ex_ are never both non-empty, a write flushes the other first */
    /* Per connection (cleared by connect()/disconnect()): TRY_SET_FM_OPL(1) was queued
     * fm_opl_refused_ is set by the sender thread when the server answers ERROR */
    bool     fm_opl_sent_ = false;
    /* Hardware v5 with FM in use: SID writes go over TRY_WRITE_EX with the FM writes, one stream. Emulation thread only */
    bool     ex_stream_ = false;
    std::atomic<bool> fm_opl_refused_{false};
    std::vector<uint8_t> pending_ex_;
    static constexpr size_t kMaxExEntriesPerPacket = 204; /* 5*204 = 1020 B, under NSD_MAX_PAYLOAD */
};

} /* namespace usbsid */

#endif /* _US_SID_SID_NETDEVICE_H_ */
