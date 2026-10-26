/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_embedded.cpp
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

#include "sid_embedded.h"

namespace usbsid {

namespace {

/* A gap wider than this has drained the PIO queue, the pre delay would run from a
 * standing start after the emulation already spent that time. These are paced against the clock
 * Below it the queue stays full and the pre delay is exact and free (needed for digi playback) */
constexpr uint32_t kPacedGap = 256;

/* Hardware delay of a paced write, 0 because the gap was sat out beforehand
 * and the bus operation lines up on PHI. A paced write costs the device the same as
 * an unpaced one, the cycle accounting (and its test) stays an equality */
constexpr uint16_t kPacedResidual = 0;

/* The hardware access costs a cycle that SidConfig::access_overhead already took off
 * in cycles_since_last_event(), nothing is taken off again
 * It was 1, which took the cycle off twice and put every write a cycle early
 * Kept as a named constant, the arithmetic below shows where the access cycle went */
constexpr uint16_t kHwAccessCycles = 0;

/* Emulation accounting of the access, taken off upstream by
 * SidConfig::access_overhead. Added back to make the pacer timeline the tune timeline */
constexpr uint32_t kAccountedAccess = 1;

/* Lag above this (250 ms) rebases the clock, the core was away for a long time
 * and continuing from that point beats sprinting to catch up */
constexpr uint64_t kMaxLagUs = 250000;

/* Rebase before the fixed point product nears overflow, about three hours of playing */
constexpr uint64_t kMaxDueCycles = 1ull << 40;

} /* namespace */

void EmbeddedSidBackend::set_clock_hz(uint32_t hz)
{
  if (hz == 0) return;
  clock_hz_ = hz;
  us_per_cycle_q16_ = static_cast<uint32_t>((1000000ull << 16) / hz);
  resync_clock();
}

/**
 * @brief Wait until the tune timeline catches up with the board clock
 *
 * A floor only, when the emulation is behind it returns at once and adds no lag
 */
void EmbeddedSidBackend::wait_until_due(void)
{
  if (us_time_us_64 == nullptr || !pacing_) return;
  const uint64_t now = us_time_us_64();

  if (due_cycles_ > kMaxDueCycles) { /* before the fixed point product grows */
    origin_us_ = now;
    due_cycles_ = 0;
    return;
  }

  if (us_per_cycle_q16_ == 0) {
    us_per_cycle_q16_ = static_cast<uint32_t>((1000000ull << 16) / clock_hz_);
  }

  const uint64_t due =
    origin_us_ + ((due_cycles_ * us_per_cycle_q16_) >> 16);

  if (now < due) {
    const uint32_t us = static_cast<uint32_t>(due - now);
    if (us_busy_wait_us != nullptr) {
      us_busy_wait_us(us);
    } else {
      while (us_time_us_64() < due) { /* spin */ }
    }
    return;
  }

  /* Too far behind to chase, start again from this point */
  if ((now - due) > kMaxLagUs) {
    origin_us_ = now;
    due_cycles_ = 0;
  }
}

/**
 * @brief Shorten a pre delay by the part that has gone by
 *
 * Ceiling half of the pacer (wait_until_due() is the floor), a late write
 * gets its lag taken off the pre delay and only gives back time already spent
 */
uint16_t EmbeddedSidBackend::trim_to_now(uint16_t hw)
{
  if (hw == 0 || !pacing_ || us_time_us_64 == nullptr || us_per_cycle_q16_ == 0) {
    return hw;
  }

  const uint64_t now = us_time_us_64();
  const uint64_t due = origin_us_ + ((due_cycles_ * us_per_cycle_q16_) >> 16);
  if (now <= due) return hw; /* board still ahead, chain intact */

  const uint64_t lag_us = now - due;

  /* Too far behind to chase, same bound as the floor
   * A clock that lies or a tune the core cannot keep up with would trim every
   * pre delay to nothing and the board writes back to back
   * Start again from this point like the floor does */
  if (lag_us > kMaxLagUs) {
    origin_us_ = now;
    due_cycles_ = 0;
    return hw;
  }

  ++late_writes_;
  const uint64_t lag_cycles = (lag_us << 16) / us_per_cycle_q16_;

  const uint16_t take = (lag_cycles >= hw) ? hw : static_cast<uint16_t>(lag_cycles);
  trimmed_cycles_ += take;
  return static_cast<uint16_t>(hw - take);
}

/**
 * @brief Account for a gap and return the delay for the hardware
 */
uint16_t EmbeddedSidBackend::schedule(uint16_t cycles)
{
  uint16_t hw = (cycles > kHwAccessCycles)
    ? static_cast<uint16_t>(cycles - kHwAccessCycles) : 0;

  /* The first access after a resync starts the timeline, its gap is from before
   * the tune and is not accounted, nothing to pace against yet */
  if (!paced_ && us_time_us_64 != nullptr) {
    origin_us_ = us_time_us_64();
    due_cycles_ = 0;
    paced_ = true;
    return hw;
  }

  /* Measured against the timeline before this gap (when the previous write was due) */
  hw = trim_to_now(hw);

  due_cycles_ += static_cast<uint32_t>(cycles) + kAccountedAccess;

  /* Short enough for the hardware to absorb, or no clock to pace against (every non-firmware build) */
  if (cycles <= kPacedGap || us_time_us_64 == nullptr || clock_hz_ == 0) {
    return hw;
  }

  /* Counted as sent, a paced and an unpaced gap add the same to the device timeline */
  paced_cycles_ += (cycles > kHwAccessCycles)
    ? static_cast<uint32_t>(cycles - kHwAccessCycles) : 0u;
  wait_until_due();
  return kPacedResidual;
}

void EmbeddedSidBackend::write(addr_t reg, data_t value, uint16_t cycles)
{
  ++writes_;
  const uint16_t hw = schedule(cycles);
  /* $80 and above never reach this board, chip 5+ (4 real sockets, see kMaxSids)
   * and the FM/OPL unclaimed park (kFmOplParkBase) are dropped, like the old
   * fixed $80/$90 FM markers */
  if (reg >= 0x80) return;
  if (us_cycled_write == nullptr) return;
  us_cycled_write(static_cast<uint8_t>(reg), value, hw);
}

data_t EmbeddedSidBackend::read(addr_t reg, uint16_t cycles)
{
  ++reads_;
  const uint16_t hw = schedule(cycles);
  if (us_cycled_read == nullptr || reg >= 0x80) return 0xff;
  return us_cycled_read(static_cast<uint8_t>(reg), hw);
}

/**
 * @brief Handle a gap too long for the sixteen bits a write carries
 *
 * There is no wait bus operation, the time goes to the pacer
 * (waiting the whole gap on top of the emulation time would halve playback speed)
 * Counted and dropped without a clock (every non-firmware build)
 */
void EmbeddedSidBackend::wait(uint16_t cycles)
{
  waited_ += cycles;

  if (us_time_us_64 == nullptr || clock_hz_ == 0) return;

  if (!paced_) { /* the timeline starts at this point, see schedule() */
    origin_us_ = us_time_us_64();
    due_cycles_ = 0;
    paced_ = true;
    return;
  }

  due_cycles_ += cycles;
  /* Not counted as paced, cycles_waited() covers it */
  wait_until_due();
}

void EmbeddedSidBackend::reset(void)
{
  writes_ = 0;
  reads_ = 0;
  waited_ = 0;
  paced_cycles_ = 0;
  trimmed_cycles_ = 0;
  late_writes_ = 0;
  resync_clock();
}

void EmbeddedSidBackend::reset_hardware(void)
{
  if (us_reset_sid_registers != nullptr) us_reset_sid_registers();
  if (us_reset_sid != nullptr) us_reset_sid();
}

} /* namespace usbsid */
