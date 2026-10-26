/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * rom_data.h
 * Stock C64 ROM images, compiled in
 * Regenerate rom_data.cpp with bin2array.py after changing any roms
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
