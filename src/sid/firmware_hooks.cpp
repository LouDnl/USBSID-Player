/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * firmware_hooks.cpp
 * Where the pointers to USBSID-Pico's own functions live.
 *
 * One translation unit, compiled into **every** build, which is the whole reason
 * this file exists rather than the definitions sitting in sid_embedded.cpp: the
 * web build does not compile the embedded backend, and the C API's benchmark
 * still reads the clock through `us_time_us_64`. Putting them with the embedded
 * backend linked on desktop and embedded and failed on the web.
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

#include <cstdint>

/* ---- the firmware hooks ----------------------------------------------------
 *
 * One definition of each, and where they point depends on whether the firmware
 * is there. Under EMBEDDED the real symbols exist, so they are declared normally
 * and taken by address; the firmware itself needs no edit and does not know this
 * changed. Anywhere else they start null and the backend goes quiet, exactly as
 * an undefined weak used to, and a test assigns whatever it wants to measure.
 *
 * See the note in sid_embedded.h for why they are pointers rather than weak
 * functions: weak is ELF only and this suite builds on Mach-O and PE too.
 */
#if defined(EMBEDDED) && EMBEDDED
extern "C" {
  void cycled_write_operation(uint8_t address, uint8_t data, uint16_t cycles);
  uint8_t cycled_read_operation(uint8_t address, uint16_t cycles);
  void reset_sid(void);
  void reset_sid_registers(void);
  uint64_t time_us_64(void);
}
#endif

extern "C" {

#if defined(EMBEDDED) && EMBEDDED
void (*us_cycled_write)(uint8_t, uint8_t, uint16_t) = &cycled_write_operation;
uint8_t (*us_cycled_read)(uint8_t, uint16_t) = &cycled_read_operation;
void (*us_reset_sid)(void) = &reset_sid;
void (*us_reset_sid_registers)(void) = &reset_sid_registers;
uint64_t (*us_time_us_64)(void) = &time_us_64;
#else
void (*us_cycled_write)(uint8_t, uint8_t, uint16_t) = nullptr;
uint8_t (*us_cycled_read)(uint8_t, uint16_t) = nullptr;
void (*us_reset_sid)(void) = nullptr;
void (*us_reset_sid_registers)(void) = nullptr;
uint64_t (*us_time_us_64)(void) = nullptr;
#endif

/* Never set on the device: there the spin on us_time_us_64 is what a busy wait
 * would have been anyway. A test sets it to drive the pacer from a clock it
 * controls. */
void (*us_busy_wait_us)(uint32_t) = nullptr;

} /* extern "C" */

/* Every one of these is initialised with a constant, either an address or null,
 * so they are set before any dynamic initialiser anywhere can run. That is what
 * makes it safe for the web build and the tests to bind them from a namespace
 * scope constructor: static initialisation always completes first. */
