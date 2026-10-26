/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * pacing.h
 * Hold emulated time to real time
 * Deadlines are computed from the start of playback
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
#ifndef _US_PLAYER_PACING_H_
#define _US_PLAYER_PACING_H_

#include <cstdint>

namespace usbsid {

class Pacer
{
  public:
    /** @brief Start pacing at a given number of cycles per frame and clock */
    void start(uint32_t cycles_per_frame, uint32_t clock_hz);

    /** @brief Sleep until frame n is due */
    void wait_for_frame(uint64_t frame);

    /** @brief Make frame n due immediately and rebase the schedule on it (used after a seek) */
    void rebase(uint64_t frame);

    /** @brief Playback lag in microseconds */
    int64_t lag_us(void) const { return lag_us_; }

    /** @brief Number of schedule rebases after a long stall */
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
