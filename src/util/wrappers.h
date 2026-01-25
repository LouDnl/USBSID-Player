/**
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
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 *
 * @file wrappers.h
 * @author LouD (usbsid-player@loudai.nl)
 * @brief Use this header as include for accessing functions in wrapper.cpp
 * @version 0.1
 * @date 2026-01-24
 *
 * @copyright Copyright (c) 2026
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
#ifndef _US_WRAPPERS_H
#define _US_WRAPPERS_H

#include <stdint.h>
#if DESKTOP
#include <unistd.h>
#elif EMBEDDED
#include <pico/time.h>
#endif

void emu_sleep_us(uint64_t sleep_time_us);


#endif /* _US_WRAPPERS_H */
