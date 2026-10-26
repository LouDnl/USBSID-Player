/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * ram.h
 * 64 KB main RAM plus 1 KB nybble wide color RAM
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
#ifndef _US_MEM_RAM_H_
#define _US_MEM_RAM_H_

#include "constants.h"
#include "types.h"

namespace usbsid {

/**
 * @brief C64 RAM, plain storage without address decoding
 *
 * color RAM is separate (4 bits wide, never banked out)
 * Array is a member, embedded build places it with US_RAM_DATA
 */
class Ram
{
  public:
    Ram(void);
    ~Ram(void) = default;

    /**
     * @brief Power on pattern, alternating 64 byte blocks of $00 and $ff
     */
    void reset(void);
    /* Fill everything with one value */
    void fill(data_t value);

    US_ALWAYS_INLINE data_t read(addr_t addr) const { return ram_[addr]; }
    US_ALWAYS_INLINE void write(addr_t addr, data_t value) { ram_[addr] = value; }

    /* color RAM is 4 bits wide, raw nybble is returned and writes are masked */
    US_ALWAYS_INLINE data_t read_color(addr_t addr) const
    {
      return color_[addr & (kC64ColorRamSize - 1)] & 0x0f;
    }
    US_ALWAYS_INLINE void write_color(addr_t addr, data_t value)
    {
      color_[addr & (kC64ColorRamSize - 1)] = value & 0x0f;
    }

    /* Direct access for loaders and the SID register mirror */
    US_ALWAYS_INLINE data_t dma_read(addr_t addr) const { return ram_[addr]; }
    US_ALWAYS_INLINE void dma_write(addr_t addr, data_t value) { ram_[addr] = value; }

    /* Bulk load, clamped at end of memory, returns bytes written */
    size_t load(addr_t addr, const data_t * data, size_t len);

    const data_t * data(void) const { return ram_; }

  private:
    data_t ram_[kC64RamSize];
    data_t color_[kC64ColorRamSize];
};

} /* namespace usbsid */

#endif /* _US_MEM_RAM_H_ */
