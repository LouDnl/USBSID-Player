/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * profile.h
 * Counters showing which chip costs the time
 * Off unless US_PROFILE is 1 (build with -DPROFILE=1), plain increments on the
 * per cycle path (read usbsid::profile, results differ a lot per tune)
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
#ifndef _US_UTIL_PROFILE_H_
#define _US_UTIL_PROFILE_H_

#include "types.h"

namespace usbsid {

#if defined(US_PROFILE) && US_PROFILE

/**
 * @brief Where the per cycle time goes
 *
 * ticks is every machine cycle, woke_* count how often the bus woke a chip.
 * cia_* count why Mos6526::cycles_to_event() refused to skip
 */
struct Profile {
  uint64_t ticks = 0;
  uint64_t woke_vic = 0, woke_cia1 = 0, woke_cia2 = 0;
  /* Why a CIA could not be skipped */
  uint64_t cia_pipeline = 0;  /* interrupt or PB pulse in flight */
  uint64_t cia_cnt = 0;       /* the CNT input moved */
  uint64_t cia_serial = 0;    /* the shift register is busy */
  uint64_t cia_timer_a = 0, cia_timer_b = 0;
  uint64_t cia_free = 0;      /* could be skipped */

  /* Why timer B refused (the four conditions in Timer::quiet_clocks()) */
  uint64_t cia_b_transition = 0;  /* mid transition, next(state) != state */
  uint64_t cia_b_stage = 0;       /* a stage that changes something part way */
  uint64_t cia_b_notphi2 = 0;     /* counting something other than phi2 */
  uint64_t cia_b_zero = 0;        /* counter already at zero */
  /* OR of the bits that differed between state and next(state) */
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
