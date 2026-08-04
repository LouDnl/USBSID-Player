/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_backend.h
 * Where SID register writes go.
 *
 * The emulation decides *what* is written and *when*, in cycles. A backend
 * decides what to do with that: send it to USBSID-Pico, throw it away, or
 * record it. Keeping the two apart is what lets the whole player be tested
 * without hardware, and it is the seam a software SID would slot into later.
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
#ifndef _US_SID_SID_BACKEND_H_
#define _US_SID_SID_BACKEND_H_

#include "types.h"

namespace usbsid {

class SidBackend
{
  public:
    virtual ~SidBackend(void) = default;

    /**
     * @brief One register write.
     *
     * @param reg     physical register, chip number already folded in:
     *                $00-$1f is the first SID, $20-$3f the second, and so on
     * @param value   the byte written
     * @param cycles  cycles since the previous event, never more than $ffff
     */
    virtual void write(data_t reg, data_t value, uint16_t cycles) = 0;

    /** @brief One register read. Backends without real hardware may guess. */
    virtual data_t read(data_t reg, uint16_t cycles) { (void)reg; (void)cycles; return 0; }

    /** @brief More than $ffff cycles passed with nothing to write. */
    virtual void wait(uint16_t cycles) { (void)cycles; }

    /** @brief End of frame: push whatever is buffered. */
    virtual void flush(void) {}

    virtual void reset(void) {}
};

/** @brief Throws everything away. The default, and what the tests run on. */
class NullSidBackend final : public SidBackend
{
  public:
    void write(data_t reg, data_t value, uint16_t cycles) override
    {
      (void)reg; (void)value; (void)cycles;
      ++writes;
    }
    void flush(void) override { ++flushes; }
    void reset(void) override { writes = 0; flushes = 0; }

    uint32_t writes = 0;
    uint32_t flushes = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_SID_BACKEND_H_ */
