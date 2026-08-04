/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * logging.h
 * Watching what the machine does, one access at a time.
 *
 * The same set of switches player-repo has, and the same command line names
 * for them, because the two are going to be compared against each other for a
 * while yet and a diff of two differently shaped logs is no use to anybody.
 *
 * All of it compiles away on the device: `US_LOGGING` is only on for the
 * desktop build, and with it off every call site becomes nothing at all. With
 * it on but the switch off, a call site is one predicted-not-taken branch,
 * which the eighteen times realtime the desktop runs at can afford.
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

/**
 * @brief What to print while the machine runs.
 *
 * The names match player-repo's variables, and the command line switches that
 * set them match its arguments.
 */
struct LogFlags {
  bool sid_rw = false;        /* -srw   SID reads and writes */
  bool cia1_rw = false;       /* -c1rw  CIA1 reads and writes */
  bool cia2_rw = false;       /* -c2rw  CIA2 reads and writes */
  bool vic_rw = false;        /* -vrw   VIC register writes */
  bool vic_reg_reads = false; /* -vrrw  VIC register reads */
  bool read_writes = false;   /* -lrw   every CPU read and write */
  bool rom_rw = false;        /* -llrw  reads that come out of a ROM */
  bool pla = false;           /* -pla   banking changes */
  bool instructions = false;  /* -ins   every instruction */
  bool timers = false;        /* -tim   the timers, once a frame */
  bool memstate = false;      /* -lmem  the SID registers, once a frame */

  /** @brief True when anything at all is switched on. */
  bool any(void) const
  {
    return sid_rw || cia1_rw || cia2_rw || vic_rw || vic_reg_reads ||
           read_writes || rom_rw || pla || instructions || timers || memstate;
  }
};

/* One set of switches for the process. A player is one machine, and this is a
 * debugging aid rather than something to thread through every constructor. */
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
