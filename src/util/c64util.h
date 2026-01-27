/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * util.h
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025-2026 LouD
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

#ifndef _C64_UTIL_H
#define _C64_UTIL_H

#include <cstdio>
#include <cstdint>

#if EMBEDDED
#include "pico/stdlib.h"
#define __us_not_in_flash_func __attribute__((section(".time_critical")))
#endif

#if DESKTOP
#define __in_flash(name)
#endif

#if DESKTOP
  #define MOSLOG(...) fprintf (stdout,__VA_ARGS__)
  #ifndef NDEBUG
    #define MOSDBG(...) fprintf (stdout,__VA_ARGS__)
  #else
    #define MOSDBG(...) do {} while (0)
  #endif
#elif EMBEDDED
  #include <logging.h>
  #define MOSLOG(...) _US_DBG(__VA_ARGS__)
  #if defined(EMUDEBUG)
    #define MOSDBG(...) _US_DBG(__VA_ARGS__)
  #else
    #define MOSDBG(...) do {} while (0)
  #endif
#endif

/**
 * C++17 removed the register keyword, so we define it to nothing so that
 * it doesn't cause errors.
 */
#if __cplusplus >= 201703L
#define register
#endif


#ifndef count_of
#define count_of(a) (sizeof(a)/sizeof((a)[0]))
#endif

#ifndef MAX
#define MAX(a, b) ((a)>(b)?(a):(b))
#endif

#ifndef MIN
#define MIN(a, b) ((b)>(a)?(a):(b))
#endif

/* https://www.reddit.com/r/cpp/comments/14n8miq/unifying_builtin_expect_and_likely/ */
/* For non C64 */
#define __likely(x) (__builtin_expect(!!(x), 1))
#define __unlikely(x) (__builtin_expect(!!(x), 0))
/* For C64 */
#define _MOS_LIKELY(x) (__builtin_expect(!!(x), 1))
#define _MOS_UNLIKELY(x) (__builtin_expect(!!(x), 0))

static inline bool ISSET_BIT(unsigned int v, unsigned int b) {
  return (v & (1U << b)) != 0;
}

static inline uint_least8_t SET_BIT(unsigned int v, unsigned int b) {
  return v |= (1 << b);
}

#endif /* _C64_UTIL_H */
