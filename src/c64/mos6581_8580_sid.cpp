/*
 * usbsid->USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with usbsid->USBSID-Pico. usbsid->USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * mos6581_8580_sid.cpp
 * This file is part of usbsid->USBSID-Player (https://github.com/LouDnl/usbsid->USBSID-Player)
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
/* Random shit */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <mos6581_8580_sid.h>
#include <mos6510_cpu.h>

#include <c64util.h>
#include <constants.h>

#if DESKTOP
#include <USBSID.h>
extern USBSID_NS::USBSID_Class* usbsid;
#endif


/**
 * @brief Construct a new mos6581 8580::mos6581 8580 object
 *
 */
mos6581_8580::mos6581_8580()
{
  MOSDBG("[SID] Init\n");

  srand(time(NULL));
}

/**
 * @brief Glue required C64 parts
 *
 */
void mos6581_8580::glue_c64(mos6510 * _cpu)
{
  cpu = _cpu;

  return;
}

/**
 * @brief Some tunes write to a mirror address instead of regular $d400
 *        This function provides a work-around
 *        Example: 5-Channel-digi-tune writes to $d5c0
 *
 * @param addr
 * @return true
 * @return false
 */
bool mos6581_8580::custom_sidaddr_check(uint16_t addr)
{
  uint16_t test_addr = (addr & 0xfff0);
  switch (test_addr) {
    case 0xd5c0 ... 0xd5df:
      return true;
    default:
      return false;
  }
  return false;
}

uint8_t mos6581_8580::sidaddr_translation(uint16_t addr)
{
  uint8_t sock2add = (forcesockettwo ? (sidssockone == 1 ? 0x20 : sidssockone == 2 ? 0x40 : 0x0) : 0x0);
  if (addr == 0xDF40 || addr == 0xDF50) {
    if (fmoplsidno >= 1 && fmoplsidno <=4) {
      sidno = fmoplsidno;
      return (((sidno - 1) * 0x20) + (addr & 0x1F));
    } else {
      sidno = 5; /* Skip */
      return (0x80 + (addr & 0x1F)); /* Out of scope address */
    }
  }
  switch (sidcount) {
    case 1: /* Easy just return the address */
      if (addr >= sidone && addr < (sidone + 0x1F)
         || custom_sidaddr_check(addr)) {
        sidno = 1;
        return (sock2add + (addr & 0x1F));
      };
      break;
    case 2: /* Intermediate remap all above 0x20 to sidno 2 */
      if (addr >= sidone && addr < (sidone + 0x1F)) {
        sidno = 1;
        return (addr & 0x1F);
      } else if (addr >= sidtwo && addr < (sidtwo + 0x1F)) {
        sidno = 2;
        return (0x20 + (addr & 0x1F));
      };
      break;
    case 3: /* Advanced */
      if (addr >= sidone && addr < (sidone + 0x1F)) {
        sidno = 1;
        return (addr & 0x1F);
      } else if (addr >= sidtwo && addr < (sidtwo + 0x1F)) {
        sidno = 2;
        return (0x20 + (addr & 0x1F));
      } else if (addr >= sidthree && addr < (sidthree + 0x1F)) {
        sidno = 3;
        return (0x40 + (addr & 0x1F));
      };
      break;
    case 4: /* Expert */
      if (addr >= sidone && addr < (sidone + 0x1F)) {
        sidno = 1;
        return (addr & 0x1F);
      } else if (addr >= sidtwo && addr < (sidtwo + 0x1F)) {
        sidno = 2;
        return (0x20 + (addr & 0x1F));
      } else if (addr >= sidthree && addr < (sidthree + 0x1F)) {
        sidno = 3;
        return (0x40 + (addr & 0x1F));
      } else if (addr >= sidfour && addr < (sidfour + 0x1F)) {
        sidno = 4;
        return (0x60 + (addr & 0x1F));
      };
      break;
    default:
      break;
  };
  return 0xFE;
}

/**
 * @brief Flush USBSID, called at the end of VSYNC
 *
 * @return * void
 */
void mos6581_8580::sid_flush(void)
{
  const CPUCLOCK now = cpu->cycles();
  CPUCLOCK cycles = (now - sid_main_clk);

  usbsid->USBSID_SetFlush(); /* Always flush USB data buffer when called */
  if (now < sid_main_clk || w_cyclecount == 0) { /* Reset / flush */
    r_cyclecount = 0;
    w_cyclecount = 0;
    sid_main_clk = now;
    return;
  }
  while(cycles > 0xFFFF) {
    cycles -= 0xFFFF;
  }
  /* Delay for any remaining cycles */
  if (usbsid) {
    usbsid->USBSID_WaitForCycle(cycles);
  }

  sid_main_clk = flush_main_clk = now;
  r_cyclecount = w_cyclecount = 0;
  return;
}

/**
 * @brief Calculate delay cycles from cpu cycles since last read/write
 *
 * @return unsigned int
 */
unsigned int mos6581_8580::sid_delay(void)
{
  CPUCLOCK now = cpu->cycles();
  CPUCLOCK cycles = (now - sid_main_clk);
  while (cycles > 0xFFFF) {
    cycles -= 0xFFFF;
    usbsid->USBSID_WaitForCycle(0xFFFF);
  }
  sid_main_clk = now;
  return cycles;
}

/**
 * @brief Read data from SID address
 *
 * @param addr
 * @return uint8_t
 */
uint8_t mos6581_8580::read_sid(uint16_t addr)
{
  uint8_t data = (rand() % 0xFF) + 1; /* Random value generator */
  uint8_t phyaddr = (sidaddr_translation(addr) & 0xFF);  /* 4 SIDs max */
  uint_fast16_t cycles = sid_delay();
  if (phyaddr == 0xFE) data = RAM[addr];
  if (log_sidrw) {
    printf("[R SID%d] $%04x $%02x:%02x [C]%5u\n",
      sidno,addr,phyaddr,data,cycles);
  }
  r_cyclecount += cycles;
  return data;
}

/**
 * @brief Write data to SID address
 *
 * @param addr
 * @param data
 */
void mos6581_8580::write_sid(uint16_t addr, uint8_t data)
{
  uint8_t phyaddr = (sidaddr_translation(addr) & 0xFF);  /* 4 SIDs max */
  uint_fast16_t cycles = sid_delay();
  if (usbsid && (phyaddr != 0xFE)) {
    usbsid->USBSID_WaitForCycle(cycles);
    usbsid->USBSID_WriteRingCycled(phyaddr, data, cycles);
  }
  RAM[addr] = data; /* Always write to RAM as mirror */
  if (log_sidrw) {
    printf("[W SID%d] $%04x $%02x:%02x [C]%5u\n",
      sidno,addr,phyaddr,data,cycles);
  }
  w_cyclecount += cycles;

  return;
}

/**
 * @brief Prints out the current SID settings
 *
 */
void mos6581_8580::print_settings(void)
{
  MOSDBG("[SID] NUM%d #%d 1$%04x 2$%04x 3$%04x 4$%04x FM%d SOCK1:%d SOCK2:%d s1s1:%d s1s2:%d s2s1:%d s2s2:%d FSOCK2:%d\n",
    /* SID related defaults */
    sidcount,
    sidno,
    sidone,
    sidtwo, /* 0xd420 */
    sidthree, /* 0xd440 */
    sidfour, /* 0xd460 */
    /* USBSID related variables and defaults */
    fmoplsidno,
    sidssockone,
    sidssocktwo,
    sockonesidone,
    sockonesidtwo,
    socktwosidone,
    socktwosidtwo,
    forcesockettwo);

  return;
}
