
/*
 * timer.cpp - Adaption for USBSID-player
 *
 * Contains excerpts from vice code, see below for licensing. information
 *
 * Changes and additions for USBSID-Player by LouD ~ (c) 2026
 *
 */

/*
 * (original) vsync.c - End-of-frame handling
 *
 * Written by
 *  Ettore Perazzoli <ettore@comm2000.it>
 *  Teemu Rantanen <tvr@cs.hut.fi>
 *  Andreas Boose <viceteam@t-online.de>
 *  Dag Lem <resid@nimrod.no>
 *  Thomas Bretz <tbretz@gsi.de>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

/* This does what has to be done at the end of each screen frame (50 times per
   second on PAL machines). */

/* NB! The timing code depends on two's complement arithmetic.
   unsigned long is used for the timer variables, and the difference
   between two time points a and b is calculated with (signed long)(b - a)
   This allows timer variables to overflow without any explicit
   overflow handling.
*/

#include <timer.h>

#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>

#if EMBEDDED
#include <pico/types.h>
#include <pico/time.h>
extern "C" uint32_t clockcycles(void);
extern "C" void clockcycle_delay(uint32_t n_cycles);
extern "C" uint64_t pico_ns_since_boot(void);
#endif

tick_t tick_per_second(void)
{
    return TICK_PER_SECOND;
}

tick_t tick_now(void)
{
#if DESKTOP
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC_RAW, &now);

    return NANO_TO_TICK(((uint64_t)NANO_PER_SECOND * now.tv_sec) + now.tv_nsec);
#elif EMBEDDED // TODO: FIX
    uint64_t now = pico_ns_since_boot();;
    return NANO_TO_TICK((uint64_t)now);
#endif
    return 0;
}

static inline void sleep_impl(tick_t sleep_ticks)
{
#if DESKTOP
    struct timespec ts;
    uint64_t nanos = TICK_TO_NANO(sleep_ticks);

    if (nanos < NANO_PER_SECOND) {
        ts.tv_sec = 0;
        ts.tv_nsec = nanos;
    } else {
        ts.tv_sec = nanos / NANO_PER_SECOND;
        ts.tv_nsec = nanos % NANO_PER_SECOND;
    }

    nanosleep(&ts, NULL);
#elif EMBEDDED
    uint64_t micros = TICK_TO_MICRO(sleep_ticks);
    sleep_us(micros);
#endif
    return;
}

/* Sleep a number of timer units. */
void tick_sleep(tick_t sleep_ticks)
{
  sleep_impl(sleep_ticks);
}

tick_t tick_now_after(tick_t previous_tick)
{
    /*
     * Fark, high performance counters, called from different threads / cpus, can be off by 1 tick.
     *
     *    "When you compare performance counter results that are acquired from different
     *     threads, consider values that differ by +/- 1 tick to have an ambiguous ordering.
     *     If the time stamps are taken from the same thread, this +/- 1 tick uncertainty
     *     doesn't apply. In this context, the term tick refers to a period of time equal
     *     to 1 divided by (the frequency of the performance counter obtained from
     *     QueryPerformanceFrequency)."
     *
     * https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps#guidance-for-acquiring-time-stamps
     */

    tick_t after = tick_now();

    if (after == previous_tick - 1) {
        after = previous_tick;
    }

    return after;
}

tick_t tick_now_delta(tick_t previous_tick)
{
    return tick_now_after(previous_tick) - previous_tick;
}
