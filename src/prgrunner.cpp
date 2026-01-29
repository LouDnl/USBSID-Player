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
 * prgrunner.cpp
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

#include <cstring>
#include <cstdint>

#include <sys/time.h>
#include <stdio.h>

#include <c64util.h>
#include <mos6510_cpu.h>
#include <mos6581_8580_sid.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"

/* external USBSID variables */
#if DESKTOP
#include <USBSID.h>
extern USBSID_NS::USBSID_Class* usbsid;
#elif EMBEDDED
#include <config.h>
extern "C" {
// #include <sid.h>
void reset_sid(void);
int return_clockrate(void);
void apply_clockrate(int n_clock, bool suspend_sids);
extern Config usbsid_config;
extern RuntimeCFG cfg;
}
#endif
extern uint16_t sidone, sidtwo, sidthree, sidfour;
extern int pcbversion, fmoplsidno;
extern int sidssockone, sidssocktwo;
extern int sockonesidone, sockonesidtwo;
extern int socktwosidone, socktwosidtwo;
extern bool forcesockettwo;
extern int sidcount;
extern int sidno;

/* External emulator functions */
extern void emulate_c64(void);
extern void start_c64_test(void);
extern void emu_write_byte(uint16_t addr, uint8_t data);
extern uint8_t emu_dma_read_ram(uint16_t address);
extern uint8_t emu_dma_write_ram(uint16_t address, uint8_t data);

/* External emulator variables */
extern mos6510 *Cpu;
extern mos6581_8580 *SID;
extern bool
  log_instructions,
  log_timers,
  log_readwrites,
  log_sidrw,
  log_cia1rw,
  log_cia2rw,
  log_vicrw,
  log_vicrrw;

using namespace std;


#if DESKTOP
/**
 * @brief Start and run a PRG from file by filename
 *
 * @param fname
 */
void run_prg(string fname, bool loop)
#elif EMBEDDED
/**
 * @brief Start and run a PRG from a pointer by size
 *
 * @param binary_
 * @param binsize_
 */
void run_prg(uint8_t * binary_, size_t binsize_, bool loop)
#endif
{
  /* Init emulator vars */
  {
    sidcount = 1;
    sidone = 0xd400;
    // sidtwo = 0xd420;
    sidtwo = 0xd000;
    SID->sidcount = sidcount; /* Default is 1 */
    SID->sidno    = 0;        /* Default startnum */
    SID->sidone   = sidone;   /* Default */
    SID->sidtwo   = sidtwo;   /* Default */
  }

  /* Start loading file */
  MOSDBG("[PRG] Loading file\n");
#if DESKTOP
  FILE *f = fopen(fname.c_str(), "rb");

  if (f == NULL) {
    MOSDBG("[PRG] File not found, exiting.\n");
    return;
  }

  /* Copy SID data to RAM */
  // uint8_t file[0x10000]; /* 64KB */
  uint_least8_t *prg = new uint_least8_t[0x10000];

  size_t prg_size = fread(prg, 1, 0x10000, f);
#elif EMBEDDED
  uint_least8_t * prg = binary_;
  size_t prg_size = binsize_;
#endif
  MOSDBG("[PRG] File with size %u loaded into buffer\n", prg_size);
  uint_least16_t l_addr = (prg[0]|prg[1]<<8);
  uint_least16_t s_addr = (prg[2]|prg[3]<<8);
  MOSDBG("[PRG] Load address $%04x start address $%04x\n", l_addr, s_addr);

  if (log_instructions) Cpu->loginstructions = true;

  MOSDBG("[PRG] DMA copy binary to RAM start\n");
  // memcpy(RAM+l_addr,prg+2,prg_size);
  for(int i = l_addr; i < (l_addr+prg_size); i++) {
    emu_dma_write_ram(i,prg[(i-l_addr)+2]);
  }
  MOSDBG("[PRG] DMA copy binary to RAM finished\n");

  {
    MOSDBG("[PRG] Start memory configuration\n");
    uint_least8_t run_lo = (s_addr & 0xff)+0x02;
    uint_least8_t run_hi = ((s_addr >> 8) & 0xff);

    /* Break routine address */
    emu_dma_write_ram(0x0316, run_lo); /* BRK lo */
    emu_dma_write_ram(0x0317, run_hi); /* BRK hi */
    /* Reset vector execution address */
    // emu_dma_write_ram(0xfffc, run_lo); /* RST lo */
    // emu_dma_write_ram(0xfffd, run_hi); /* RST hi */

    /* Video standard */
    emu_dma_write_ram(0x02A6, 0x00); /* PAL */

    // emu_dma_write_ram(0x030c, song_number); /* songno */

    emu_write_byte(0xd011, 0x0b); /* Set raster IRQ hi to 0x000 */
    emu_write_byte(0xd012, 0xfe); /* Set raster IRQ lo to 0xfe */
    emu_write_byte(0xd01a, 0x00); /* Disable VIC raster IRQ */

    emu_write_byte(0xdc0d, 0x7f); /* Cia 1 disable IRQ's */
    emu_write_byte(0xdc0e, 0x00); /* Cia 1 disable timer a */
    emu_write_byte(0xdc0f, 0x00); /* Cia 1 disable timer b */
    emu_write_byte(0xdc04, 0x25); /* Cia 1 prescaler a lo */
    emu_write_byte(0xdc05, 0x40); /* Cia 1 prescaler a hi and force load */
    emu_write_byte(0xdc06, 0xff); /* Cia 1 prescaler b lo */
    emu_write_byte(0xdc07, 0xff); /* Cia 1 prescaler b hi and force load */
    emu_write_byte(0xdc0d, 0x81); /* Cia 1 enable timer a IRQ */
    emu_write_byte(0xdc0e, 0x01); /* Cia 1 enable timer a */

    emu_write_byte(0xdd0d, 0x7f); /* Cia 2 disable IRQ's */
    emu_write_byte(0xdd0e, 0x00); /* Cia 2 disable timer a */
    emu_write_byte(0xdd0f, 0x00); /* Cia 2 disable timer b */
    emu_write_byte(0xdd04, 0xff); /* Cia 2 prescaler a lo */
    emu_write_byte(0xdd05, 0xff); /* Cia 2 prescaler a hi and force load */
    emu_write_byte(0xdd06, 0xff); /* Cia 2 prescaler b lo */
    emu_write_byte(0xdd07, 0xff); /* Cia 2 prescaler b hi and force load */
    emu_write_byte(0xdd0d, 0x08); /* Cia 2 start/stop */
    emu_write_byte(0xdd0e, 0x08); /* Cia 2 start/stop */

    emu_write_byte(0x0001, 0x37); /* Switch banks */

    MOSDBG("[PRG] End memory configuration\n");
  }
  Cpu->dump_flags();
  Cpu->reset();
  Cpu->pc(s_addr+0x02);  Cpu->dump_flags();

  if (loop) {
    MOSDBG("[emulate_c64]\n");
    emulate_c64();
  }

  return;
}

/**
 * @brief Start 6502_functional_test binary
 *
 */
void start_test(void)
{
  Cpu->reset();
  if (log_instructions) Cpu->loginstructions = true;
  MOSDBG("[MEM] $0000:%2x\n",emu_dma_read_ram(0));
  MOSDBG("[MEM] $0001:%2x\n",emu_dma_read_ram(1));
  start_c64_test(); /* Starts binary test */
}

#pragma GCC diagnostic pop
