
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
#include <pico/stdlib.h>
#include <pico/types.h>
#include <pico/time.h>
#include <hardware/timer.h>
#endif

/**
 * @brief Returns the ticks per second
 * @note nanoseconds for DESKTOP
 * @note microseconds for EMBEDDED
 */
tick_t tick_per_second(void)
{
  return TICK_PER_SECOND;
}

/**
 * @brief For convenience and more clarity
 *        DESKTOP and EMBEDDED are split
 *        into separate blocks for the same
 *        functions
 */

 #if DESKTOP
/**
 * @brief return the current system ticks in nanoseconds
 */
tick_t tick_now(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);
  return NANO_TO_TICK(((uint64_t)NANO_PER_SECOND * now.tv_sec) + now.tv_nsec);
}

/**
 * @brief sleep implementation called by tick_sleep
 */
static inline void sleep_impl(tick_t sleep_ticks)
{
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
  return;
}

/**
 * @brief Sleep a number of timer units
 */
void tick_sleep(tick_t sleep_ticks)
{
  sleep_impl(sleep_ticks);
}

/**
 * @brief Returns the the number of system ticks
 */
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

#elif EMBEDDED

/**
 * @brief Returns the current 64-bit hardware timer value in microseconds.
 * @note The RP2040 timer is 64-bit; it will not overflow for 584,000 years.
 * @note On RP2350, this reads from the shared hardware timer block.
 */
tick_t __not_in_flash_func(tick_now)(void) {
    return time_us_64();
}

/**
 * @brief Ensures monotonic behavior.
 * @note The RP2040 timer is a single hardware peripheral shared by both cores,
 * so the +/- 1 tick jitter mentioned in the Windows documentation is typically
 * not an issue here, but we maintain the safety logic.
 * @note RP2350 timers are fully monotonic and shared across all cores (Cortex-M33
 * or Hazard3 RISC-V), eliminating the inter-core drift seen on x86.
 */
tick_t tick_now_after(tick_t previous_tick) {
    tick_t after = tick_now();

    /* Defensive check to ensure time never appears to move backward */
    if (after < previous_tick) {
        return previous_tick;
    }

    return after;
}

/**
 * @brief High-precision microsecond sleep implementation
 *        using the SDK's microsecond-accurate sleeper
 */
void tick_sleep(tick_t sleep_ticks) {
  sleep_us(sleep_ticks);
}

#endif
