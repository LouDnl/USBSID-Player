/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * types.h
 * Basic types, branch hints and memory placement attributes.
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
#ifndef _US_CORE_TYPES_H_
#define _US_CORE_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace usbsid {

/* Free running PHI2 cycle counter, 64 bit (32 bit wraps after ~72 min at ~1 MHz) */
using cycle_t = uint_fast64_t;

/* C64 bus types */
using addr_t = uint16_t; /* 16 bit address bus */
using data_t = uint8_t;  /* 8 bit data bus */
using reg_t  = uint8_t;  /* chip register index */

/* Sub cycle step counter inside one instruction (0..7) */
using ustep_t = uint_fast8_t;

/* Branch hints for rarely taken checks (raster match, badline, timer underflow) */
#define US_LIKELY(x)   __builtin_expect(!!(x), 1)
#define US_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Memory placement
 * RP2350 per cycle code and RAM array must stay out of flash (XIP stalls break timing)
 * No-ops on desktop, place after the declarator: void run(cycle_t n) US_RAM_ATTR;
 */
#if defined(EMBEDDED) && EMBEDDED
#define US_RAM_ATTR __attribute__((section(".time_critical.usbsid")))
#define US_RAM_DATA __attribute__((section(".data.usbsid")))
#else
#define US_RAM_ATTR
#define US_RAM_DATA
#endif

/* Force inline for small per cycle accessors */
#define US_ALWAYS_INLINE inline __attribute__((always_inline))

} /* namespace usbsid */

#endif /* _US_CORE_TYPES_H_ */
