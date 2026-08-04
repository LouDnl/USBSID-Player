/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * flat_memory.h
 * A machine that is nothing but 64 KB of RAM: no ROMs, no banking, no IO.
 * This is what the 6502 test suites want, and it is the smallest thing the
 * CPU can be tested against.
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
#ifndef _US_MEM_FLAT_MEMORY_H_
#define _US_MEM_FLAT_MEMORY_H_

#include "memory.h"
#include "ram.h"

namespace usbsid {

class FlatMemory final : public Memory
{
  public:
    explicit FlatMemory(Ram & ram) : ram_(ram) {}

    data_t read(addr_t addr) override { return ram_.read(addr); }
    void write(addr_t addr, data_t value) override { ram_.write(addr, value); }

    /* Bus cycle counters, handy when a test wants to know that the dummy
     * accesses really happened */
    unsigned reads  = 0;
    unsigned writes = 0;

    Ram & ram(void) { return ram_; }

  private:
    Ram & ram_;
};

/**
 * @brief Same thing, but counting every access.
 *
 * Kept separate so the plain FlatMemory stays free of bookkeeping in the
 * per cycle path.
 */
class CountingMemory final : public Memory
{
  public:
    explicit CountingMemory(Ram & ram) : ram_(ram) {}

    data_t read(addr_t addr) override
    {
      ++reads;
      last_read_addr = addr;
      return ram_.read(addr);
    }
    void write(addr_t addr, data_t value) override
    {
      ++writes;
      last_write_addr = addr;
      last_write_data = value;
      ram_.write(addr, value);
    }

    unsigned reads  = 0;
    unsigned writes = 0;
    addr_t last_read_addr  = 0;
    addr_t last_write_addr = 0;
    data_t last_write_data = 0;

  private:
    Ram & ram_;
};

} /* namespace usbsid */

#endif /* _US_MEM_FLAT_MEMORY_H_ */
