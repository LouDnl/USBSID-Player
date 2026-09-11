/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * bus.h
 * The system bus: owns the single PHI2 cycle counter, ticks every device once
 * per cycle in a fixed order and carries the IRQ, NMI and BA lines.
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
#ifndef _US_CORE_BUS_H_
#define _US_CORE_BUS_H_

#include "types.h"

namespace usbsid {

/**
 * @brief Anything that is clocked once per PHI2 cycle.
 *
 * No cycle argument or count: a device never "catches up" over a delta,
 * one call is one cycle.
 */
class ClockedDevice
{
  public:
    virtual ~ClockedDevice(void) = default;

    /* Advance this device by exactly one PHI2 cycle */
    virtual void tick(void) = 0;
    /* Return to power on state */
    virtual void reset(void) = 0;
};

/* The CPU is an ordinary ClockedDevice. It reads IRQ/NMI/BA off the bus
 * itself rather than having them pushed every cycle: pushing cost three
 * virtual calls/cycle, measured 45% of the emulation budget. The sampling
 * delay, poll timing and BA grace period all live in the CPU, not here. */

/* Interrupt sources are wired-or on real hardware, so every source keeps its
 * own bit and the line is asserted while any bit is set. */
enum class IrqSource : uint8_t {
  Vic       = 0x01,
  Cia1      = 0x02,
  Cia2      = 0x04, /* only when CIA2 is jumpered to IRQ, normally it is NMI */
  Expansion = 0x08,
};

enum class NmiSource : uint8_t {
  Cia2    = 0x01,
  Restore = 0x02,
};

/**
 * @brief The machine clock and the wiring between the chips.
 */
class Mos6510;
class Mos6526;
class Mos6569;

class Bus
{
  public:
    Bus(void);
    ~Bus(void) = default;

    /* Wiring. Any slot may stay empty, which is what the unit tests use to
     * bring up one chip at a time. */
    void attach_vic(ClockedDevice * vic);
    void attach_cia1(ClockedDevice * cia1);
    void attach_cia2(ClockedDevice * cia2);
    void attach_cpu(ClockedDevice * cpu);

    /* Reset the cycle counter, the lines and every attached device */
    void reset(void);

    /**
     * @brief Advance the whole machine by exactly one PHI2 cycle.
     *
     * The order is fixed, and every chip is clocked once per cycle in it:
     *   1. VIC       (PHI1 half: fetches, badline and BA decision, raster IRQ)
     *   2. CIA1      (timers, TOD, serial, IRQ line)
     *   3. CIA2      (idem, NMI line)
     *   4. CPU       (one bus cycle, it samples the lines itself)
     *   5. cycle counter
     */
    void tick(void) US_RAM_ATTR;

    /**
     * @brief The same four devices, by their real types - four direct calls
     * a cycle instead of four vtable trips. `attach_*` above stays for tests
     * that bring up one chip with fakes for the rest; `tick()` uses whichever
     * is set.
     */
    void attach_fast(Mos6569 * vic, Mos6526 * cia1, Mos6526 * cia2,
                     Mos6510 * cpu);

    /* A CIA's schedule changed (control register write or input moving
     * brought its next event forward) - ask it again. */
    void cia_rescheduled(const Mos6526 * who) US_RAM_ATTR;

    /** @brief The VIC's schedule changed, ask it again. */
    void vic_rescheduled(void) US_RAM_ATTR { vic_wake_ = 1; }

    /* Advance n cycles */
    void run(cycle_t n) US_RAM_ATTR;

    /* Free running PHI2 cycle counter */
    US_ALWAYS_INLINE cycle_t cycles(void) const { return cycles_; }

    /* How many ticks a caught-up-lazily device should have run. Mid-cycle,
     * the counter still names the cycle in progress, so a kept-up chip has
     * run one tick more than it; between cycles they agree. Reads/writes
     * arrive mid-cycle, the player/tests from outside it - being one tick
     * out moves every interrupt by a cycle. */
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
    /* NMI is edge sensitive, so the transition is latched here, at the
     * source, and stays latched until the CPU consumes it. */
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

    /* Bus Available. The VIC pulls this low while it steals cycles. */
    US_ALWAYS_INLINE void set_ba(bool available) { ba_ = available; }
    US_ALWAYS_INLINE bool ba(void) const { return ba_; }

  private:
    ClockedDevice * vic_  = nullptr;
    ClockedDevice * cia1_ = nullptr;
    ClockedDevice * cia2_ = nullptr;
    ClockedDevice * cpu_  = nullptr;

    /* the same four, typed, when a whole machine is attached */
    Mos6569 * fast_vic_  = nullptr;
    Mos6526 * fast_cia1_ = nullptr;
    Mos6526 * fast_cia2_ = nullptr;
    Mos6510 * fast_cpu_  = nullptr;

    /* Cycles left before each CIA has to be looked at again. Counting down an
     * integer is most of what a cycle costs once the chip is asleep. */
    uint32_t vic_wake_  = 1;
    uint32_t cia1_wake_ = 1;
    uint32_t cia2_wake_ = 1;

    cycle_t cycles_    = 0;
    uint8_t irq_lines_ = 0;
    uint8_t nmi_lines_ = 0;
    bool    nmi_edge_  = false;
    bool    ba_        = true; /* high (bus available) when nothing steals */
    bool    in_cycle_  = false; /* true while tick() is running */
};

} /* namespace usbsid */

#endif /* _US_CORE_BUS_H_ */
