/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * cynthcart_embedded.cpp
 * Wires the MC68B50 ACIA onto the player's machine and drives Cynthcart
 * through the ordinary PRG boot path (see player/player.cpp Player::init_prg,
 * which already boots the KERNAL, pokes the binary in and types RUN, exactly
 * what Cynthcart's "$0801, 10 SYS2061" stub needs). Embedded only, see
 * docs/EMBEDDED.md and _llm-memory/player-repo/TODO.md #50: nothing here is
 * wanted on desktop.
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
 */

#if defined(EMBEDDED) && EMBEDDED

#include "cynthcart_embedded.h"

#include "pico.h" /* cynthcart.h's __in_flash(...) needs pico/platform/sections.h,
                   * pulled in transitively here; pico/platform.h refuses to be
                   * included directly. */
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

  /* One ACIA for the life of the firmware, same "everything here is
   * statically allocated" reasoning as usplayer.cpp's g_machine. */
  static usbsid::MC68B50 acia(machine.bus());
  g_acia = &acia;
  g_acia->reset();
  machine.mmu().attach_io1(g_acia);

  /* Boots the KERNAL, pokes cynthcart[] in at its load address and types
   * "run", the same as load_sidtune()+init_sidplayer()+start_sidplayer()
   * does for a tune. */
  load_prg(cynthcart, sizeof(cynthcart), false);
}

void stop_cynthcart(void)
{
  stop_sidplayer();
  g_acia = nullptr;
}

unsigned int run_cynthcart(void)
{
  /* One instruction per call, not one frame: loop_sidplayer()'s frame
   * granularity (Player::run_frame() -> machine_.run(cycles_to_frame_end()))
   * only let a queued MIDI byte become visible to the CPU once every ~20ms,
   * and only one byte per call regardless of how many were queued - a
   * throughput cap under real MIDI traffic, not just a fixed latency. The
   * old emudore loop stepped and drained the ACIA together every iteration;
   * Machine::step_instruction() is the same pairing, at the same real
   * instruction-boundary IRQ-sample point a 6502 uses anyway. */
  if (g_acia == nullptr) return 0;
  usbsid::usplayer_machine().step_instruction();
  g_acia->emulate();
  return 0;
}

#endif /* EMBEDDED */
