/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * machine.cpp
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

#include "machine.h"

namespace usbsid {

Machine::Machine(void)
  : mmu_(ram_, pla_), cpu_(mmu_, bus_),
    cia1_(bus_, CiaLine::Irq), cia2_(bus_, CiaLine::Nmi),
    vic_(bus_, VideoModel::Pal6569), keyboard_(cia1_), sid_(bus_, null_sid_)
{
  bus_.attach_cpu(&cpu_);
  /* and again by their real types, so a cycle costs four direct calls */
  bus_.attach_fast(&vic_, &cia1_, &cia2_, &cpu_);
  cpu_.attach_mmu(&mmu_);
  bus_.attach_cia1(&cia1_);
  bus_.attach_cia2(&cia2_);
  bus_.attach_vic(&vic_);
  mmu_.attach_cia1(&cia1_);
  mmu_.attach_cia2(&cia2_);
  mmu_.attach_vic(&vic_);
  mmu_.attach_sid(&sid_);
  /* the FM/OPL addresses live in the second expansion IO page */
  mmu_.attach_io2(&sid_);
  cia2_.set_port_observer(this);
  vic_.set_frame_observer(&sid_);
  set_video_model(VideoModel::Pal6569);
  power_on();
}

void Machine::set_video_model(VideoModel model)
{
  vic_.set_model(model);
  /* the CIAs divide the system clock down to the TOD tick */
  cia1_.set_clock_hz(vic_.timing().clock_hz);
  cia2_.set_clock_hz(vic_.timing().clock_hz);
}

void Machine::cia_port_changed(uint8_t port, data_t value)
{
  /* Only CIA2 reports here. Port A bits 0 and 1 select the 16 KB bank the
   * VIC sees, inverted: %11 is bank 0. */
  if (port == 0) {
    mmu_.set_vic_bank(static_cast<uint8_t>(~value & 0x03));
  }
}

void Machine::power_on(void)
{
  ram_.reset();
  pla_.reset();
  mmu_.reset();
  bus_.reset(); /* also resets the cpu, which fetches the reset vector */
}

void Machine::reset(void)
{
  pla_.reset();
  mmu_.reset();
  cia1_.reset();
  cia2_.reset();
  vic_.reset();
  sid_.reset();
  cpu_.reset();
}

void Machine::run(cycle_t cycles)
{
  bus_.run(cycles);
}

void Machine::step_instruction(void)
{
  do {
    bus_.tick();
  } while (!cpu_.instruction_done());
}

} /* namespace usbsid */
