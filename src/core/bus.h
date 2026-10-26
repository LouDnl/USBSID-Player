/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * bus.h
 * System bus, holds the PHI2 cycle counter, ticks each device once per
 * cycle and carries the IRQ, NMI and BA lines
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
#ifndef _US_CORE_BUS_H_
#define _US_CORE_BUS_H_

#include "types.h"

namespace usbsid {

/**
 * @brief Device clocked once per PHI2 cycle, one call is one cycle
 */
class ClockedDevice
{
  public:
    virtual ~ClockedDevice(void) = default;

    /* Advance one PHI2 cycle */
    virtual void tick(void) = 0;
    /* Return to power on state */
    virtual void reset(void) = 0;
};

/* The CPU reads IRQ/NMI/BA from the bus (sampling delay, poll timing
   and BA grace period are in the CPU) */

/* Interrupt sources are wired-or, line is asserted while any source bit is set */
enum class IrqSource : uint8_t {
  Vic       = 0x01,
  Cia1      = 0x02,
  Cia2      = 0x04, /* Only when CIA2 is jumpered to IRQ, normally NMI */
  Expansion = 0x08,
};

enum class NmiSource : uint8_t {
  Cia2    = 0x01,
  Restore = 0x02,
};

/**
 * @brief Machine clock and chip wiring
 */
class Mos6510;
class Mos6526;
class Mos6569;

class Bus
{
  public:
    Bus(void);
    ~Bus(void) = default;

    /* Wiring, any slot may stay empty */
    void attach_vic(ClockedDevice * vic);
    void attach_cia1(ClockedDevice * cia1);
    void attach_cia2(ClockedDevice * cia2);
    void attach_cpu(ClockedDevice * cpu);

    /* Reset cycle counter, lines and attached devices */
    void reset(void);

    /**
     * @brief Advance the machine one PHI2 cycle, in this order
     *   1. VIC       (PHI1 half: fetches, badline and BA decision, raster IRQ)
     *   2. CIA1      (timers, TOD, serial, IRQ line)
     *   3. CIA2      (same, NMI line)
     *   4. CPU       (one bus cycle, samples the lines)
     *   5. cycle counter
     */
    void tick(void) US_RAM_ATTR;

    /**
     * @brief Attach the four devices by real type for direct calls
     */
    void attach_fast(Mos6569 * vic, Mos6526 * cia1, Mos6526 * cia2,
                     Mos6510 * cpu);

    /* A CIA schedule changed (control register write or input), ask it again */
    void cia_rescheduled(const Mos6526 * who) US_RAM_ATTR;

    /** @brief The VIC schedule changed, ask it again */
    void vic_rescheduled(void) US_RAM_ATTR { vic_wake_ = 1; }

    /* Advance n cycles */
    void run(cycle_t n) US_RAM_ATTR;

    /* Free running PHI2 cycle counter */
    US_ALWAYS_INLINE cycle_t cycles(void) const { return cycles_; }

    /* Ticks a lazily caught up device should have run, +1 mid-cycle
       (one tick off moves every interrupt by a cycle) */
    US_ALWAYS_INLINE cycle_t catch_up_target(void) const
    {
      return cycles_ + (in_cycle_ ? 1u : 0u);
    }
    US_ALWAYS_INLINE void cycles(cycle_t v) { cycles_ = v; }

    /* Interrupt lines, wired-or per source */
    US_ALWAYS_INLINE void set_irq(IrqSource src, bool asserted)
    {
      if (asserted) irq_lines_ |=  static_cast<uint8_t>(src);
      else          irq_lines_ &= ~static_cast<uint8_t>(src);
    }
    /* NMI is edge sensitive, latched until the CPU consumes it */
    US_ALWAYS_INLINE void set_nmi(NmiSource src, bool asserted)
    {
      const uint8_t before = nmi_lines_;
      if (asserted) nmi_lines_ |=  static_cast<uint8_t>(src);
      else          nmi_lines_ &= ~static_cast<uint8_t>(src);
      if (before == 0 && nmi_lines_ != 0) nmi_edge_ = true;
    }
    US_ALWAYS_INLINE bool nmi_edge(void) const { return nmi_edge_; }
    US_ALWAYS_INLINE void clear_nmi_edge(void) { nmi_edge_ = false; }
    US_ALWAYS_INLINE bool irq_asserted(void) const { return irq_lines_ != 0; }
    US_ALWAYS_INLINE bool nmi_asserted(void) const { return nmi_lines_ != 0; }
    US_ALWAYS_INLINE uint8_t irq_sources(void) const { return irq_lines_; }
    US_ALWAYS_INLINE uint8_t nmi_sources(void) const { return nmi_lines_; }

    /* Bus Available, VIC pulls it low while stealing cycles */
    US_ALWAYS_INLINE void set_ba(bool available) { ba_ = available; }
    US_ALWAYS_INLINE bool ba(void) const { return ba_; }

  private:
    ClockedDevice * vic_  = nullptr;
    ClockedDevice * cia1_ = nullptr;
    ClockedDevice * cia2_ = nullptr;
    ClockedDevice * cpu_  = nullptr;

    /* Same four by real type, set with a whole machine attached */
    Mos6569 * fast_vic_  = nullptr;
    Mos6526 * fast_cia1_ = nullptr;
    Mos6526 * fast_cia2_ = nullptr;
    Mos6510 * fast_cpu_  = nullptr;

    /* Cycles left before each chip is looked at again */
    uint32_t vic_wake_  = 1;
    uint32_t cia1_wake_ = 1;
    uint32_t cia2_wake_ = 1;

    cycle_t cycles_    = 0;
    uint8_t irq_lines_ = 0;
    uint8_t nmi_lines_ = 0;
    bool    nmi_edge_  = false;
    bool    ba_        = true; /* High = bus available */
    bool    in_cycle_  = false; /* True while tick() runs */
};

} /* namespace usbsid */

#endif /* _US_CORE_BUS_H_ */
