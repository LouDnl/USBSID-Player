/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * cynthcart_embedded.cpp
 * Wires the MC68B50 ACIA onto the player machine and runs Cynthcart through
 * the PRG boot path (Player::init_prg), embedded only (docs/API_EMBEDDED.md)
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
 */

#if defined(EMBEDDED) && EMBEDDED

#include "cynthcart_embedded.h"

#include "pico.h" /* needed for __in_flash(...) of cynthcart.h, pico/platform.h cannot be included directly */
#include "usplayer.h"
#include "core/machine.h"
#include "MC68B50.h"
#include "cynthcart.h"

namespace {
usbsid::MC68B50 * g_acia = nullptr;
} /* namespace */

void start_cynthcart(void)
{
  usbsid::Machine & machine = usbsid::usplayer_machine();

  /* One ACIA for the life of the firmware, statically allocated */
  static usbsid::MC68B50 acia(machine.bus());
  g_acia = &acia;
  g_acia->reset();
  machine.mmu().attach_io1(g_acia);

  /* Boots the KERNAL, pokes cynthcart[] in at its load address and types "run" */
  load_prg(cynthcart, sizeof(cynthcart), false);
}

void stop_cynthcart(void)
{
  stop_sidplayer();
  g_acia = nullptr;
}

unsigned int run_cynthcart(void)
{
  /* One instruction per call, a frame per call delays MIDI bytes by ~20ms and passes one byte per frame.
   * Machine::step_instruction() samples IRQ at the instruction boundary like a 6502 */
  if (g_acia == nullptr) return 0;
  usbsid::usplayer_machine().step_instruction();
  g_acia->emulate();
  return 0;
}

#endif /* EMBEDDED */
