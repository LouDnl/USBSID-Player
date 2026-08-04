/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * io_device.h
 * A chip that lives in the $d000-$dfff IO block. The MMU decodes the block
 * and hands the full address through; each chip masks it down to its own
 * register set, because the mirroring differs per chip.
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
#ifndef _US_MEM_IO_DEVICE_H_
#define _US_MEM_IO_DEVICE_H_

#include "types.h"

namespace usbsid {

class IoDevice
{
  public:
    virtual ~IoDevice(void) = default;

    virtual data_t io_read(addr_t addr) = 0;
    virtual void io_write(addr_t addr, data_t value) = 0;
};

} /* namespace usbsid */

#endif /* _US_MEM_IO_DEVICE_H_ */
