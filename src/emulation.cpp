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
#include <wrappers.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"

/* USBSID variables */
#if DESKTOP
#include <USBSID.h>
USBSID_NS::USBSID_Class* usbsid;
#elif EMBEDDED
#include <config.h>
extern "C" {
void reset_sid(void);
void reset_sid_registers(void);
int return_clockrate(void);
void apply_clockrate(int n_clock, bool suspend_sids);
extern Config usbsid_config;
extern RuntimeCFG cfg;
}
#endif
int pcbversion = -1;
int fmoplsidno = -1;
uint16_t sidone, sidtwo, sidthree, sidfour;
int sidssockone = 0, sidssocktwo = 0;
int sockonesidone = 0, sockonesidtwo = 0;
int socktwosidone = 0, socktwosidtwo = 0;
bool forcesockettwo = false; /* force play on socket two */
int sidcount = 1;
int sidno = 0;

#define CIA1_ADDRESS 0xDC00
#define CIA2_ADDRESS 0xDD00
/* Emulation variables */
#if DESKTOP
volatile sig_atomic_t stop;
volatile sig_atomic_t playing;
volatile sig_atomic_t paused;
volatile sig_atomic_t vsidpsid;
#elif EMBEDDED
volatile bool stop;
volatile bool playing;
volatile bool paused;
volatile bool vsidpsid;
#endif

/* C64 Variables */
mos6510 *Cpu;
mos6526 *Cia1;
mos6526 *Cia2;
mos6560_6561 *Vic;
mos906114 *Pla;
mos6581_8580 *SID;
mmu *MMU;

/* Emulation variables */
bool threaded = true;
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

/* VSIDPSID external functions */
extern void next_prev_tune(bool next);

/* Pre declarations */
void emulate_c64_single(void);


#if DESKTOP
int setup_USBSID(void)
{
  usbsid = new USBSID_NS::USBSID_Class();

  MOSDBG("[USBSID] Opening with buffer for cycle exact writing\n");
  if (usbsid->USBSID_Init(true, true) < 0) {
    MOSDBG("USBSID-Pico not found, exiting\n");
    return 0;
  }
  /* Sleep 400ms for Reset to settle */
  struct timespec tv = { .tv_sec = 0, .tv_nsec = (400 * 1000 * 1000) };
  nanosleep(&tv, &tv);

  return 1;
}
#endif

void getinfo_USBSID(int clockspeed)
{
#if DESKTOP
  if(usbsid->USBSID_GetClockRate() != clockspeed) {
    usbsid->USBSID_SetClockRate(clockspeed, true);
  }

  if(usbsid->USBSID_GetNumSIDs() < sidcount) {
    MOSDBG("[WARNING] Tune no.sids %d is higher then USBSID-Pico no.sids %d\n", sidcount, usbsid->USBSID_GetNumSIDs());
  }

  uint8_t socket_config[10];
  usbsid->USBSID_GetSocketConfig(socket_config);
  MOSDBG("[USBSID] SOCKET CONFIG: ");
  for (int i = 0; i < 10; i++) {
    MOSDBG("%02X ", socket_config[i]);
  }
  MOSDBG("\n");

  MOSDBG("[USBSID] SOCK1#.%d SID1:%d SID2:%d\n[USBSID] SOCK2#.%d SID1:%d SID2:%d\n",
    usbsid->USBSID_GetSocketNumSIDS(1, socket_config),
    usbsid->USBSID_GetSocketSIDType1(1, socket_config),
    usbsid->USBSID_GetSocketSIDType2(1, socket_config),
    usbsid->USBSID_GetSocketNumSIDS(2, socket_config),
    usbsid->USBSID_GetSocketSIDType1(2, socket_config),
    usbsid->USBSID_GetSocketSIDType2(2, socket_config)
  );

  sidssockone = usbsid->USBSID_GetSocketNumSIDS(1, socket_config);
  sidssocktwo = usbsid->USBSID_GetSocketNumSIDS(2, socket_config);
  sockonesidone = usbsid->USBSID_GetSocketSIDType1(1, socket_config);
  sockonesidtwo = usbsid->USBSID_GetSocketSIDType2(1, socket_config);
  socktwosidone = usbsid->USBSID_GetSocketSIDType1(2, socket_config);
  socktwosidtwo = usbsid->USBSID_GetSocketSIDType2(2, socket_config);
  fmoplsidno = usbsid->USBSID_GetFMOplSID();
  pcbversion = usbsid->USBSID_GetPCBVersion();
#elif EMBEDDED
  int clockrates[] = { 1000000, 985248, 1022727, 1023440, 1022730 };
  if(usbsid_config.clock_rate != clockspeed) {
    for (uint i = 0; i < count_of(clockrates); i++) {
      if (clockrates[i] == clockspeed) {
        apply_clockrate(i, true);
      }
    }
  }

  if(cfg.numsids < sidcount) {
    MOSDBG("[WARNING] Tune no.sids %d is higher then USBSID-Pico no.sids %d\n", sidcount, cfg.numsids);
  }

  MOSDBG("[USBSID] SOCK1#.%d SID1:%d SID2:%d\n[USBSID] SOCK2#.%d SID1:%d SID2:%d\n",
    cfg.sids_one, usbsid_config.socketOne.sid1.type, usbsid_config.socketOne.sid2.type,
    cfg.sids_two, usbsid_config.socketTwo.sid1.type, usbsid_config.socketTwo.sid2.type
  );

  sidssockone = cfg.sids_one;
  sidssocktwo = cfg.sids_two;
  sockonesidone = usbsid_config.socketOne.sid1.type;
  sockonesidtwo = usbsid_config.socketOne.sid2.type;
  socktwosidone = usbsid_config.socketTwo.sid1.type;
  socktwosidtwo = usbsid_config.socketTwo.sid2.type;
  fmoplsidno = cfg.fmopl_sid;
#endif

  return;
}


void hardwaresid_init(void)
{
  MOSDBG("[HARDWARESID] Init\n");
#if DESKTOP
  if (!setup_USBSID()) { usbsid = nullptr; }
  if (usbsid) {
    usbsid->USBSID_ResetAllRegisters();
    usbsid->USBSID_Reset();
  }
#elif EMBEDDED
  reset_sid();
  emu_sleep_us(100);
  reset_sid_registers();
  emu_sleep_us(100);
#endif
  return;
}

void hardwaresid_deinit(void)
{
  MOSDBG("[HARDWARESID] Deinit\n");
#if DESKTOP
  if (usbsid) {
    usbsid->USBSID_Flush();
    usbsid->USBSID_DisableThread();
    usbsid->USBSID_ResetAllRegisters();
    usbsid->USBSID_Reset();
    delete usbsid;
  }
#elif EMBEDDED
  reset_sid_registers();
  emu_sleep_us(100);
  reset_sid();
  emu_sleep_us(100);
#endif

  return;
}

/**
 * @brief log the logs, talk the talks?
 *
 */
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
 * @brief Send keyboard command to emulator for pause
 */
void emu_pause_playing(bool pause)
{
  if (vsidpsid) {
    paused = pause;
#if DESKTOP
    if (paused) usbsid->USBSID_Mute();
    else usbsid->USBSID_UnMute();
#endif
  } else {
    /* This is actually not a pause but a stop command */
    Cia1->write_prab_bits(row_bit_runstop,col_bit_runstop,true);
    emu_sleep_us((uint64_t)Vic->refresh_rate);
    Cia1->write_prab_bits(row_bit_runstop,col_bit_runstop,false);
  }
  return;
}

/**
 * @brief Send keyboard command to emulator for next subtune
 *
 */
void emu_next_subtune(void)
{
  if (vsidpsid) {
    MOSDBG("[EMU] Next tune SID\n");
    next_prev_tune(true);
  } else {
    MOSDBG("[EMU] Next tune PRG\n");
    Cia1->write_prab_bits(row_bit_plus,col_bit_plus,true);
#if DESKTOP /* On DESKTOP we can just do a simple refresh rate long sleep */
    emu_sleep_us((uint64_t)Vic->refresh_rate);
#elif EMBEDDED /* On EMBEDDED we need the emulator to finish the current frame instead of sleeping */
    uint64_t start_cycles = Cpu->cycles();
    while (Cpu->cycles() - start_cycles < (uint64_t)Vic->refresh_rate) {
        emulate_c64_single();
    }
#endif
    Cia1->write_prab_bits(row_bit_plus,col_bit_plus,false);
  }
  return;
}

/**
 * @brief Send keyboard command to emulator for previous subtune
 *
 */
void emu_previous_subtune(void)
{
  if (vsidpsid) {
    MOSDBG("[EMU] Previous tune SID\n");
    next_prev_tune(false);
  } else {
    MOSDBG("[EMU] Previous tune PRG\n");
    Cia1->write_prab_bits(row_bit_minus,col_bit_minus,true);
#if DESKTOP /* On DESKTOP we can just do a simple refresh rate long sleep */
    emu_sleep_us((uint64_t)Vic->refresh_rate);
#elif EMBEDDED /* On EMBEDDED we need the emulator to finish the current frame instead of sleeping */
    uint64_t start_cycles = Cpu->cycles();
    while (Cpu->cycles() - start_cycles < (uint64_t)Vic->refresh_rate) {
        emulate_c64_single();
    }
#endif
    Cia1->write_prab_bits(row_bit_minus,col_bit_minus,false);
  }
  return;
}

/**
 * @brief Wrapper around MMU->dma_read_ram()
 *
 * @param address
 * @return uint8_t
 */
uint8_t emu_dma_read_ram(uint16_t address)
{
  return MMU->dma_read_ram(address);
}

/**
 * @brief Wrapper around MMU->dma_write_ram()
 *
 * @param address
 * @param data
 * @return uint8_t
 */
void emu_dma_write_ram(uint16_t address, uint8_t data)
{
  MMU->dma_write_ram(address,data);
  return ;
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

/**
 * @brief Reset the player state between restarts
 */
void reset_player_state(void)
{
    /* Reset all global flags to their initial state */
    stop = false;
    playing = false;
    paused = false;
    vsidpsid = false;

    /* Reset SID-related variables */
    sidcount = 1;
    sidno = 0;
    sidssockone = 0;
    sidssocktwo = 0;
    sockonesidone = 0;
    sockonesidtwo = 0;
    socktwosidone = 0;
    socktwosidtwo = 0;
    fmoplsidno = -1;
    pcbversion = -1;

    /* Reset any SID-specific state */
    sidone = 0;
    sidtwo = 0;
    sidthree = 0;
    sidfour = 0;

    /* Reset socket flags */
    forcesockettwo = false;
}

/**
 * @brief Emulator init
 *
 */
void emu_init(void)
{
  MOSDBG("[C64] Init\n");

  /* Reset state to ensure clean start when embedding */
  reset_player_state();

  MMU = new mmu();
  Cpu = new mos6510(emu_read_byte, emu_write_byte);
  Pla = new mos906114(MMU);
  Vic = new mos6560_6561();
  Cia1 = new mos6526(CIA1_ADDRESS);
  Cia2 = new mos6526(CIA2_ADDRESS);
  SID = new mos6581_8580();
  Cia1->glue_c64(Cpu);
  Cia2->glue_c64(Cpu);
  Vic->glue_c64(emu_vic_read_byte,Cpu,SID);
  Pla->glue_c64(Cpu);
  Cpu->glue_c64(MMU,Vic,Cia1,Cia2);
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

  playing = true;
  return;
}

/**
 * @brief Emulator deinit
 *
 */
void emu_deinit(void)
{
  MOSDBG("[C64] Deinit\n");
  stop = true; /* Make sure we're stopped if not already */

  /* Required or the player will not restart when embedding */
  Pla->reset();
  Cia1->reset();
  Cia2->reset();
  Vic->reset();
  Cpu->reset();

  /* Delete all objects */
  delete SID;
  delete Pla;
  delete Cia1;
  delete Cia2;
  delete Vic;
  delete Cpu;
  delete MMU;

  /* Nullify all objects */
  SID = NULL;
  Pla = NULL;
  Cia1 = NULL;
  Cia2 = NULL;
  Vic = NULL;
  Cpu = NULL;
  MMU = NULL;

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

void emulate_c64_single(void)
{
  if (!stop) {
    Cpu->emulate();
    Vic->emulate();
    Cia1->emulate();
    Cia2->emulate();
  }
  return;
}

void emulate_c64(void)
{
  log_logs();
  while (!stop) {
#if DESKTOP
    while (paused){}
#endif
    Cpu->emulate();
    Vic->emulate();
    Cia1->emulate();
    Cia2->emulate();
#if DESKTOP
    if __unlikely (log_timers) {
      Cia1->dump_timers();
      Cia2->dump_timers();
      Vic->dump_timers();
      MOSDBG("\n");
    }
#endif
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
  for(int i = startaddr; i < count_of(functional_6502_test); i++) {
    emu_dma_write_ram(i,functional_6502_test[(i-startaddr)]);
  }
  Cpu->pc(0x400); /* Fix address at $400 for binary test*/

  emulate_c64_upto(0x3463);


  MOSDBG("Test exiting\n");
  exit(1);
}

#pragma GCC diagnostic pop
