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
 * usplayer.h
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
#ifndef _US_PLAYER_H_
#define _US_PLAYER_H_

/* C64 Constants */
static const int INIT_CYCLES = 100000; /* Run N cycles as boot */

enum CIA_registers
{ /* https://www.c64-wiki.com/wiki/mos6526 */
  CIA1_TIMER_LO = 0xDC04,
  CIA1_TIMER_HI = 0xDC05
};

static const char *us_sidtype[5] = {"Unknown", "N/A", "MOS8580", "MOS6581", "FMopl" };  /* 0 = unknown, 1 = N/A, 2 = MOS8085, 3 = MOS6581, 4 = FMopl */
static const char *chiptype[4] = {"Unknown", "MOS6581", "MOS8580", "MOS6581 and MOS8580"};
static const char *clockspeed[5] = {"Unknown", "PAL", "NTSC", "PAL and NTSC", "DREAN"};

/* RSID player */
const unsigned int NUM_SCREEN_PAGES = 4;
const unsigned int NUM_MINDRV_PAGES = 2; // driver without screen display
const unsigned int NUM_EXTDRV_PAGES = 5; // driver with screen display
const unsigned int NUM_CHAR_PAGES = 4; // size of charset in pages
const unsigned int MAX_PAGES = 256;
const int SIDTUNE_COMPATIBILITY_R64 = 0x02; // File is Real C64 only

#endif /* _US_PLAYER_H_ */
