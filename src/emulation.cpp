/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * Parts if this emulator are based on other great emulators and players
 * like Vice, SidplayFp, Websid, SidBerry and emudore/adorable
 *
 * emulation.cpp
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025-2026 LouD
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

#include <bitset>
#include <sstream>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <ios>
#include <chrono>
#include <thread>

#include <sys/time.h>
#include <signal.h>

#include <constants.h>

#include <mos6510_cpu.h>
#include <mos6526_cia.h>
#include <mos6581_8580_sid.h>
#include <mos6560_6561_vic.h>
#include <mos906114_pla.h>
#include <mmu.h>

#include <sidfile.h>

#include <c64util.h>
#include <emulation.h>

#if DESKTOP && DEBUGGER_SUPPORT
#include <debugger.h>
#endif

#if DESKTOP
#include <USBSID.h>
extern USBSID_NS::USBSID_Class* usbsid;
#endif


#define CIA1_ADDRESS 0xDC00
#define CIA2_ADDRESS 0xDD00
/* External player variables */
extern volatile sig_atomic_t stop;
extern uint8_t RAM[];

/* C64 Variables */
mos6510 *Cpu;
mos6526 *Cia1;
mos6526 *Cia2;
mos6560_6561 *Vic;
mos906114 *Pla;
mos6581_8580 *SID;
mmu *MMU;
#if DESKTOP && DEBUGGER_SUPPORT
Debugger *r2;
#endif

/* Emulation variables */
bool enable_r2 = false;
bool force_psiddrv = false;
bool log_instructions = false;
bool log_timers = false;
bool log_pla = false;
bool log_readwrites = false;
bool log_romrw = false;
bool log_vicrw = false;
bool log_vicrrw = false;
bool log_cia1rw = false;
bool log_cia2rw = false;
bool log_sidrw = false;

void log_logs(void)
{ /* LOL :-) */
  MOSDBG("[ARGS] %d%d%d%d%d%d%d%d\n",
    log_sidrw,
    log_cia1rw,
    log_cia2rw,
    log_vicrw,
    log_vicrrw,
    log_readwrites,
    log_instructions,
    log_timers
  );
}

/**
 * @brief Wrapper around MMU->read_byte()
 *
 * @param address
 * @return uint8_t
 */
uint8_t emu_read_byte(uint16_t address)
{
  return MMU->read_byte(address);
}

/**
 * @brief Wrapper around MMU->write_byte()
 *
 * @param address
 * @param data
 */
void emu_write_byte(uint16_t address, uint8_t data)
{
  MMU->write_byte(address, data);
  return;
}

/**
 * @brief Wrapper around MMU->vic_read_byte()
 *
 * @param address
 * @return uint8_t
 */
uint8_t emu_vic_read_byte(uint16_t address)
{
  return MMU->vic_read_byte(address);
}

/* Emulator init / de-init */
void emu_init(void)
{
  MOSDBG("[C64] Init\n");
  Cpu = new mos6510(emu_read_byte, emu_write_byte);
  MOSDBG("[CPU] created\n");
  Pla = new mos906114();
  MOSDBG("[PLA] created\n");
  Vic = new mos6560_6561();
  MOSDBG("[VIC] created\n");
  Cia1 = new mos6526(CIA1_ADDRESS);
  MOSDBG("[CIA] 1 created\n");
  Cia2 = new mos6526(CIA2_ADDRESS);
  MOSDBG("[CIA] 2 created\n");
  SID = new mos6581_8580();
  MOSDBG("[SID] created\n");
  MMU = new mmu();
  MOSDBG("[MMU] created\n");
  Cia1->glue_c64(Cpu);
  Cia2->glue_c64(Cpu);
  Vic->glue_c64(emu_vic_read_byte,Cpu,SID);
  Pla->glue_c64(Cpu);
  Cpu->glue_c64(Vic,Cia1,Cia2);
  MMU->glue_c64(Cpu,Pla,Vic,Cia1,Cia2,SID);
  MOSDBG("[C64] glued\n");

  mos6510::loginstructions = log_instructions;

  MMU->log_pla = log_pla;
  MMU->log_readwrites = log_readwrites;
  MMU->log_romrw = log_romrw;
  MMU->log_cia1rw = log_cia1rw;
  MMU->log_cia2rw = log_cia2rw;
  MMU->log_vicrw = log_vicrw;
  MMU->log_vicrrw = log_vicrrw;

  SID->log_sidrw = log_sidrw;

  /* r2 support */
  #if DEBUGGER_SUPPORT
  if (enable_r2) {
    r2 = new Debugger();
    r2->glue_c64(Cpu,MMU);
  }
  #endif

  return;
}

void emu_deinit(void)
{
  MOSDBG("[C64] Deinit\n");
  if (MMU) delete MMU;
  if (SID) delete SID;
  if (Pla) delete Pla;
  if (Cia1) delete Cia1;
  if (Cia2) delete Cia2;
  if (Vic) delete Vic;
  delete Cpu;

  return;
}

void cycle_callback(mos6510* cpu)
{
  MOSDBG("POO! %u\n",cpu->cycles());
  return;
}

void emulate_c64_upto(uint_least16_t pc)
{
  while (!stop) {
    if (Cpu->pc() == pc) break;
    Cpu->emulate();
    Vic->emulate();
    Cia1->emulate();
    Cia2->emulate();
  }
  MOSDBG("[CPU] PC $%04x reached!\n",pc);

  return;
}

void emulate_until_opcode(uint_least8_t opcode)
{
  while (!stop) {
    Cpu->emulate();
    Vic->emulate();
    Cia1->emulate();
    Cia2->emulate();
    if (Cpu->last_insn == opcode) { return; }
  }

  return;
}

void emulate_until_rti(void)
{
  while (!stop) {
    Cpu->emulate();
    Vic->emulate();
    Cia1->emulate();
    Cia2->emulate();
    if (Cpu->last_insn == 0x40) { return; }
  }

  return;
}

void emulate_c64(void)
{
  log_logs();
  while (!stop) {
    if (enable_r2) { r2->emulate(); }
    Vic->emulate();
    Cia1->emulate();
    Cia2->emulate();
    Cpu->emulate();
    if __unlikely (log_timers) {
      Cia1->dump_timers();
      Cia2->dump_timers();
      Vic->dump_timers();
      MOSDBG("\n");
    }
  }

  return;
}

void start_c64_test(void) /* Finishes successfully */
{
  log_logs();

  char b;
  uint16_t pc = 0;
  uint16_t startaddr = 0x400;

  /* unmap C64 ROMs */
  emu_write_byte(pAddrMemoryLayout, 0);
  /* load tests into RAM */
  #include <6502_functional_test.h>
  memcpy(RAM+startaddr,functional_6502_test,count_of(functional_6502_test));
  Cpu->pc(0x400); /* Fix address at $400 for binary test*/

  emulate_c64_upto(0x3463);


  MOSDBG("Test exiting\n");
  exit(1);
}
