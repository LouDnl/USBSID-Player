/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * Parts if this emulator are based on other great emulators and players
 * like Vice, SidplayFp, Websid and emudore/adorable
 *
 * mmu.cpp
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

#include <cstdint>
#include <cstring>

#include <c64util.h>
#include <constants.h>

#include <mos6510_cpu.h>
#include <mos6526_cia.h>
#include <mos6560_6561_vic.h>
#include <mos6581_8580_sid.h>
#include <mos906114_pla.h>
#include <mmu.h>

#if EMBEDDED
#include <pico.h>
#endif
#include <basic.901226-01.h> /* basic_901226_01 */
#include <characters.901225-01.h> /* characters_901225_01 */
#include <kernal.901227-03.h> /* kernal_901227_03 */

/* Create external accesible variable for C64 RAM initialized with zero values */
uint8_t *RAM;
#if EMBEDDED
#include <pico.h>
extern uint8_t __not_in_flash("c64_memory") c64memory[];
#endif


/**
 * @brief Construct a new mmu::mmu object
 *
 */
mmu::mmu(void)
{
  MOSDBG("[MMU] Init\n");

#if DESKTOP
  RAM = new uint8_t[0x10000];
#elif EMBEDDED
  RAM = c64memory;
#endif

  /* Connect RAM pointer to RAM variable */
  memset(RAM,0,0x10000); /* Clear up that bitch */

  /* Set Data Direction bits defaults as per https://www.pagetable.com/c64ref/c64mem/ */
  RAM[pAddrDataDirection] = 0xef;
  RAM[pAddrMemoryLayout]  = 0x37;

  /* Init ROMS */
  basic = basic_901226_01;
  chargen = characters_901225_01;
  kernal = kernal_901227_03;

  return;
}

/**
 * @brief Destroy the mmu::mmu object
 *
 */
mmu::~mmu(void)
{
  MOSDBG("[MMU] Deinit\n");
  memset(RAM,0,0x10000); /* Clear that bitch up */
  return;
}

/**
 * @brief Glue required C64 parts
 *
 */
void __us_not_in_flash_func(glue_c64) mmu::glue_c64(mos6510 *_cpu, mos906114 *_pla, mos6560_6561 *_vic, mos6526 *_cia1, mos6526 *_cia2, mos6581_8580 *_sid)
{
 cpu = _cpu;
 pla = _pla;
 vic = _vic;
 cia1 = _cia1;
 cia2 = _cia2;
 sid = _sid;

 return;
}

/**
 * @brief IO Read from SID
 *
 * @param addr
 * @return uint8_t
 */
uint8_t __us_not_in_flash_func(read_sid) mmu::read_sid(uint16_t addr)
{
  return sid->read_sid(addr);
}

/**
 * @brief IO Write to SID
 *
 * @param addr
 * @param data
 */
void __us_not_in_flash_func(write_sid) mmu::write_sid(uint16_t addr, uint8_t data)
{
  sid->write_sid(addr,data);
  return;
}

/**
 * @brief IO Read from Cia1/Cia2
 *
 * @param addr
 * @return uint8_t
 */
uint8_t __us_not_in_flash_func(read_cia) mmu::read_cia(uint_least16_t addr)
{
  uint8_t data = RAM[addr]; /* Always read from RAM as fallback */
  uint_least16_t cia_page = (addr & 0xFF00);
  uint8_t cia_addr = (addr & 0xF);
  if (cia_page == pAddrCIA1Page) {
    data = cia1->read_register(cia_addr);
    if (log_cia1rw) MOSDBG("[R CIA1] $%04x $%02x:%02x\n",addr,cia_addr,data);
  } else if (cia_page == pAddrCIA2Page) {
    data = cia2->read_register(cia_addr);
    if (log_cia2rw) MOSDBG("[R CIA2] $%04x $%02x:%02x\n",addr,cia_addr,data);
  }

  return data;
}

/**
 * @brief IO Write to Cia1/Cia2
 *
 * @param addr
 * @param data
 */
void __us_not_in_flash_func(write_cia) mmu::write_cia(uint_least16_t addr, uint8_t data)
{
  uint_least16_t cia_page = (addr & 0xFF00);
  uint8_t cia_addr = (addr & 0xF);
  if (cia_page == pAddrCIA1Page) {
    if (log_cia1rw) MOSDBG("[W CIA1] $%04x:%02x\n",addr,data);
    cia1->write_register(cia_addr,data);
  } else if (cia_page == pAddrCIA2Page) {
    if (log_cia2rw) MOSDBG("[W CIA2] $%04x:%02x\n",addr,data);
    cia2->write_register(cia_addr,data);
  }

  return;
}

/**
 * @brief IO Read from VIC
 *
 * @param addr
 * @return uint8_t
 */
uint8_t __us_not_in_flash_func(read_vic) mmu::read_vic(uint_least16_t addr)
{
  uint8_t data = 0xff;
  uint8_t vic_addr = (addr & 0x3f);
  if (log_vicrw) MOSDBG("[R  VIC] $%04x:%02x\n",addr,data);
  if _MOS_LIKELY (vic_addr <=0x3f) {
    data = vic->read_register(vic_addr);
  }

  return data;
}

/**
 * @brief IO Write to VIC
 *
 * @param addr
 * @param data
 */
void __us_not_in_flash_func(write_vic) mmu::write_vic(uint_least16_t addr, uint8_t data)
{
  uint8_t vic_addr = (addr & 0x3F);
  if _MOS_LIKELY (vic_addr <=0x3f) {
    vic->write_register(vic_addr,data);
  }
  if (log_vicrw) MOSDBG("[W  VIC] $%04x:%02x\n",addr,data);

  return;
}

/**
 * @brief Reads a byte of data from ROM based on the provided
 * ROM type
 *
 * @param addr
 * @param rom
 * @return uint8_t
 */
uint8_t __us_not_in_flash_func(rom_read_byte) mmu::rom_read_byte(uint16_t addr, char rom)
{
  uint8_t data;
  switch (rom) {
    case 'B':
      data = basic[addr];
      break;
    case 'C':
      data = chargen[addr];
      break;
    case 'K':
      data = kernal[addr];
      break;
    default:
      break;
  }
  if (log_romrw) {
    MOSDBG("[R  ROM]$%04x:%02x [B%dC%dK%d]\n",
      addr,data,bsc,crg,krn);
  }
  return data;
}

/**
 * @brief Read byte from RAM from the VIC's perspective
 *
 * The VIC has only 14 address lines so it can only access
 * 16kB of memory at once, the two missing address bits are
 * provided by CIA2.
 *
 * The VIC always reads from RAM ignoring the memory configuration,
 * there's one exception: the character generator ROM. Unless the
 * Ultimax mode is selected, VIC sees the character generator ROM
 * in the memory areas:
 *
 *  $1000/$1fff
 *  $9000/$9fff
 */
uint8_t __us_not_in_flash_func(vic_read_byte) mmu::vic_read_byte(uint16_t addr) /* Unused */
{
  uint8_t v;
  uint16_t vic_addr = cia2->vic_base_address() + (addr & 0x3fff);
  if((vic_addr >= 0x1000 && vic_addr <  0x2000) ||
     (vic_addr >= 0x9000 && vic_addr <  0xa000)) {
    v = chargen[(vic_addr & 0xfff)];
  } else {
    v = RAM[vic_addr];
  }
  if (log_vicrrw) {
    MOSDBG("[VIC RR] $%04x:%02x (%04x/%04x/%04x)\n",
      addr,v,vic_addr,
      (pAddrCharsFirstPage + (vic_addr & 0xfff)),
      cia2->vic_base_address());
  }
  return v;
}

/**
 * @brief Read a byte of data from address with regards to
 * the configured memory layout, will read from RAM if no
 * IO is needed
 *
 * @param addr
 * @return uint8_t
 */
uint8_t __us_not_in_flash_func(read_byte) mmu::read_byte(uint16_t addr)
{
  bsc = pla->memory_banks(mos906114::kBankBasic);
  crg = pla->memory_banks(mos906114::kBankChargen);
  krn = pla->memory_banks(mos906114::kBankKernal);

  bool read_io = (crg == mos906114::kIO);
  bool b_rom = (bsc == mos906114::kROM);
  bool c_rom = (crg == mos906114::kROM);
  bool k_rom = (krn == mos906114::kROM);

  /* Always read from RAM first as default, gets overriden by actual read */
  uint8_t data = RAM[addr];
  switch (addr) {
    /* $0000 */
    case pAddrDataDirection:
      /* TODO: Add routine? */
      break;
    /* $0001 */
    case pAddrMemoryLayout:
      /* TODO: Add routine? */
      break;
    /* $a000/$bfff ~ Basic ROM or RAM */
    case pAddrBasicFirstPage ... (pAddrBasicLastPage + pC64PageEnd):
      if (b_rom) {
        data = rom_read_byte((addr&0x1fff), 'B');
      }
      break;
    /* $d000/$d3ff ~ Character ROM or VIC-II DMA */
    case pAddrVicFirstPage ... (pAddrVicLastPage + pC64PageEnd):
      if _MOS_LIKELY (read_io) {
        data = read_vic(addr);
      } else if _MOS_UNLIKELY (c_rom) {
        data = rom_read_byte((addr&0x0fff), 'C');
      }
      break;
    /* $d400/$d7ff ~ SID audio */
    case pAddrSIDFirstPage ... (pAddrSIDLastPage + pC64PageEnd):
      if _MOS_LIKELY (read_io) {
        data = read_sid(addr);
      } else if _MOS_UNLIKELY (c_rom) {
        data = rom_read_byte((addr&0x0fff), 'C');
      }
      break;
    /* $d800/$dbff ~ Color RAM */
    case pAddrColorRAMFirstPage ... (pAddrColorRAMLastPage + pC64PageEnd):
      if (c_rom) {
        data = rom_read_byte((addr&0x0fff), 'C');
      }
      break;
    /* $dc00/$dcff ~ Cia 1 */
    case pAddrCIA1Page ... (pAddrCIA1Page + pC64PageEnd):
      if _MOS_LIKELY (read_io) {
        data = read_cia(addr);
      } else if _MOS_UNLIKELY (c_rom) {
        data = rom_read_byte((addr&0x0fff), 'C');
      }
      break;
    /* $dd00/$ddff ~ Cia 2 */
    case pAddrCIA2Page ... (pAddrCIA2Page + pC64PageEnd):
      if _MOS_LIKELY (read_io) {
        data = read_cia(addr);
      } else if _MOS_UNLIKELY (c_rom) {
        data = rom_read_byte((addr&0x0fff), 'C');
      }
      break;
    /* $de00/$dfff ~ IO or RAM */
    case pAddrIO1Page ... (pAddrIO2Page + pC64PageEnd):
      if _MOS_LIKELY (read_io) {
        data = read_sid(addr);
      } else if _MOS_UNLIKELY (c_rom) {
        data = rom_read_byte((addr&0x0fff ), 'C');
      }
      break;
    /* $e000/$ffff ~ Kernal ROM or RAM*/
    case pAddrKernalFirstPage ... (pAddrKernalLastPage + pC64PageEnd):
      if (k_rom) {
        data = rom_read_byte((addr&0x1fff), 'K');
      }
      break;
    default:
      break;
  }
  if (log_readwrites)
    MOSDBG("[R MEM %d%d%d%d]$%04x:%02x\n",
      read_io,b_rom,c_rom,k_rom,addr,data);
  return data;
}

/**
 * @brief Write a byte of data to address with regards to
 * the configured memory layout, will write to RAM if no
 * IO is needed
 *
 * @param addr
 * @param data
 */
void __us_not_in_flash_func(write_byte) mmu::write_byte(uint16_t addr, uint8_t data)
{
  crg = pla->memory_banks(mos906114::kBankChargen);
  bool write_io = (crg == mos906114::kIO);

  if (log_readwrites) MOSDBG("[W MEM %d___]$%04x:%02x\n",write_io,addr,data);
  switch (addr) {
    /* $0000 */
    case pAddrDataDirection:
      /* TODO: Add something */
      return;
    /* $0001 */
    case pAddrMemoryLayout:
      pla->runtime_bank_switching(data);
      return;
    /* $d000/$d3ff ~ VIC-II DMA or Character ROM */
    case pAddrVicFirstPage ... (pAddrVicLastPage + pC64PageEnd):
      if _MOS_LIKELY (write_io) {
        write_vic(addr, data);
        return;
      }
      break;
    /* $d400/$d7ff ~ SID audio */
    case pAddrSIDFirstPage ... (pAddrSIDLastPage + pC64PageEnd):
      if _MOS_LIKELY (write_io) {
        write_sid(addr, data);
        return;
      }
      break;
    /* $dc00/$dcff ~ Cia 1 */
    case pAddrCIA1Page ... (pAddrCIA1Page + pC64PageEnd):
      if _MOS_LIKELY (write_io) {
        write_cia(addr, data);
        return;
      }
      break;
    /* $dd00/$ddff ~ Cia 2 */
    case pAddrCIA2Page ... (pAddrCIA2Page + pC64PageEnd):
      if _MOS_LIKELY (write_io) {
        write_cia(addr, data);
        return;
      }
      break;
    /* $de00/$dfff ~ IO or RAM */
    case pAddrIO1Page ... (pAddrIO2Page + pC64PageEnd):
      if _MOS_LIKELY (write_io) {
        write_sid(addr, data);
        return;
      }
      break;
    default:
      break;
  }
  /* Always write to RAM in all other cases */
  RAM[addr] = data;
}

uint8_t __us_not_in_flash_func(dma_read_ram) mmu::dma_read_ram(uint16_t addr)
{
  /* MOSDBG("[DMA  READ] $%04x:%02x\n", addr, RAM[addr]); */
  return RAM[addr];
}

void __us_not_in_flash_func(dma_write_ram) mmu::dma_write_ram(uint16_t addr, uint8_t data)
{
  RAM[addr] = data;
  /* MOSDBG("[DMA WRITE] $%04x:%02x(%02x)\n", addr, data, RAM[addr]); */
  return;
}
