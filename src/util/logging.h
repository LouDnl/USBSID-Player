/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * logging.h
 * Logging of machine accesses
 * Same switches and command line names as old player, for comparing logs
 * Compiled out on the device (US_LOGGING is on for the desktop build only)
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
#ifndef _US_UTIL_LOGGING_H_
#define _US_UTIL_LOGGING_H_

#include "types.h"

#if defined(DESKTOP) && DESKTOP
#define US_LOGGING 1
#include <cstdio>
#else
#define US_LOGGING 0
#endif

namespace usbsid {

/** @brief What to print while the machine runs, names match old player */
struct LogFlags {
  bool sid_r = false;         /* -sidr  SID reads */
  bool sid_w = false;         /* -sidw  SID writes */
  bool sid_rw = false;        /* -srw   SID reads and writes */
  bool cia1_rw = false;       /* -c1rw  CIA1 reads and writes */
  bool cia2_rw = false;       /* -c2rw  CIA2 reads and writes */
  bool vic_rw = false;        /* -vrw   VIC register writes */
  bool vic_reg_reads = false; /* -vrrw  VIC register reads */
  bool read_writes = false;   /* -lrw   all CPU reads and writes */
  bool rom_rw = false;        /* -llrw  reads that come out of a ROM */
  bool pla = false;           /* -pla   banking changes */
  bool instructions = false;  /* -ins   all instructions */
  bool timers = false;        /* -tim   the timers, once a frame */
  bool memstate = false;      /* -lmem  the SID registers, once a frame */

  /** @brief True if anything is switched on */
  bool any(void) const
  {
    return sid_r || sid_w || sid_rw || cia1_rw || cia2_rw || vic_rw || vic_reg_reads ||
           read_writes || rom_rw || pla || instructions || timers || memstate;
  }
};

/* One set of switches for the process */
extern LogFlags us_log;

#if US_LOGGING
#define US_LOG_IF(flag, ...)                                    \
  do {                                                          \
    if (US_UNLIKELY(::usbsid::us_log.flag)) {                   \
      printf(__VA_ARGS__);                                      \
    }                                                           \
  } while (0)
#else
#define US_LOG_IF(flag, ...) do { } while (0)
#endif

} /* namespace usbsid */

#endif /* _US_UTIL_LOGGING_H_ */
