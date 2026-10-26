/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_embedded.h
 * The backend used when the player is the firmware
 *
 * On the desktop register writes travel over USB to a USBSID-Pico
 * On the device the firmware bus code drives the SID directly and takes a
 * cycle delay with every access. This backend is a thin adapter onto two
 * firmware functions, the cycle exact deltas go straight to the hardware
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
#ifndef _US_SID_SID_EMBEDDED_H_
#define _US_SID_SID_EMBEDDED_H_

#include "sid_backend.h"
#include "types.h"

/* The firmware side of the bridge, USBSID-Pico bus functions reached through pointers
 * Weak symbols are ELF only, macOS/Windows builds fail or collide on them
 * Under EMBEDDED these are initialised to the real addresses in sid_embedded.cpp
 * Otherwise null and a test assigns them */
extern "C" {
  extern void (*us_cycled_write)(uint8_t address, uint8_t data, uint16_t cycles);
  extern uint8_t (*us_cycled_read)(uint8_t address, uint16_t cycles);
  extern void (*us_reset_sid)(void);
  extern void (*us_reset_sid_registers)(void);
  /* Pico SDK microsecond clock, the only real time this backend has */
  extern uint64_t (*us_time_us_64)(void);
  /* Optional block for this long, nothing sets it on the device (the spin on
   * us_time_us_64 is the busy wait), a test sets it to run the pacer against a controlled clock */
  extern void (*us_busy_wait_us)(uint32_t us);
}

namespace usbsid {

/**
 * @brief Send register writes to the SID bus of the firmware it runs inside
 *
 * cycled_write_operation takes the gap since the previous access and waits it
 * out before putting the value on the bus. For writes close together this is
 * exact and free, the PIO has earlier writes queued and sits out one gap while
 * the emulation produces the following one. The hardware is the slower of the two and sets the pace
 *
 * A wide gap drains the queue. The host time to emulate it is already spent
 * when the gap arrives and the hardware spends the same gap again from a
 * standing start, the frame takes half again as long as it should
 * A tune with a play routine and an idle loop is made of such gaps, one write
 * burst and then nineteen thousand cycles of nothing, every frame
 *
 * Gaps wide enough to drain the queue are not sent to the hardware
 * They are sat out in this class against the board clock, only the time not
 * already spent emulating them is waited
 * The hardware gets a token delay and the write
 * Narrower gaps keep the exact pre delay (digi playback depends on it)
 */
class EmbeddedSidBackend final : public SidBackend
{
  public:
    EmbeddedSidBackend(void) = default;

    void write(addr_t reg, data_t value, uint16_t cycles) override US_RAM_ATTR;
    data_t read(addr_t reg, uint16_t cycles) override US_RAM_ATTR;
    void wait(uint16_t cycles) override US_RAM_ATTR;
    void flush(void) override {}
    void reset(void) override;

    /** @brief Check if the firmware bus functions are linked in */
    static bool hardware_present(void)
    {
      return us_cycled_write != nullptr;
    }

    /** @brief Reset the chips and the counters */
    void reset_hardware(void);

    /** @brief Set the SID clock of the board, for waiting in real time */
    void set_clock_hz(uint32_t hz);
    uint32_t clock_hz(void) const { return clock_hz_; }

    /**
     * @brief Start the pacer clock from the current time
     *
     * The pacer measures the tune timeline from the first access it sees
     * That must be the first access of the tune (the KERNAL boot before it is skipped)
     * Called by the player when playback starts
     */
    void resync_clock(void) { paced_ = false; due_cycles_ = 0; }

    /**
     * @brief Sit out wide gaps in real time
     *
     * On for playback. Off while a tune is set up, booting and loading is no
     * part of the timeline and pacing it only lengthens the wait before the first note
     * Switching on restarts the clock
     */
    void set_pacing(bool on) { pacing_ = on; if (on) resync_clock(); }

    uint32_t writes(void) const { return writes_; }
    uint32_t reads(void) const { return reads_; }
    uint64_t cycles_waited(void) const { return waited_; }
    /** @brief Cycles the pacer sat out */
    uint64_t cycles_paced(void) const { return paced_cycles_; }
    /** @brief Cycles taken off pre delays to make up lost time */
    uint64_t cycles_trimmed(void) const { return trimmed_cycles_; }
    /** @brief Writes that found the timeline behind */
    uint32_t late_writes(void) const { return late_writes_; }

  private:
    /** @brief Account for a gap and decide what the hardware is told */
    uint16_t schedule(uint16_t cycles) US_RAM_ATTR;
    /** @brief Block until the timeline catches up with the clock */
    void wait_until_due(void) US_RAM_ATTR;
    /** @brief Shorten a pre delay by the part that has gone by */
    uint16_t trim_to_now(uint16_t hw) US_RAM_ATTR;

    uint32_t writes_ = 0;
    uint32_t reads_ = 0;
    uint64_t waited_ = 0;
    uint32_t clock_hz_ = 985248; /* PAL until a tune says otherwise */

    /* The pacer, due_cycles_ is the tune timeline measured from origin_us_ on the board clock
     * Microseconds per cycle is kept in 16.16 fixed point (multiply and shift, no division per access) */
    uint64_t origin_us_ = 0;
    uint64_t due_cycles_ = 0;
    uint64_t paced_cycles_ = 0;
    uint64_t trimmed_cycles_ = 0;
    uint32_t late_writes_ = 0;
    uint32_t us_per_cycle_q16_ = 0;
    bool paced_ = false;      /* origin_us_ set */
    bool pacing_ = true;      /* wide gaps are waited */
};

} /* namespace usbsid */

#endif /* _US_SID_SID_EMBEDDED_H_ */
