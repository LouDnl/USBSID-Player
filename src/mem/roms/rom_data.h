/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * rom_data.h
 * The stock C64 ROM images, compiled in so neither the desktop player nor the
 * firmware needs to load a file. Regenerate rom_data.cpp with
 * temp/tools/bin2array.py after changing anything in roms/.
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
#ifndef _US_MEM_ROM_DATA_H_
#define _US_MEM_ROM_DATA_H_

#include "constants.h"
#include "types.h"

namespace usbsid {

extern const data_t kRomBasic[kRomSizeBasic];
extern const data_t kRomKernal[kRomSizeKernal];
extern const data_t kRomChargen[kRomSizeChargen];

} /* namespace usbsid */

#endif /* _US_MEM_ROM_DATA_H_ */
