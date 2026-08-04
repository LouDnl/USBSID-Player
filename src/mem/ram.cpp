/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * ram.cpp
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

#include <cstring>

#include "ram.h"

namespace usbsid {

/* Size of one block in the power on pattern */
static constexpr size_t kPowerOnBlock = 0x40;

Ram::Ram(void)
{
  reset();
}

void Ram::reset(void)
{
  /* $00 for 64 bytes, $ff for 64 bytes, repeated over the whole 64 KB */
  for (size_t i = 0; i < kC64RamSize; i += kPowerOnBlock) {
    const data_t value = ((i / kPowerOnBlock) & 1) ? 0xff : 0x00;
    memset(&ram_[i], value, kPowerOnBlock);
  }
  memset(color_, 0, kC64ColorRamSize);
}

void Ram::fill(data_t value)
{
  memset(ram_, value, kC64RamSize);
}

size_t Ram::load(addr_t addr, const data_t * data, size_t len)
{
  if (data == nullptr || len == 0) return 0;

  size_t space = kC64RamSize - addr;
  size_t n = (len > space) ? space : len;
  memcpy(&ram_[addr], data, n);
  return n;
}

} /* namespace usbsid */
