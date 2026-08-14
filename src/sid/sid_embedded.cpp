/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_embedded.cpp
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

#include "sid_embedded.h"

namespace usbsid {

namespace {

/* A gap wider than this has drained the PIO's queue, so its pre delay would
 * be served from a standing start, after the emulation has already spent the
 * same time producing it. Those are paced against the clock instead. Below it
 * the writes are close enough together that the queue stays full and the pre
 * delay is both exact and free, which is what digi playback needs. */
constexpr uint32_t kPacedGap = 256;

/* What a paced write asks the hardware for. Nothing: the gap has been sat out
 * here already, and the bus operation lines itself up on PHI on its own. It is
 * zero rather than a token cycle or two so that a paced write costs the device
 * exactly what an unpaced one does, which keeps the cycle accounting, and the
 * test that checks it, an equality rather than an approximation. */
constexpr uint16_t kPacedResidual = 0;

/* Performing the access costs the hardware a cycle of its own, and that cycle
 * has already been taken off upstream, by SidConfig::access_overhead in
 * cycles_since_last_event(). So nothing is taken off again here: what arrives
 * is the pre delay the bus operation should sit out and nothing more. This was
 * one, which took the same cycle off twice and put every write a cycle early;
 * across a frame of a write heavy tune that is the tempo. Kept as a named
 * constant rather than deleted so the arithmetic below still says out loud
 * where the access cycle went. */
constexpr uint16_t kHwAccessCycles = 0;

/* The emulation's own accounting of the access, taken off upstream by
 * SidConfig::access_overhead. Adding it back is what makes the pacer's
 * timeline the tune's timeline. */
constexpr uint32_t kAccountedAccess = 1;

/* Past this much lag the clock is rebased rather than chased. Something took
 * the core away for a long time; carrying on from here is better than
 * sprinting through a quarter second of catching up. */
constexpr uint64_t kMaxLagUs = 250000;

/* Rebase before the fixed point product gets anywhere near overflowing. This
 * is about three hours of playing. */
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
 * @brief Wait until the tune's own timeline catches up with the board's clock.
 *
 * Only ever a floor. If the emulation is behind, which is the whole point of
 * measuring, this returns at once and nothing is added to the lag.
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

  /* Too far behind to be worth chasing: start again from here. */
  if ((now - due) > kMaxLagUs) {
    origin_us_ = now;
    due_cycles_ = 0;
  }
}

/**
 * @brief Shorten a pre delay by however much of it has already gone by.
 *
 * The other half of the pacer, and the half it was missing. `wait_until_due()`
 * is a floor: when the emulation is ahead it holds it back. Nothing was a
 * ceiling. When the emulation is behind, the write is issued late *and* still
 * asks the board for the whole gap, so the lateness is served twice and the
 * write lands late for good. The board cannot give it back: a pre delay starts
 * when the DMA fires, not when the previous write finished, so once the state
 * machine's queue has drained the gap is measured from a standing start.
 *
 * That is the difference between this player and the desktop one. Over USB the
 * firmware's own buffer keeps the board a long way ahead, the queue never
 * drains, and the delays chain exactly. Here the emulation feeds the bus just
 * in time from the same core that runs the rest of usbsid.c, so anything that
 * takes the core away, a frame boundary or a dense burst the emulation cannot
 * keep up with, empties the queue. Measured on the simulated board that is six
 * or seven times a frame, each one putting a write about 130 cycles late. The
 * tempo survives, because the floor still holds the average; the individual
 * writes do not, and a digi made of individually late writes is a crackle.
 *
 * So take the lost time off the next pre delay instead. Bounded by the delay
 * itself: this only ever gives back time already spent, never asks the board
 * to write in the past.
 */
uint16_t EmbeddedSidBackend::trim_to_now(uint16_t hw)
{
  if (hw == 0 || !pacing_ || us_time_us_64 == nullptr || us_per_cycle_q16_ == 0) {
    return hw;
  }

  const uint64_t now = us_time_us_64();
  const uint64_t due = origin_us_ + ((due_cycles_ * us_per_cycle_q16_) >> 16);
  if (now <= due) return hw; /* the board is still ahead, the chain is intact */

  const uint64_t lag_us = now - due;

  /* Too far behind to be worth chasing, the same bound the floor uses. Without
   * this a clock that is not telling the truth, or a tune the core simply
   * cannot keep up with, trims every pre delay to nothing and the board writes
   * back to back with no timing left in it at all. Start again from here
   * instead, which is what the floor does with the same lag. */
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
 * @brief Account for a gap, and return the delay the hardware should be given.
 */
uint16_t EmbeddedSidBackend::schedule(uint16_t cycles)
{
  uint16_t hw = (cycles > kHwAccessCycles)
    ? static_cast<uint16_t>(cycles - kHwAccessCycles) : 0;

  /* The first access after a resync is where the timeline starts. Its own gap
   * is whatever came before the tune and is not part of it, so it is not
   * accounted and there is nothing yet to pace against. */
  if (!paced_ && us_time_us_64 != nullptr) {
    origin_us_ = us_time_us_64();
    due_cycles_ = 0;
    paced_ = true;
    return hw;
  }

  /* Measured against where the timeline stood *before* this gap, which is when
   * the previous write was due to land. */
  hw = trim_to_now(hw);

  due_cycles_ += static_cast<uint32_t>(cycles) + kAccountedAccess;

  /* Short enough for the hardware to absorb, or there is no clock to pace
   * against, which is every build that is not the firmware. */
  if (cycles <= kPacedGap || us_time_us_64 == nullptr || clock_hz_ == 0) {
    return hw;
  }

  /* Counted as what would have been sent, so that a paced gap and an unpaced
   * one contribute the same to the device's timeline. */
  paced_cycles_ += (cycles > kHwAccessCycles)
    ? static_cast<uint32_t>(cycles - kHwAccessCycles) : 0u;
  wait_until_due();
  return kPacedResidual;
}

void EmbeddedSidBackend::write(data_t reg, data_t value, uint16_t cycles)
{
  ++writes_;
  const uint16_t hw = schedule(cycles);
  /* $80 and above are not SID registers: they are the FM/OPL addresses that no
   * chip claimed, and only a transport that carries FM itself can use them. This
   * one talks to a board, so it drops them, which is what happened before they
   * were forwarded at all. */
  if (reg >= 0x80) return;
  if (us_cycled_write == nullptr) return;
  us_cycled_write(reg, value, hw);
}

data_t EmbeddedSidBackend::read(data_t reg, uint16_t cycles)
{
  ++reads_;
  const uint16_t hw = schedule(cycles);
  if (us_cycled_read == nullptr) return 0xff;
  return us_cycled_read(reg, hw);
}

/**
 * @brief A gap too long to fit in the sixteen bits a write carries.
 *
 * There is no "wait and do nothing" bus operation to send, so the time goes
 * to the pacer, the same as any other gap too wide for the hardware queue to
 * hide. Which means the wait is for the time that has not already been spent
 * emulating the gap, not for the whole of it: sitting out the full sixty five
 * milliseconds on top of the host time it took to produce them is how a tune
 * ends up playing at half speed.
 *
 * With no clock available, which is every build that is not the firmware, the
 * gap is counted and dropped.
 */
void EmbeddedSidBackend::wait(uint16_t cycles)
{
  waited_ += cycles;

  if (us_time_us_64 == nullptr || clock_hz_ == 0) return;

  if (!paced_) { /* the timeline starts here, see schedule() */
    origin_us_ = us_time_us_64();
    due_cycles_ = 0;
    paced_ = true;
    return;
  }

  due_cycles_ += cycles;
  /* not counted as paced: cycles_waited() already accounts for it */
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
