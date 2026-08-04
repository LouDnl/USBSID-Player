/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_web.h
 * The backend used when the player runs in a browser.
 *
 * WASM never touches USB. WebUSB and Web MIDI live in JavaScript, so the
 * emulation writes cycle exact (register, value, cycles) triples into a ring
 * buffer in the WASM heap and the page drains it once a frame and forwards it
 * to whichever transport it is using. One seam, two transports, and the
 * hardware half stays in the language that can actually reach the hardware.
 *
 * The gaps are the same gaps the desktop backend sends over libusb, one cycle
 * short of the emulated gap because performing the access costs the board a
 * cycle of its own. What is *not* here is a pacer: the browser paces itself,
 * stepping whole frames against wall clock time from an AudioWorklet or from
 * requestAnimationFrame, exactly as the command line player's frame pacer
 * does. See sid_embedded.h for the case where there is no such clock and the
 * cycle deltas are the only timing there is.
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
#ifndef _US_SID_SID_WEB_H_
#define _US_SID_SID_WEB_H_

#include "sid_backend.h"
#include "types.h"

/* The same weak clock the embedded backend uses. Nothing in this file needs
 * it, but the C API's benchmark does, and on the web build that is the only
 * header declaring it. src/host/web_api.cpp defines it from the browser's own
 * clock, so it is a real function there rather than an unresolved weak one. */
extern "C" {
  uint64_t time_us_64(void) __attribute__((weak));
}

namespace usbsid {

/**
 * @brief Queues register writes for the JavaScript side to send.
 *
 * Single producer, single consumer: the emulation writes, the page reads, and
 * neither ever writes the other's index. `head_` and `tail_` count entries and
 * are allowed to wrap at 2^32; what is pending is `head_ - tail_`, which stays
 * right across the wrap because it is unsigned arithmetic.
 *
 * Entries are four bytes, `[reg, value, cycles_hi, cycles_lo]`, which is the
 * payload of USBSID-Pico's CYCLED_WRITE command. The page can therefore copy a
 * run of them straight into a packet without unpacking anything.
 *
 * A full ring drops the newest write and counts it. That is a diagnostic, not
 * a recovery: 8192 entries is more than fifty times the busiest frame in the
 * collection, so anything above zero means the page has stopped draining.
 */
class WebSidBackend final : public SidBackend
{
  public:
    /* Power of two, so the index is a mask rather than a division. 8192
     * entries is 32 KB and holds a digi tune's frame twelve times over. */
    static constexpr uint32_t kRingEntries = 8192;
    static constexpr uint32_t kEntryBytes = 4;

    WebSidBackend(void) = default;

    void write(data_t reg, data_t value, uint16_t cycles) override;
    data_t read(data_t reg, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override { ++flushes_; }
    void reset(void) override;

    /* ---- parity with the other backends -------------------------------- *
     * The C API in src/api/usplayer.cpp is written against one backend and
     * compiled against whichever one the target has, so these exist here with
     * the same names. Pacing is the page's job, so the two pacer controls are
     * accepted and do nothing. */

    /** @brief Silence the chips: drop what is queued and ask the page to reset. */
    void reset_hardware(void);
    /** @brief The clock the tune wants, which the page passes to the device. */
    void set_clock_hz(uint32_t hz) { if (hz != 0) clock_hz_ = hz; }
    uint32_t clock_hz(void) const { return clock_hz_; }
    void resync_clock(void) {}
    void set_pacing(bool on) { (void)on; }

    uint32_t writes(void) const { return writes_; }
    uint32_t reads(void) const { return reads_; }
    uint64_t cycles_waited(void) const { return waited_; }
    /** @brief Always zero here: nothing is paced in WASM. */
    uint64_t cycles_paced(void) const { return 0; }

    /* ---- the ring, as the page sees it --------------------------------- */

    const uint8_t * ring_ptr(void) const { return ring_; }
    static constexpr uint32_t ring_entries(void) { return kRingEntries; }
    uint32_t head(void) const { return head_; }
    uint32_t tail(void) const { return tail_; }
    void set_tail(uint32_t tail) { tail_ = tail; }
    uint32_t pending(void) const { return head_ - tail_; }

    /** @brief Frame boundaries crossed, which is when the page flushes. */
    uint32_t flushes(void) const { return flushes_; }
    /** @brief Writes lost to a full ring. Zero unless the page stopped. */
    uint32_t drops(void) const { return drops_; }
    /** @brief Times the player asked for the chips to be silenced. */
    uint32_t resets(void) const { return resets_; }

    /** @brief Read one entry, for the tests. Index counts from `tail`. */
    void entry(uint32_t index, data_t & reg, data_t & value,
               uint16_t & cycles) const;

  private:
    uint8_t ring_[kRingEntries * kEntryBytes] = {};
    uint32_t head_ = 0;   /* entries written by the emulation */
    uint32_t tail_ = 0;   /* entries taken by the page */

    uint32_t writes_ = 0;
    uint32_t reads_ = 0;
    uint32_t flushes_ = 0;
    uint32_t drops_ = 0;
    uint32_t resets_ = 0;
    uint64_t waited_ = 0;
    uint32_t clock_hz_ = 985248; /* PAL, until a tune says otherwise */
};

/**
 * @brief The one the player writes into on the web build.
 *
 * The C exports have to reach the same object the player is using, and the
 * player's own statics are private to src/api/usplayer.cpp. A function local
 * static is the smallest thing that gives both of them the same instance,
 * while leaving the class free to be constructed as many times as a test
 * likes.
 */
WebSidBackend & web_backend(void);

} /* namespace usbsid */

#endif /* _US_SID_SID_WEB_H_ */
