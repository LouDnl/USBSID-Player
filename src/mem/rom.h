/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * rom.h
 * Where the machine gets its ROM images from. Defaults to the compiled in
 * stock images; the pointers exist so a caller can substitute its own set
 * (a patched KERNAL, a different character generator) without the MMU
 * knowing anything about it.
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
#ifndef _US_MEM_ROM_H_
#define _US_MEM_ROM_H_

#include "roms/rom_data.h"
#include "types.h"

namespace usbsid {

struct Roms {
  const data_t * basic   = kRomBasic;
  const data_t * kernal  = kRomKernal;
  const data_t * chargen = kRomChargen;
};

} /* namespace usbsid */

#endif /* _US_MEM_ROM_H_ */
