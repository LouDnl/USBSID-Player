/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * bus.cpp
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

#include "bus.h"
#include "util/profile.h"

#include "mos6510.h"
#include "mos6526.h"
#include "mos6569.h"

namespace usbsid {

Bus::Bus(void)
{
}

void Bus::attach_vic(ClockedDevice * vic)
{
  vic_ = vic;
}

void Bus::attach_cia1(ClockedDevice * cia1)
{
  cia1_ = cia1;
}

void Bus::attach_cia2(ClockedDevice * cia2)
{
  cia2_ = cia2;
}

void Bus::attach_cpu(ClockedDevice * cpu)
{
  cpu_ = cpu;
}

void Bus::reset(void)
{
  cycles_    = 0;
  irq_lines_ = 0;
  nmi_lines_ = 0;
  nmi_edge_  = false;
  ba_        = true;

  /* Reset order mirrors the tick order so a device that looks at the bus
   * during reset sees the same neighbours it will see while running. */
  if (vic_  != nullptr) vic_->reset();
  if (cia1_ != nullptr) cia1_->reset();
  if (cia2_ != nullptr) cia2_->reset();
  if (cpu_  != nullptr) cpu_->reset();
}

void Bus::attach_fast(Mos6569 * vic, Mos6526 * cia1, Mos6526 * cia2,
                      Mos6510 * cpu)
{
  fast_vic_ = vic;
  fast_cia1_ = cia1;
  fast_cia2_ = cia2;
  fast_cpu_ = cpu;
  vic_wake_ = 1;
  cia1_wake_ = 1;
  cia2_wake_ = 1;
}

void Bus::cia_rescheduled(const Mos6526 * who)
{
  if (who == fast_cia1_) cia1_wake_ = 1;
  else if (who == fast_cia2_) cia2_wake_ = 1;
}

/**
 * @brief One PHI2 cycle.
 *
 * With a whole machine attached the four devices are called by their real
 * types, which is four direct calls rather than four loads from a vtable
 * followed by four indirect branches. It is worth a few percent on a desktop
 * and rather more on a Cortex-M33, where an indirect call predicts badly.
 */
#if defined(US_PROFILE) && US_PROFILE
Profile profile;
#endif

void Bus::tick(void)
{
  in_cycle_ = true;
  US_PROF(ticks);

  if (US_LIKELY(fast_cpu_ != nullptr)) {
    if (US_UNLIKELY(--vic_wake_ == 0)) {
      US_PROF(woke_vic);
      fast_vic_->catch_up(cycles_ + 1);
      vic_wake_ = fast_vic_->cycles_to_event();
    }

    /* The CIAs are only looked at when they said something would happen. In
     * between, this is a decrement: a timer counting down from a thousand has
     * nothing to show for nine hundred and ninety nine of those cycles, and
     * anything that reads the chip catches it up before it looks. */
    if (US_UNLIKELY(--cia1_wake_ == 0)) {
      US_PROF(woke_cia1);
      fast_cia1_->catch_up(cycles_ + 1);
      cia1_wake_ = fast_cia1_->cycles_to_event();
    }
    if (US_UNLIKELY(--cia2_wake_ == 0)) {
      US_PROF(woke_cia2);
      fast_cia2_->catch_up(cycles_ + 1);
      cia2_wake_ = fast_cia2_->cycles_to_event();
    }

    fast_cpu_->tick();
    ++cycles_;
    in_cycle_ = false;
    return;
  }

  /* one chip at a time, which is how the tests bring the machine up */
  if (vic_ != nullptr)  vic_->tick();
  if (cia1_ != nullptr) cia1_->tick();
  if (cia2_ != nullptr) cia2_->tick();
  if (cpu_ != nullptr)  cpu_->tick();
  ++cycles_;
  in_cycle_ = false;
}

void Bus::run(cycle_t n)
{
  while (n-- != 0) {
    tick();
  }
}

} /* namespace usbsid */
