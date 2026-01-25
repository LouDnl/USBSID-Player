/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * Parts if this emulator are based on other great emulators and players
 * like Vice, SidplayFp, Websid, SidBerry and emudore/adorable
 *
 * types.h
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
#ifndef _US_TYPES_H
#define _US_TYPES_H


/* CLOCK */
typedef uint_fast64_t CPUCLOCK;
typedef uint_fast32_t TIMER;

/* Using a 32-bit type for tick_t prevents wraparound bugs on platforms with 32-bit timers (Vice) */
typedef uint32_t tick_t;
typedef uint_fast16_t counter_t;
typedef uint_fast8_t cycle_t;

#define MILLI_PER_SECOND    (1000)
#define MICRO_PER_SECOND    (1000 * 1000)
#define NANO_PER_SECOND     (1000 * 1000 * 1000)

#define TICK_PER_SECOND     (MICRO_PER_SECOND)

#define TICK_TO_MILLI(tick) ((uint_fast32_t) (uint_fast64_t)((double)(tick) * ((double)MILLI_PER_SECOND / TICK_PER_SECOND)))
#define TICK_TO_MICRO(tick) ((uint_fast32_t) (uint_fast64_t)((double)(tick) * ((double)MICRO_PER_SECOND / TICK_PER_SECOND)))
#define TICK_TO_NANO(tick)  ((uint_fast64_t) (uint_fast64_t)((double)(tick) * ((double)NANO_PER_SECOND  / TICK_PER_SECOND)))
#define NANO_TO_TICK(nano)  ((tick_t)   (uint_fast64_t)((double)(nano) / ((double)NANO_PER_SECOND  / TICK_PER_SECOND)))


/* DATA */
typedef uint_least32_t longword_t;
typedef uint_least16_t addr_t;
typedef uint_least8_t  val_t;
typedef uint_least8_t  reg_t;


#endif /* _US_TYPES_H */
