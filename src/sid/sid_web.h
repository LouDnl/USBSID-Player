/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_web.h
 * The backend used when the player runs in a browser
 *
 * WASM never touches USB, WebUSB and Web MIDI live in JavaScript
 * The emulation writes cycle exact (register, value, cycles) triples into a
 * ring buffer in the WASM heap, the page drains it once a frame and forwards it
 * to the transport it uses
 *
 * Gaps are the desktop backend gaps over libusb, one cycle short of the
 * emulated gap (the access costs the board a cycle)
 * No pacer in this class, the browser steps whole frames against wall clock
 * time from an AudioWorklet or requestAnimationFrame, like the command line frame pacer
 * See sid_embedded.h for the implementation without such a clock
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
#ifndef _US_SID_SID_WEB_H_
#define _US_SID_SID_WEB_H_

#include "sid_backend.h"
#include "types.h"

/* Same clock pointer as the embedded backend (see sid_embedded.h)
 * Not needed in this file but the C API benchmark does, on the web build this is the only header a translation unit sees */
#include "sid_embedded.h" /* us_time_us_64 */

namespace usbsid {

/**
 * @brief Queue register writes for the JavaScript side to send
 *
 * Single producer, single consumer, neither writes the index of the other
 * head_ and tail_ count entries and wrap at 2^32, head_ - tail_ is the pending
 * count (unsigned arithmetic stays right across the wrap)
 *
 * Entries are four bytes [reg, value, cycles_hi, cycles_lo], the payload of
 * the USBSID-Pico CYCLED_WRITE command, the page copies a run straight into a packet
 *
 * A full ring drops the newest write and counts it, a diagnostic only
 * 8192 entries is over fifty times the busiest frame in the collection
 * Above zero means the page stopped draining
 */
class WebSidBackend final : public SidBackend
{
  public:
    /* Power of two for a mask index, 8192 entries is 32 KB and holds
     * a digi tune frame twelve times over */
    static constexpr uint32_t kRingEntries = 8192;
    static constexpr uint32_t kEntryBytes = 4;

    WebSidBackend(void) = default;

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    data_t read(addr_t reg, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override { ++flushes_; }
    void reset(void) override;

    /* Parity with the other backends
     * The C API in usplayer.cpp is written against one backend and compiled
     * against the one of the target, these have the same names
     * Pacing is the page's job, the two pacer controls do nothing */

    /** @brief Silence the chips, drop what is queued and ask the page to reset */
    void reset_hardware(void);
    /** @brief The clock the tune wants, the page passes it to the device */
    void set_clock_hz(uint32_t hz) { if (hz != 0) clock_hz_ = hz; }
    uint32_t clock_hz(void) const { return clock_hz_; }
    void resync_clock(void) {}
    void set_pacing(bool on) { (void)on; }

    uint32_t writes(void) const { return writes_; }
    uint32_t reads(void) const { return reads_; }
    uint64_t cycles_waited(void) const { return waited_; }
    /** @brief Always zero, nothing is paced in WASM */
    uint64_t cycles_paced(void) const { return 0; }

    /* The ring as the page sees it */

    const uint8_t * ring_ptr(void) const { return ring_; }
    static constexpr uint32_t ring_entries(void) { return kRingEntries; }
    uint32_t head(void) const { return head_; }
    uint32_t tail(void) const { return tail_; }
    void set_tail(uint32_t tail) { tail_ = tail; }
    uint32_t pending(void) const { return head_ - tail_; }

    /** @brief Frame boundaries crossed, when the page flushes */
    uint32_t flushes(void) const { return flushes_; }
    /** @brief Writes lost to a full ring, zero unless the page stopped */
    uint32_t drops(void) const { return drops_; }
    /** @brief Times the player asked to silence the chips */
    uint32_t resets(void) const { return resets_; }

    /** @brief Read one entry, for tests, index counts from tail */
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
    uint32_t clock_hz_ = 985248; /* PAL until a tune says otherwise */
};

/**
 * @brief The backend the player writes into on the web build
 *
 * The C exports must reach the object the player uses, its statics are private
 * to usplayer.cpp. A function local static gives both the same
 * instance and a test can still construct the class as often as it likes
 */
WebSidBackend & web_backend(void);

} /* namespace usbsid */

#endif /* _US_SID_SID_WEB_H_ */
