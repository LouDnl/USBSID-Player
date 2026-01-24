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
 * vsidpsid.cpp
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
#include <mos6560_6561_vic.h>
#include <mos6581_8580_sid.h>


/* External emulator functions */
extern void log_logs(void);
extern void getinfo_USBSID(int clockspeed);
extern uint8_t emu_read_byte(uint16_t addr);
extern void emu_write_byte(uint16_t addr, uint8_t data);
extern void emulate_c64(void);

/* External emulator variables */
extern mos6510 *Cpu;
extern mos6560_6561 *Vic;
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

/* external USBSID variables */
#if DESKTOP
#include <USBSID.h>
extern USBSID_NS::USBSID_Class* usbsid;
#endif
extern uint16_t sidone, sidtwo, sidthree, sidfour;
extern int pcbversion, fmoplsidno;
extern int sidssockone, sidssocktwo;
extern int sockonesidone, sockonesidtwo;
extern int socktwosidone, socktwosidtwo;
extern bool forcesockettwo;
extern int sidcount;
extern int sidno;

/* VSID PSID variables */
extern int numsids, sid2loc, sid3loc;


/**
 * @brief Start VSID PSID player
 *
 * @param is_pal
 */
void start_vsid_player(bool is_pal)
{
#if DESKTOP
  if (usbsid) {
#elif EMBEDDED
  {
#endif
    getinfo_USBSID((is_pal?985248:1022727));
    /* USBSID related variables and defaults */
    SID->fmoplsidno = fmoplsidno;
    SID->sidssockone = sidssockone;
    SID->sidssocktwo = sidssocktwo;
    SID->sockonesidone = sockonesidone;
    SID->sockonesidtwo = sockonesidtwo;
    SID->socktwosidone = socktwosidone;
    SID->socktwosidtwo = socktwosidtwo;
    SID->forcesockettwo = forcesockettwo;
  }

  /* mos6581_8580 */
  SID->sidcount = numsids; /* Default is 1 */
  SID->sidno    = 0;       /* Default startnum */
  SID->sidone   = 0xd400;  /* Default */
  SID->sidtwo   = sid2loc;
  SID->sidthree = sid3loc;
  SID->sidfour  = 0xd000;  /* No 4x SID in psiddrv yet :-( */

  bool pal_system = ((emu_read_byte(0x02a6) == 0) ? false : true);

  MOSDBG("[USPLAYER] is_pal: %d pal_system: %d\n",is_pal,pal_system);

  pal_system = (is_pal != pal_system ? pal_system : is_pal);

  /* mos6560_6561 */
  Vic->cycles_per_sec = (pal_system ? 985248 : 1022727);
  Vic->refresh_rate = (double)(pal_system ? 19656 : 17096);
  Vic->refresh_frequency = (double)((double)Vic->cycles_per_sec / (double)Vic->refresh_rate);
  Vic->raster_lines = (pal_system ? 312 : 263);
  Vic->raster_row_cycles = (pal_system ? 63 : 65);;
  Vic->set_timer_speed(100);
#if DESKTOP
  usbsid->USBSID_SetClockRate(Vic->cycles_per_sec, true);
#elif EMBEDDED
  /* TODO: Do something */
#endif

  SID->print_settings();

  MOSDBG("[VIC] RL:%u RRC:%u\n",Vic->raster_lines,Vic->raster_row_cycles);

  if (log_instructions) Cpu->loginstructions = true;
  log_logs();

  /* Definitions confirming to sidfileformat.txt */
  uint_least8_t pal_lo = 0x25;
  uint_least8_t pal_hi = 0x40;
  uint_least8_t ntsc_lo = 0x42;
  uint_least8_t ntsc_hi = 0x95;


  emu_write_byte(0xd011, 0x1b); /* VIC set raster IRQ hi to 0x100 */
  emu_write_byte(0xd012, 0x37); /* VIC set raster IRQ lo to 0x37 */
  emu_write_byte(0xd01a, 0x00); /* VIC disable raster IRQ */

  emu_write_byte(0xdc0d, 0x7f); /* Cia 1 disable IRQ's */
  emu_write_byte(0xdc0e, 0x80); /* Cia 1 disable timer a */
  emu_write_byte(0xdc0f, 0x00); /* Cia 1 disable timer b */
  emu_write_byte(0xdc04, (pal_system ? pal_lo : ntsc_lo)); /* Cia 1 prescaler a lo */
  emu_write_byte(0xdc05, (pal_system ? pal_hi : ntsc_hi)); /* Cia 1 prescaler a hi and force load */
  emu_write_byte(0xdc06, 0xff); /* Cia 1 prescaler b lo */
  emu_write_byte(0xdc07, 0xff); /* Cia 1 prescaler b hi and force load */
  emu_write_byte(0xdc0d, 0x81); /* Cia 1 enable timer a IRQ */
  emu_write_byte(0xdc0e, 0x81); /* Cia 1 enable timer a with TOD @ 60Hz */

  emu_write_byte(0xdd0d, 0x7f); /* Cia 2 disable IRQ's */
  emu_write_byte(0xdd0e, 0x80); /* Cia 2 disable timer a */
  emu_write_byte(0xdd0f, 0x00); /* Cia 2 disable timer b */
  emu_write_byte(0xdd04, 0xff); /* Cia 2 prescaler a lo */
  emu_write_byte(0xdd05, 0xff); /* Cia 2 prescaler a hi and force load */
  emu_write_byte(0xdd06, 0xff); /* Cia 2 prescaler b lo */
  emu_write_byte(0xdd07, 0xff); /* Cia 2 prescaler b hi and force load */

  emu_write_byte(0x0001, 0x37); /* Switch banks */

  Cpu->reset();

  // MOSDBG("$fffa:$%02x%02x $fffc:$%02x%02x $fffe:$%02x%02x\n",
  //  emu_dma_read_ram(0xfffa),emu_dma_read_ram(0xfffb),
  //  emu_dma_read_ram(0xfffc),emu_dma_read_ram(0xfffd),
  //  emu_dma_read_ram(0xfffe),emu_dma_read_ram(0xffff)
  // );
  // /* Install default NMI/RESET/IRQ vectors */
  // emu_dma_write_ram(0x0316, 0xe2); /* BRK lo */
  // emu_dma_write_ram(0x0317, 0xfc); /* BRK hi */
  // emu_dma_write_ram(0xfffa, 0x43);
  // emu_dma_write_ram(0xfffb, 0xfe);
  // emu_dma_write_ram(0xfffc, 0xe2);
  // emu_dma_write_ram(0xfffd, 0xfc);
  // emu_dma_write_ram(0xfffe, 0x48);
  // emu_dma_write_ram(0xffff, 0xff);

  // MOSDBG("$fffa:$%02x%02x $fffc:$%02x%02x $fffe:$%02x%02x\n",
  //  emu_dma_read_ram(0xfffa),emu_dma_read_ram(0xfffb),
  //  emu_dma_read_ram(0xfffc),emu_dma_read_ram(0xfffd),
  //  emu_dma_read_ram(0xfffe),emu_dma_read_ram(0xffff)
  // );

  MOSDBG("[emulate_c64]\n");
  emulate_c64();

}
