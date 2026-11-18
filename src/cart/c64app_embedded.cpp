/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * c64app_embedded.cpp
 * Wires the MC68B50 ACIA onto the player machine and runs an embedded C64
 * MIDI application (Cynthcart, SID SYNTH) through the PRG boot path
 * (Player::init_prg), embedded only (docs/API_EMBEDDED.md)
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

#include "c64app_embedded.h"

#include <stddef.h>

#include "pico.h" /* needed for __in_flash(...) of the prg headers, pico/platform.h cannot be included directly */
#include "usplayer.h"
#include "core/machine.h"
#include "MC68B50.h"
#include "vdrive.h"
#include "cynthcart.h"
#include "sid_synth.h"
#include "sid_synth_disk.h"

namespace {

/* The banks of the SID SYNTH disk, names as the synth's browser shows them */
const usbsid::VFile kSidSynthFiles[] = {
  { "PRESETS1.SLO", sid_synth_presets1, sizeof(sid_synth_presets1) },
  { "PRESETS2.SLO", sid_synth_presets2, sizeof(sid_synth_presets2) },
  { "PRESETS3.SLO", sid_synth_presets3, sizeof(sid_synth_presets3) },
  { "PRESETS4.SLO", sid_synth_presets4, sizeof(sid_synth_presets4) },
  { "PRESETS5.SLO", sid_synth_presets5, sizeof(sid_synth_presets5) },
};

struct C64App {
  unsigned char * prg;
  size_t size;
  const usbsid::VFile * files;  /* nullptr: no virtual drive */
  size_t file_count;
  const char * disk_name;
  const char * disk_id;
  usbsid::addr_t sid2_base;     /* 0: one SID at $d400 */
};

/* Indexed by c64app_t. Both expect a Datel-layout ACIA on IO1 */
const C64App kApps[C64APP_COUNT] = {
  { cynthcart, sizeof(cynthcart), nullptr, 0, nullptr, nullptr, 0xd420 },
  { sid_synth, sizeof(sid_synth), kSidSynthFiles, sizeof(kSidSynthFiles) / sizeof(kSidSynthFiles[0]),
    "SID SYNTH V1.0", "NA", 0xd420 },
};

usbsid::MC68B50 * g_acia = nullptr;
int g_app = C64APP_COUNT;

} /* namespace */

void start_c64app(int app)
{
  if (app < 0 || app >= C64APP_COUNT) app = C64APP_CYNTHCART;
  usbsid::Machine & machine = usbsid::usplayer_machine();
  const C64App & a = kApps[app];

  /* One ACIA for the life of the firmware, statically allocated */
  static usbsid::MC68B50 acia(machine.bus());
  g_acia = &acia;
  g_acia->reset();

  /* The drive owns IO1 and passes $de00-$de7f on to the ACIA, one drive per app with files */
  usbsid::VirtualDrive * drive = nullptr;
  if (app == C64APP_SIDSYNTH) {
    static usbsid::VirtualDrive sid_synth_drive(machine.ram(), &acia, a.files, a.file_count, a.disk_name, a.disk_id);
    drive = &sid_synth_drive;
    drive->reset();
    machine.mmu().attach_io1(drive);
  } else {
    machine.mmu().attach_io1(g_acia);
  }

  /* Chip layout of the app, a prg load keeps the layout of the previous tune */
  usbsid::SidConfig & sid = machine.sid().config();
  sid.count = (a.sid2_base != 0) ? 2 : 1;
  for (uint8_t i = 0; i < usbsid::kMaxSids; i++) sid.base[i] = 0;
  sid.base[0] = 0xd400;
  sid.base[1] = a.sid2_base;

  /* Boots the KERNAL, pokes the prg in at its load address and types "run" */
  g_app = app;
  load_prg(a.prg, a.size, false);

  /* The KERNAL boot set $031a-$0333, the program has not used a file yet */
  if (drive != nullptr) drive->install();
}

void stop_c64app(void)
{
  stop_sidplayer();
  g_acia = nullptr;
  g_app = C64APP_COUNT;
}

unsigned int run_c64app(void)
{
  /* One instruction per call, a frame per call delays MIDI bytes by ~20ms and passes one byte per frame.
   * Machine::step_instruction() samples IRQ at the instruction boundary like a 6502 */
  if (g_acia == nullptr) return 0;
  usbsid::usplayer_machine().step_instruction();
  g_acia->emulate();
  return 0;
}

int running_c64app(void)
{
  return g_app;
}

#endif /* EMBEDDED */
