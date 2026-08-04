/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * constants.h
 * C64 memory map, fixed vectors and keyboard matrix bits.
 * Address map and PSID64 keyboard bits are carried over from
 * player-repo/src/util/constants.h so both players agree.
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

#pragma once
#ifndef _US_CORE_CONSTANTS_H_
#define _US_CORE_CONSTANTS_H_

#include "types.h"

namespace usbsid {

/* Memory sizes */
constexpr size_t kC64RamSize      = 0x10000; /* 64 KB main RAM */
constexpr size_t kC64ColorRamSize = 0x0400;  /* 1 KB nybble wide colour RAM */
constexpr size_t kC64PageSize     = 0x100;
constexpr size_t kRomSizeBasic    = 0x2000;  /* 8 KB */
constexpr size_t kRomSizeKernal   = 0x2000;  /* 8 KB */
constexpr size_t kRomSizeChargen  = 0x1000;  /* 4 KB */

/* IO page bases */
constexpr addr_t kAddrVicFirstPage      = 0xd000;
constexpr addr_t kAddrVicLastPage       = 0xd300;
constexpr addr_t kAddrSidFirstPage      = 0xd400;
constexpr addr_t kAddrSidSecondPage     = 0xd500;
constexpr addr_t kAddrSidLastPage       = 0xd700;
constexpr addr_t kAddrColorRamFirstPage = 0xd800;
constexpr addr_t kAddrColorRamLastPage  = 0xdb00;
constexpr addr_t kAddrCia1Page          = 0xdc00;
constexpr addr_t kAddrCia2Page          = 0xdd00;
constexpr addr_t kAddrIo1Page           = 0xde00;
constexpr addr_t kAddrIo2Page           = 0xdf00;

/* ROM ranges */
constexpr addr_t kAddrBasicFirstPage  = 0xa000;
constexpr addr_t kAddrBasicLastPage   = 0xbf00;
constexpr addr_t kAddrCharsFirstPage  = 0xd000;
constexpr addr_t kAddrCharsLastPage   = 0xdf00;
constexpr addr_t kAddrKernalFirstPage = 0xe000;
constexpr addr_t kAddrKernalLastPage  = 0xff00;

/* Fixed addresses and vectors */
constexpr addr_t kAddrDataDirection = 0x0000; /* 6510 processor port DDR */
constexpr addr_t kAddrMemoryLayout  = 0x0001; /* 6510 processor port */
constexpr addr_t kBaseAddrStack     = 0x0100;
constexpr addr_t kAddrNmiVector     = 0xfffa;
constexpr addr_t kAddrResetVector   = 0xfffc;
constexpr addr_t kAddrIrqVector     = 0xfffe;

/* Keyboard matrix bits used for PSID64 style control.
 * Rows are written to $dc00, columns are read from $dc01. */
constexpr uint8_t kRowBitRunstop = 7;
constexpr uint8_t kColBitRunstop = 7;
constexpr uint8_t kRowBitSpace   = 7;
constexpr uint8_t kColBitSpace   = 4;
constexpr uint8_t kRowBitPlus    = 5;
constexpr uint8_t kColBitPlus    = 0;
constexpr uint8_t kRowBitMinus   = 5;
constexpr uint8_t kColBitMinus   = 3;

} /* namespace usbsid */

#endif /* _US_CORE_CONSTANTS_H_ */
