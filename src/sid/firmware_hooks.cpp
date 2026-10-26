/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * firmware_hooks.cpp
 * Pointers to the USBSID-Pico firmware functions
 *
 * Compiled into every build, the web build has no embedded backend
 * and the C API benchmark still reads the clock through us_time_us_64
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

#include <cstdint>

/* Firmware hooks
 *
 * EMBEDDED: the real symbols are declared and taken by address
 * Otherwise null and the backend stays quiet, a test assigns its own
 *
 * Pointers because weak functions are ELF only (see sid_embedded.h)
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

/* Never set on the device, a test sets it to drive the pacer from a clock it controls */
void (*us_busy_wait_us)(uint32_t) = nullptr;

} /* extern "C" */

/* Constant initialisers (address or null), set before any dynamic initialiser runs
 * Safe to bind from a namespace scope constructor in the web build and tests */
