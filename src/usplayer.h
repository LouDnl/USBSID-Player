/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * usplayer.h
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025 LouD
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
#ifndef _US_PLAYER_H_
#define _US_PLAYER_H_

#define CLOCK_CYCLES 100000

/* C64 Constants */
static const size_t C64RamSize = 0x10000;
static const size_t C64PageSize = 0x100;
static const int INIT_CYCLES = CLOCK_CYCLES;
/* RAM, I/O registers and Color RAM */
/* VIC */
static const uint16_t kAddrVicFirstPage    = 0xd000;
static const uint16_t kAddrVicLastPage     = 0xd300;
/* SID */
static const uint16_t kAddrSIDFirstPage    = 0xd400;
static const uint16_t kAddrSIDSecondPage   = 0xd500;
static const uint16_t kAddrSIDLastPage     = 0xd700;
/* CIA */
static const uint16_t kAddrCIA1Page        = 0xdc00;
static const uint16_t kAddrCIA2Page        = 0xdd00;
/* IO */
static const uint16_t kAddrIO1Page         = 0xde00;
static const uint16_t kAddrIO2Page         = 0xdf00;

enum CIA1_registers
{ /* https://www.c64-wiki.com/wiki/CIA */
  CIA_TIMER_LO = 0xDC04,
  CIA_TIMER_HI = 0xDC05,
};

// static const enum clock_speeds clockSpeed[] = {UNKNOWN, PAL, NTSC, BOTH, DREAN};
// static const enum refresh_rates refreshRate[] = {DEFAULT, EU, US, GLOBAL};
// static const enum scan_lines scanLines[] = {C64_PAL_SCANLINES, C64_PAL_SCANLINES, C64_NTSC_SCANLINES, C64_NTSC_SCANLINES};
// static const enum scanline_cycles scanlinesCycles[] = {C64_PAL_SCANLINE_CYCLES, C64_PAL_SCANLINE_CYCLES, C64_NTSC_SCANLINE_CYCLES, C64_NTSC_SCANLINE_CYCLES};
// const char *sidtype[5] = {"Unknown", "N/A", "MOS8580", "MOS6581", "FMopl" };  /* 0 = unknown, 1 = N/A, 2 = MOS8085, 3 = MOS6581, 4 = FMopl */
const char *chiptype[4] = {"Unknown", "MOS6581", "MOS8580", "MOS6581 and MOS8580"};
const char *clockspeed[5] = {"Unknown", "PAL", "NTSC", "PAL and NTSC", "DREAN"};

#endif /* _US_PLAYER_H_ */
