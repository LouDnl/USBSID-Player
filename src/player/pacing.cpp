/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * pacing.cpp
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

#include <ctime>

#include "pacing.h"

namespace usbsid {

namespace {

uint64_t now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

void sleep_us(uint64_t us)
{
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(us / 1000000ull);
  ts.tv_nsec = static_cast<long>((us % 1000000ull) * 1000ull);
  nanosleep(&ts, nullptr);
}

} /* namespace */

void Pacer::start(uint32_t cycles_per_frame, uint32_t clock_hz)
{
  frame_us_ = (1000000.0 * static_cast<double>(cycles_per_frame)) /
              static_cast<double>(clock_hz);
  frame_rate_ = 1000000.0 / frame_us_;
  start_us_ = now_us();
  base_frame_ = 0;
  resyncs_ = 0;
  lag_us_ = 0;
}

void Pacer::rebase(uint64_t frame)
{
  start_us_ = now_us();
  base_frame_ = frame;
  lag_us_ = 0;
}

void Pacer::wait_for_frame(uint64_t frame)
{
  /* The deadline is measured from a fixed point, never from the previous
   * frame, so an ordinary slow frame does not push every later one back with
   * it and the error cannot accumulate. */
  const uint64_t due = start_us_ +
    static_cast<uint64_t>(frame_us_ * static_cast<double>(frame - base_frame_));
  const uint64_t now = now_us();

  if (now < due) {
    sleep_us(due - now);
    lag_us_ = 0;
    return;
  }

  lag_us_ = static_cast<int64_t>(now - due);

  /* Falling a long way behind is different from being a little late, and
   * catching up is the wrong answer to it. Running frames back to back until
   * the schedule is met sends the hardware a burst of writes at many times
   * the rate it can take them, and the ring buffer between here and the
   * device has no room for that: what comes out is a crackle. Whatever caused
   * the stall has already cost that time, so the schedule is moved instead
   * and playback carries on from here. */
  constexpr int64_t kResyncThresholdUs = 250000; /* about twelve frames */

  if (lag_us_ > kResyncThresholdUs) {
    start_us_ = now;
    base_frame_ = frame;
    ++resyncs_;
  }
}

} /* namespace usbsid */
