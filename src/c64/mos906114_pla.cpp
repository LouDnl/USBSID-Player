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
 * pla.cpp
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

#include <mos6510_cpu.h>
#include <mos906114_pla.h>
#include <c64util.h>


extern bool log_pla;

/**
 * @brief Construct a new mos906114::mos906114 object
 *
 */
mos906114::mos906114()
{
  MOSDBG("[PLA] Init\n");

  default_bankmode = (kLORAM|kHIRAM|kCHARGEN|kGAME|kEXROM);
  setup_memory_banks(default_bankmode);
  /* configure data directional bits to default boot setting */
  RAM[0x0001] = data_direction_default;
}

/**
 * @brief Glue required C64 parts
 *
 */
void mos906114::glue_c64(mos6510 * _cpu)
{
  cpu = _cpu;

  return;
}

/**
 * @brief
 *
 */
void mos906114::reset(void)
{
  setup_memory_banks(default_bankmode);
  /* configure data directional bits to default boot setting */
  RAM[0x0001] = data_direction_default;
}

/**
 * @brief Bank switching logic
 *
 * @param v
 *
 * Example:
 * PLA latch bits: 11111 ~ 54321
 * kLORAM   ~ 1
 * kHIRAM   ~ 2
 * kCHARGEN ~ 3
 * kGAME    ~ 4
 * kEXROM   ~ 5
 *
 * banks_[] variable gets set up with the values
 * from kBankCfg referenced by they bit location
 *
 * In mode 31 (default) all bits are high (1)
 * and setting this mode equals:
 * kEXROM,kGAME,kCHARGEN,kHIRAM,kLORAM
 *      1,    1,       1,     1,     1
 *
 * In mode 20 bits are 10100 this corresponds to:
 * kEXROM,      kCHARGEN
 *      1,    0,       1,     0,     0
 *
 * So banks_[]
 * [kBankRam0,kBankRam1,kBankCart,kBankBasic,kBankRam2,kBankChargen,kBankKernal]
 * will look like this for mode 32:
 * [1,1,1,0,1,2,0]
 * and for mode 20:
 * [1,-1,3,-1,-1,2,4]
 *
 */
void mos906114::switch_banks(uint8_t v)
{
  /* Setup bank mode on boot */
  switch ((v&0x1F)) { /* Masked to check only available bits */
    case m31:
      banks_[kBankRam0]    = kRAM; /* RAM     ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM     ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM     ~ 0x8000 */
      banks_[kBankBasic]   = kROM; /* BASIC   ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM     ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO      ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL  ~ 0xe000 */
      break;
    case m30: /* fallthrough ~ same settings */
    case m14:
      banks_[kBankRam0]    = kRAM; /* RAM     ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM     ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM     ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM     ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM     ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO      ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL  ~ 0xe000 */
      break;
    case m29: /* fallthrough ~ same settings */
    case m13:
      banks_[kBankRam0]    = kRAM; /* RAM     ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM     ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM     ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM     ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM     ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO      ~ 0xd000 */
      banks_[kBankKernal]  = kRAM; /* RAM     ~ 0xe000 */
      break;
    case m28: /* fallthrough ~ same settings */
    case m24:
      banks_[kBankRam0]    = kRAM; /* RAM     ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM     ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM     ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM     ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM     ~ 0xc000 */
      banks_[kBankChargen] = kRAM; /* RAM     ~ 0xd000 */
      banks_[kBankKernal]  = kRAM; /* RAM     ~ 0xe000 */
      break;
    case m27:
      banks_[kBankRam0]    = kRAM; /* RAM     ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM     ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM     ~ 0x8000 */
      banks_[kBankBasic]   = kROM; /* BASIC   ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM     ~ 0xc000 */
      banks_[kBankChargen] = kROM; /* CHARGEN ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL  ~ 0xe000 */
      break;
    case m26: /* fallthrough ~ same settings */
    case m10:
      banks_[kBankRam0]    = kRAM; /* RAM     ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM     ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM     ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM     ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM     ~ 0xc000 */
      banks_[kBankChargen] = kROM; /* CHARGEN ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL  ~ 0xe000 */
      break;
    case m25: /* fallthrough ~ same settings */
    case m09:
      banks_[kBankRam0]    = kRAM; /* RAM     ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM     ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM     ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM     ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM     ~ 0xc000 */
      banks_[kBankChargen] = kROM; /* CHARGEN ~ 0xd000 */
      banks_[kBankKernal]  = kRAM; /* KERNAL  ~ 0xe000 */
      break;
    case m23: /* fallthrough ~ same settings */
    case m22:
    case m21:
    case m20:
    case m19:
    case m18:
    case m17:
    case m16:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kUNM; /* UNMAPPED ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kCLO; /* CART LO  ~ 0x8000 */
      banks_[kBankBasic]   = kUNM; /* UNMAPPED ~ 0xa000 */
      banks_[kBankRam2]    = kUNM; /* UNMAPPED ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO       ~ 0xd000 */
      banks_[kBankKernal]  = kCHI; /* CART HI  ~ 0xe000 */
      break;
    case m15:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kCLO; /* CART LO  ~ 0x8000 */
      banks_[kBankBasic]   = kROM; /* BASIC    ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO       ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL   ~ 0xe000 */
      break;
    case m12: /* fallthrough ~ same settings */
    case m08:
    case m04:
    case m00:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM      ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM      ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kRAM; /* RAM      ~ 0xd000 */
      banks_[kBankKernal]  = kRAM; /* RAM      ~ 0xe000 */
      break;
    case m11:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kCLO; /* CART LO  ~ 0x8000 */
      banks_[kBankBasic]   = kROM; /* BASIC    ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kROM; /* CHARGEN  ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL   ~ 0xe000 */
      break;
    case m07:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kCLO; /* CART LO  ~ 0x8000 */
      banks_[kBankBasic]   = kCHI; /* CART HI  ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO       ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL   ~ 0xe000 */
      break;
    case m06:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM      ~ 0x8000 */
      banks_[kBankBasic]   = kCHI; /* CART HI  ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO       ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL   ~ 0xe000 */
      break;
    case m05:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM      ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM      ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kIO;  /* IO       ~ 0xd000 */
      banks_[kBankKernal]  = kRAM; /* RAM      ~ 0xe000 */
      break;
    case m03:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kCLO; /* CART LO  ~ 0x8000 */
      banks_[kBankBasic]   = kCHI; /* CART HI  ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kROM; /* CHARGEN  ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL   ~ 0xe000 */
      break;
    case m02:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM      ~ 0x8000 */
      banks_[kBankBasic]   = kCHI; /* CART HI  ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kROM; /* CHARGEN  ~ 0xd000 */
      banks_[kBankKernal]  = kROM; /* KERNAL   ~ 0xe000 */
      break;
    case m01:
      banks_[kBankRam0]    = kRAM; /* RAM      ~ 0x0000 - Unchangeable */
      banks_[kBankRam1]    = kRAM; /* RAM      ~ 0x1000 - Gets unmapped if in Ultimax Mode */
      banks_[kBankCart]    = kRAM; /* RAM      ~ 0x8000 */
      banks_[kBankBasic]   = kRAM; /* RAM      ~ 0xa000 */
      banks_[kBankRam2]    = kRAM; /* RAM      ~ 0xc000 */
      banks_[kBankChargen] = kRAM; /* RAM      ~ 0xd000 */
      banks_[kBankKernal]  = kRAM; /* RAM      ~ 0xe000 */
      break;
    default:
      MOSDBG("[PLA] Unmapped bank switch mode from %02X to %02X %02X requested\n",
        RAM[0x0001],
        v, (v&0x1f));
      break;
  }
}

/**
 * @brief configure memory banks on boot and PLA reset
 *
 * @param v
 *
 * There are five latch bits that control the configuration allowing
 * for a total of 32 different memory layouts, for now we only take
 * in count three bits : HIRAM/LORAM/CHAREN
 *
 */
void mos906114::setup_memory_banks(uint8_t v)
{
  /* start with mode 0 and set everything to ram */
  for(size_t i=0 ; i < sizeof(banks_) ; i++) {
    banks_[i] = kRAM;
  }
  /* Switch banks on boot */
  banks_at_runtime = banks_at_boot = v;
  switch_banks(banks_at_boot);
  /* write the raw value to the zero page (doesn't influence the cart bits) */
  RAM[0x0001] = v;
}

/**
 * @brief generates a banksetup byte from the banks_[] variable
 *
 */
uint_least8_t mos906114::generate_bank_setup(void)
{
  return
  banks_[kBankRam0]+
  banks_[kBankRam1]+
  banks_[kBankCart]+
  banks_[kBankBasic]+
  banks_[kBankRam2]+
  banks_[kBankChargen]+
  banks_[kBankKernal];
}

/**
 * @brief configure memory banks during runtime, limited to 3 cpu latches
 *
 * During runtime the system can only change the cpu latches at bit 0,1 and 2
 * @param v
 *
 */
void mos906114::runtime_bank_switching(uint8_t v)
{
  /* Setup bank mode during runtime */
  uint8_t b = banks_at_boot; /* Use boot time state as preset */
  b &= (0x18|(v&0x7)); /* Preserve _cart bits_ and only set cpu latches */
  if (log_pla) printf("[PLA] Bank switch @ runtime from %02X to: %02X with %02X requested\n",banks_at_boot,b,v);
  switch_banks(b);
  // if (c64_->havecart && ((v&0x18) != (banks_at_boot&0x18))) { /* Cart ROM switch requested */
  /* write the raw value to the zero page (doesn't influence the cart bits) */
  RAM[0x0001] = v;
  banks_at_runtime = v;
}
