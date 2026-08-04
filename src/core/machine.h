/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * machine.h
 * One assembled C64: RAM, ROMs, PLA, MMU, CPU and the bus that clocks them.
 * The chips that do not exist yet simply are not attached; the machine runs
 * without them, which is what every step up to now has relied on.
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
#ifndef _US_CORE_MACHINE_H_
#define _US_CORE_MACHINE_H_

#include "core/bus.h"
#include "keyboard.h"
#include "mmu.h"
#include "mos6526.h"
#include "mos6569.h"
#include "mos6581_8580.h"
#include "sid_backend.h"
#include "mos6510.h"
#include "mos906114_pla.h"
#include "ram.h"
#include "types.h"

namespace usbsid {

class Machine final : public CiaPortObserver
{
  public:
    Machine(void);
    ~Machine(void) = default;

    /* Power on: RAM pattern, PLA to mode 31, CPU through the reset vector */
    void power_on(void);
    /* Reset line only: the RAM keeps its contents */
    void reset(void);

    US_ALWAYS_INLINE void tick(void) { bus_.tick(); }
    void run(cycle_t cycles) US_RAM_ATTR;
    /* Run until the current instruction has finished */
    void step_instruction(void) US_RAM_ATTR;
    US_ALWAYS_INLINE cycle_t cycles(void) const { return bus_.cycles(); }

    Bus & bus(void) { return bus_; }
    Ram & ram(void) { return ram_; }
    Mos906114Pla & pla(void) { return pla_; }
    Mmu & mmu(void) { return mmu_; }
    Mos6510 & cpu(void) { return cpu_; }
    Mos6526 & cia1(void) { return cia1_; }
    Mos6526 & cia2(void) { return cia2_; }
    Mos6569 & vic(void) { return vic_; }
    /* The keyboard hangs off CIA1, which is the only place it is wired */
    Keyboard & keyboard(void) { return keyboard_; }
    Mos6581_8580 & sid(void) { return sid_; }

    /* Swap where SID writes go. The machine owns a null backend so it always
     * has somewhere to put them. */
    void set_sid_backend(SidBackend & backend) { sid_.set_backend(backend); }

    /* Picking the video model retimes the VIC and the TOD dividers together */
    void set_video_model(VideoModel model);
    VideoModel video_model(void) const { return vic_.model(); }

    /* CIA2 port A carries the VIC bank in its two low bits, inverted */
    void cia_port_changed(uint8_t port, data_t value) override;

  private:
    /* Declaration order is construction order: the MMU needs the RAM and the
     * PLA, the CPU needs the MMU and the bus. */
    Ram ram_;
    Mos906114Pla pla_;
    Mmu mmu_;
    Bus bus_;
    Mos6510 cpu_;
    Mos6526 cia1_;
    Mos6526 cia2_;
    Mos6569 vic_;
    Keyboard keyboard_;
    NullSidBackend null_sid_;
    Mos6581_8580 sid_;
};

} /* namespace usbsid */

#endif /* _US_CORE_MACHINE_H_ */
