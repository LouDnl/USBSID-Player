/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * memory.h
 * What the CPU sees of the world: one read and one write per bus cycle.
 * Step 2.4 plugs the MMU in here, the CPU never learns the difference.
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
#ifndef _US_MEM_MEMORY_H_
#define _US_MEM_MEMORY_H_

#include "types.h"

namespace usbsid {

/**
 * @brief The CPU side of the address bus.
 *
 * Every call is one real bus cycle, including the dummy reads and the dummy
 * write of a read-modify-write. IO side effects depend on those, so they are
 * never optimised away.
 */
class Memory
{
  public:
    virtual ~Memory(void) = default;

    virtual data_t read(addr_t addr) = 0;
    virtual void write(addr_t addr, data_t value) = 0;
};

} /* namespace usbsid */

#endif /* _US_MEM_MEMORY_H_ */
