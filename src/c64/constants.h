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
 * constants.h
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
#ifndef _US_CONSTANTS_H_
#define _US_CONSTANTS_H_

#include <cstdint>


/* C64 Constants */
const size_t pC64RamSize = 0x10000;
const size_t pC64PageSize = 0x100;
const size_t pC64PageEnd = 0xff;

/* RAM, I/O registers and Color RAM */
/* mos6560_6561 */
const uint16_t pAddrVicFirstPage    = 0xd000;
const uint16_t pAddrVicLastPage     = 0xd300;
/* mos6581_8580 */
const uint16_t pAddrSIDFirstPage    = 0xd400;
const uint16_t pAddrSIDSecondPage   = 0xd500;
const uint16_t pAddrSIDLastPage     = 0xd700;
/* Color RAM */
const uint16_t pAddrColorRAMFirstPage  = 0xd800;
const uint16_t pAddrColorRAMLastPage   = 0xdb00;
/* mos6526 */
const uint16_t pAddrCIA1Page        = 0xdc00;
const uint16_t pAddrCIA2Page        = 0xdd00;
/* IO */
const uint16_t pAddrIO1Page         = 0xde00;
const uint16_t pAddrIO2Page         = 0xdf00;

/* Fixed addresses */
const uint16_t pAddrDataDirection   = 0x0000;
const uint16_t pAddrMemoryLayout    = 0x0001;
const uint16_t pBaseAddrStack       = 0x0100;

/* ROM pages */
/* BASIC */
const uint16_t pAddrBasicFirstPage  = 0xa000;
const uint16_t pAddrBasicLastPage   = 0xbf00;
/* CHAR */
const uint16_t pAddrCharsFirstPage  = 0xd000;
const uint16_t pAddrCharsLastPage   = 0xdf00;
/* KERNAL */
const uint16_t pAddrKernalFirstPage = 0xe000;
const uint16_t pAddrKernalLastPage  = 0xff00;

/* Keyboard interaction keys for PSID64 */
/* rows are $dc00, cols are $dc01 */
const uint8_t row_bit_plus = 5;  /* $20/$df */
const uint8_t col_bit_plus = 0;  /* $01/$fe */
const uint8_t row_bit_minus = 5; /* $20/$df */
const uint8_t col_bit_minus = 3; /* $80/$7f */

#endif /* _US_CONSTANTS_H_ */
