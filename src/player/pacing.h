/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * pacing.h
 * Keeping emulated time and real time together.
 *
 * The emulation runs as fast as it can, which on a desktop is around a
 * hundred times too fast. Something has to hold it back to the tune's own
 * frame rate, and it has to do so without drifting: sleeping "a frame" every
 * frame accumulates error, so the deadline is always computed from the start
 * of playback.
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
#ifndef _US_PLAYER_PACING_H_
#define _US_PLAYER_PACING_H_

#include <cstdint>

namespace usbsid {

class Pacer
{
  public:
    /** @brief Start pacing at a given number of cycles per frame and clock. */
    void start(uint32_t cycles_per_frame, uint32_t clock_hz);

    /** @brief Sleep until frame number n is due. */
    void wait_for_frame(uint64_t frame);

    /** @brief How far behind real time playback has fallen, in microseconds. */
    int64_t lag_us(void) const { return lag_us_; }

    /** @brief How many times the schedule had to be given up on and rebased. */
    uint32_t resyncs(void) const { return resyncs_; }

    double frame_rate(void) const { return frame_rate_; }

  private:
    uint64_t start_us_ = 0;
    uint64_t base_frame_ = 0;
    uint32_t resyncs_ = 0;
    double frame_us_ = 20000.0;
    double frame_rate_ = 50.0;
    int64_t lag_us_ = 0;
};

} /* namespace usbsid */

#endif /* _US_PLAYER_PACING_H_ */
