/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * ram.h
 * 64 KB main RAM plus the 1 KB nybble wide colour RAM.
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
#ifndef _US_MEM_RAM_H_
#define _US_MEM_RAM_H_

#include "constants.h"
#include "types.h"

namespace usbsid {

/**
 * @brief C64 RAM.
 *
 * Plain storage, no address decoding: the MMU decides what a given address
 * means. Colour RAM is kept separate because it is only 4 bits wide and is
 * never banked out.
 *
 * The array is a member, not a heap allocation, so the embedded build can
 * place the whole object in RAM through US_RAM_DATA and there is no
 * allocation in the per cycle path.
 */
class Ram
{
  public:
    Ram(void);
    ~Ram(void) = default;

    /**
     * @brief Power on pattern.
     *
     * A real C64 powers up with alternating blocks of $00 and $ff, 64 bytes
     * each. Some tunes and a fair number of PRGs depend on it, so it is the
     * default rather than an all zero fill.
     */
    void reset(void);
    /* Fill everything with one value, used by the CPU test harness which
     * wants a plain all RAM machine */
    void fill(data_t value);

    US_ALWAYS_INLINE data_t read(addr_t addr) const { return ram_[addr]; }
    US_ALWAYS_INLINE void write(addr_t addr, data_t value) { ram_[addr] = value; }

    /* Colour RAM is 4 bits wide. Reads return the upper nybble as open bus
     * would on hardware; the MMU decides what to or in, so here the raw
     * nybble is returned and the write masks. */
    US_ALWAYS_INLINE data_t read_color(addr_t addr) const
    {
      return color_[addr & (kC64ColorRamSize - 1)] & 0x0f;
    }
    US_ALWAYS_INLINE void write_color(addr_t addr, data_t value)
    {
      color_[addr & (kC64ColorRamSize - 1)] = value & 0x0f;
    }

    /* Direct access for loaders and for the SID register mirror. Same thing
     * as read()/write(), named after the old player dma_ helpers so the
     * loader code reads the same way. */
    US_ALWAYS_INLINE data_t dma_read(addr_t addr) const { return ram_[addr]; }
    US_ALWAYS_INLINE void dma_write(addr_t addr, data_t value) { ram_[addr] = value; }

    /* Bulk load, clamped at the end of memory. Returns bytes written. */
    size_t load(addr_t addr, const data_t * data, size_t len);

    const data_t * data(void) const { return ram_; }

  private:
    data_t ram_[kC64RamSize];
    data_t color_[kC64ColorRamSize];
};

} /* namespace usbsid */

#endif /* _US_MEM_RAM_H_ */
