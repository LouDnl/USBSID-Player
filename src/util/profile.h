/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback, for embedding on RP2350 (Pico2), and in a browser.
 *
 * profile.h
 * Counters for finding out which chip is costing the time.
 *
 * Off unless US_PROFILE is 1, in which case they are plain increments on the
 * per cycle path. They exist because "the emulation is too slow" is not an
 * actionable statement and the answer turned out to be different for every
 * tune: measured with these, `rsid/Microsleep_tune_10.sid` wakes the VIC on
 * 31.6% of cycles and barely touches the CIAs, while `prg/Musik_Run_Stop.prg`
 * wakes CIA1 on 81.6% and barely touches the VIC. Guessing from one tune got
 * that wrong.
 *
 * Build with -DPROFILE=1 (see CMakeLists.txt) and read them with
 * `usbsid::profile`. Nothing in a shipping build refers to them.
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
#ifndef _US_UTIL_PROFILE_H_
#define _US_UTIL_PROFILE_H_

#include "types.h"

namespace usbsid {

#if defined(US_PROFILE) && US_PROFILE

/**
 * @brief Where the per cycle time goes.
 *
 * `ticks` is every machine cycle. The `woke_*` counters are how often the bus
 * decided a chip had something to do, so `woke_vic / ticks` is the fraction of
 * cycles the VIC was caught up on. The `cia_*` counters break down why
 * `Mos6526::cycles_to_event()` refused to skip, which is the difference
 * between a chip that is genuinely busy and one that is being woken for
 * nothing.
 */
struct Profile {
  uint64_t ticks = 0;
  uint64_t woke_vic = 0, woke_cia1 = 0, woke_cia2 = 0;
  /* why a CIA could not be skipped */
  uint64_t cia_pipeline = 0;  /* an interrupt or a PB pulse in flight */
  uint64_t cia_cnt = 0;       /* the CNT input moved */
  uint64_t cia_serial = 0;    /* the shift register is busy */
  uint64_t cia_timer_a = 0, cia_timer_b = 0;
  uint64_t cia_free = 0;      /* it could be skipped */

  /* Why timer B in particular said "not a moment". It is the single largest
   * reason a CIA bound tune cannot be skipped, so knowing which of the four
   * conditions in Timer::quiet_clocks() is the one that fires decides what a
   * lookahead would have to handle. */
  uint64_t cia_b_transition = 0;  /* mid transition, next(state) != state */
  uint64_t cia_b_stage = 0;       /* a stage that changes something part way */
  uint64_t cia_b_notphi2 = 0;     /* counting something other than phi2 */
  uint64_t cia_b_zero = 0;        /* counter already at zero */
  /* OR of every bit that differed between state and next(state), so the
   * unstable bits name themselves instead of being guessed at. */
  uint64_t cia_b_diffbits = 0;
  uint64_t cia_b_statebits = 0;   /* OR of the states seen while refusing */

  void clear(void) { *this = Profile(); }
};

extern Profile profile;

#define US_PROF(field) (++::usbsid::profile.field)

#else

#define US_PROF(field) ((void)0)

#endif /* US_PROFILE */

} /* namespace usbsid */

#endif /* _US_UTIL_PROFILE_H_ */
