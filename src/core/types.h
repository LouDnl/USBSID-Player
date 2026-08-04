/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * types.h
 * Basic types, branch hints and memory placement attributes.
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
#ifndef _US_CORE_TYPES_H_
#define _US_CORE_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace usbsid {

/* Free running PHI2 cycle counter.
 * 64 bit on purpose: at ~1 MHz a 32 bit counter wraps after ~72 minutes, which
 * is well inside normal playing time and would corrupt every cycle delta. */
using cycle_t = uint_fast64_t;

/* C64 bus types */
using addr_t = uint16_t; /* 16 bit address bus */
using data_t = uint8_t;  /* 8 bit data bus */
using reg_t  = uint8_t;  /* chip register index */

/* Sub cycle step counter inside one instruction (0..7 is enough for a 6510) */
using ustep_t = uint_fast8_t;

/* Branch hints. The per cycle paths are dominated by "almost never taken"
 * checks (raster match, badline, timer underflow), so the hints matter. */
#define US_LIKELY(x)   __builtin_expect(!!(x), 1)
#define US_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Memory placement.
 * On the RP2350 the per cycle code and the RAM array must not live in flash,
 * or XIP stalls destroy the timing. On desktop these are no-ops.
 *
 * Both macros are written in the trailing attribute position so they work on
 * member functions as well as on free functions:
 *   void run(cycle_t n) US_RAM_ATTR;
 */
#if defined(EMBEDDED) && EMBEDDED
#define US_RAM_ATTR __attribute__((section(".time_critical.usbsid")))
#define US_RAM_DATA __attribute__((section(".data.usbsid")))
#else
#define US_RAM_ATTR
#define US_RAM_DATA
#endif

/* Force inline for the small accessors that sit in the per cycle path */
#define US_ALWAYS_INLINE inline __attribute__((always_inline))

} /* namespace usbsid */

#endif /* _US_CORE_TYPES_H_ */
