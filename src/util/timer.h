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
 * timer.h
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
#ifndef _US_TIMER_H
#define _US_TIMER_H


#include <stdint.h>

#include <types.h>

#ifndef US_MONOTONIC_CLOCK
#if defined(_WIN32) || defined(__CYGWIN__)
#define US_MONOTONIC_CLOCK CLOCK_MONOTONIC
#else
#define US_MONOTONIC_CLOCK CLOCK_MONOTONIC_RAW
#endif
#endif /* US_MONOTONIC_CLOCK */


/* number of ticks per second */
tick_t tick_per_second(void);

/* Get time in ticks. */
tick_t tick_now(void);

/* Get time in ticks, compensating for the +/- 1 tick that is possible on Windows. */
tick_t tick_now_after(tick_t previous_tick);

/* Sleep a number of ticks. */
void tick_sleep(tick_t delay);


#endif /* _US_TIMER_H */
