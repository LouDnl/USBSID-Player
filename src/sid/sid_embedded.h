/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_embedded.h
 * The backend used when the player *is* the firmware.
 *
 * On the desktop the register writes travel over USB to a USBSID-Pico. On the
 * device there is no USB in the middle: the firmware's own bus code drives the
 * SID directly, and it already takes a cycle delay with every access. So this
 * backend is a thin adapter onto two firmware functions, and the cycle exact
 * deltas the emulation produces are handed straight to the hardware.
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
#ifndef _US_SID_SID_EMBEDDED_H_
#define _US_SID_SID_EMBEDDED_H_

#include "sid_backend.h"
#include "types.h"

/* The firmware side of the bridge. These are USBSID-Pico's own bus functions,
 * declared weak so this file links anywhere: in a build without the firmware
 * they resolve to null and the backend goes quiet instead of failing to link.
 * That is also what lets the tests substitute their own definitions. */
extern "C" {
  void cycled_write_operation(uint8_t address, uint8_t data, uint16_t cycles)
    __attribute__((weak));
  uint8_t cycled_read_operation(uint8_t address, uint16_t cycles)
    __attribute__((weak));
  void reset_sid(void) __attribute__((weak));
  void reset_sid_registers(void) __attribute__((weak));
  /* the Pico SDK's microsecond clock, the only real time this backend has */
  uint64_t time_us_64(void) __attribute__((weak));
  /* Optional: block for this long. Nothing defines it on the device, where the
   * fallback spin on time_us_64 is what a busy wait would have been anyway. It
   * exists so a test can run the pacer against a clock it controls, which is
   * the only way to measure device tempo without a device. */
  void usplayer_busy_wait_us(uint32_t us) __attribute__((weak));
}

namespace usbsid {

/**
 * @brief Sends register writes to the SID bus of the firmware we run inside.
 *
 * `cycled_write_operation` takes the gap since the previous access and waits
 * it out *before* putting the value on the bus. For writes that come close
 * together that is both exact and free: the PIO still has earlier writes
 * queued, so it is sitting out one gap while the emulation produces the next,
 * and the hardware, being the slower of the two, sets the pace.
 *
 * A wide gap breaks that. The queue drains during it, so by the time this
 * backend is handed the gap it has already spent the host time to emulate it,
 * and the hardware then spends the same gap again from a standing start. The
 * two are serialised and the frame takes half again as long as it should.
 * A tune with a play routine and an idle loop is entirely made of such gaps:
 * one write burst and then nineteen thousand cycles of nothing, every frame.
 *
 * So gaps wide enough to have drained the queue are not sent to the hardware
 * at all. They are sat out here instead, against the board's own clock, and
 * only the time that has *not* already gone into emulating them is waited.
 * The hardware gets a token delay and the write. Everything narrower keeps
 * the exact pre delay it had, which is what digi playback depends on.
 */
class EmbeddedSidBackend final : public SidBackend
{
  public:
    EmbeddedSidBackend(void) = default;

    void write(data_t reg, data_t value, uint16_t cycles) override US_RAM_ATTR;
    data_t read(data_t reg, uint16_t cycles) override US_RAM_ATTR;
    void wait(uint16_t cycles) override US_RAM_ATTR;
    void flush(void) override {}
    void reset(void) override;

    /** @brief Whether the firmware bus functions are actually linked in. */
    static bool hardware_present(void)
    {
      return cycled_write_operation != nullptr;
    }

    /** @brief Reset the chips themselves, not just this object's counters. */
    void reset_hardware(void);

    /** @brief The SID clock the board is running at, for waiting in real time. */
    void set_clock_hz(uint32_t hz);
    uint32_t clock_hz(void) const { return clock_hz_; }

    /**
     * @brief Start the pacer's clock from now.
     *
     * The pacer measures a tune's timeline from the first access it sees. That
     * has to be the first access of the tune and not of the KERNAL boot before
     * it, so the player calls this when playback starts.
     */
    void resync_clock(void) { paced_ = false; due_cycles_ = 0; }

    /**
     * @brief Whether wide gaps are sat out in real time.
     *
     * On for playback, which is the whole point. Off while a tune is being
     * set up: booting a machine and loading a program is not part of anyone's
     * timeline, nobody is listening to it, and pacing it only makes the wait
     * before the first note longer than the work actually takes. Switching it
     * back on restarts the clock.
     */
    void set_pacing(bool on) { pacing_ = on; if (on) resync_clock(); }

    uint32_t writes(void) const { return writes_; }
    uint32_t reads(void) const { return reads_; }
    uint64_t cycles_waited(void) const { return waited_; }
    /** @brief Cycles the pacer has sat out itself rather than sending them. */
    uint64_t cycles_paced(void) const { return paced_cycles_; }
    /** @brief Cycles taken off pre delays to make up time already lost. */
    uint64_t cycles_trimmed(void) const { return trimmed_cycles_; }
    /** @brief Writes that found the timeline already behind. */
    uint32_t late_writes(void) const { return late_writes_; }

  private:
    /** @brief Account for a gap and decide what the hardware should be told. */
    uint16_t schedule(uint16_t cycles) US_RAM_ATTR;
    /** @brief Block until the accounted timeline catches up with the clock. */
    void wait_until_due(void) US_RAM_ATTR;
    /** @brief Shorten a pre delay by however much of it has already gone by. */
    uint16_t trim_to_now(uint16_t hw) US_RAM_ATTR;

    uint32_t writes_ = 0;
    uint32_t reads_ = 0;
    uint64_t waited_ = 0;
    uint32_t clock_hz_ = 985248; /* PAL, until a tune says otherwise */

    /* The pacer. `due_cycles_` is the tune's own timeline measured from
     * `origin_us_` on the board's clock, and the microseconds one cycle takes
     * is kept in 16.16 fixed point so putting the two together is a multiply
     * and a shift rather than a division per access. */
    uint64_t origin_us_ = 0;
    uint64_t due_cycles_ = 0;
    uint64_t paced_cycles_ = 0;
    uint64_t trimmed_cycles_ = 0;
    uint32_t late_writes_ = 0;
    uint32_t us_per_cycle_q16_ = 0;
    bool paced_ = false;      /* whether origin_us_ has been set yet */
    bool pacing_ = true;      /* whether wide gaps are waited at all */
};

} /* namespace usbsid */

#endif /* _US_SID_SID_EMBEDDED_H_ */
