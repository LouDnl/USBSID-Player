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
 * mos906114_pla.h
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025 LouD
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

#ifndef _MOS906114_PLA_H
#define _MOS906114_PLA_H

#include <cstdint>

class mmu;
class mos6510;

class mos906114
{
  private:
    /* Glue */
    mmu * mmu_;
    mos6510 * cpu;

    /* Memory banks */
    uint_least8_t data_direction_default = 0x2F; /* (https://www.c64-wiki.com/wiki/Zeropage) */
    uint_least8_t banks_at_boot = 0x1F;
    uint_least8_t banks_at_runtime = 0x1F;
    uint_least8_t banks_[7];

    /* default bank setup is m31 0x1F */
    uint_least8_t default_bankmode; /* Set at start */

    uint_least8_t generate_bank_setup(void);

  public:
    mos906114(mmu * _mmu);
    ~mos906114(void);

    /** Bank Switching Zones
     *
     * @brief as described at https://www.c64-wiki.com/wiki/Bank_Switching#Bank_Switching_Zones
     * ID Hex Address Dec Address Page         Size Contents
     *  0 $0000/$0fff 0-4095      Page 0-15       4 kBytes RAM (which the system requires and must appear in each mode)
     *  1 $1000/$7fff 4096-32767  Page 16-127    28 kBytes RAM or is unmapped
     *  2 $8000/$9fff 32768-40959 Page 128-159    8 kBytes RAM or cartridge ROM
     *  3 $a000/$bfff 40960-49151 Page 160-191    8 kBytes RAM, BASIC interpretor ROM, cartridge ROM or is unmapped
     *  4 $c000/$cfff 49152-53247 Page 192-207    4 kBytes RAM or is unmapped
     *  5 $d000/$dfff 53248-57343 Page 208-223    4 kBytes RAM, Character generator ROM, or I/O registers and Color RAM
     *  6 $e000/$ffff 57344-65535 Page 224-255    8 kBytes RAM, KERNAL ROM or cartridge ROM
     */
    enum Banks
    {
      kBankRam0    = 0, /* RAM, cannot be changed */
      kBankRam1    = 1, /* RAM or unmapped */
      kBankCart    = 2, /* RAM or cartridge ROM */
      kBankBasic   = 3, /* RAM, Basic ROM, cartridge ROM or unmapped */
      kBankRam2    = 4, /* RAM or unmapped */
      kBankChargen = 5, /* RAM, Character ROM, I/O registers and Color RAM */
      kBankKernal  = 6, /* RAM, Kernal ROM or cartridge ROM */
    };
    const char * BankModeNames[5] = {
      "kROM","kRAM","kIO","kCLO","kCHI"
    };

    /* Bank Switching Bit ID's */
    enum kBankCfg
    { /* CPU Control Lines */
      kROM, /* bit 0, weight  1 */
      kRAM, /* bit 1, weight  2 */
      kIO,  /* bit 2, weight  4 */
      /* Expansion Port Pins 8 and 9 */
      kCLO, /* bit 3, weight  8 */
      kCHI, /* bit 4, weight 16 */
      kUNM = -1, /* Unmapped */
    };

    /* Bank Switching Bits */
    static const uint_least8_t kLORAM   = 1 << kROM; /* 0 */
    static const uint_least8_t kHIRAM   = 1 << kRAM; /* 1 */
    static const uint_least8_t kCHARGEN = 1 << kIO;  /* 2 */
    static const uint_least8_t kGAME    = 1 << kCLO; /* 3 */
    static const uint_least8_t kEXROM   = 1 << kCHI; /* 4 */

    /** Bank Switching Modes
     * @brief as decribed at https://www.c64-wiki.com/wiki/Bank_Switching#Mode_Table
     */
    enum Modes
    { /* Visible = 1, Not visible = 0 */
      m31 = (kEXROM|kGAME|kCHARGEN|kHIRAM|kLORAM),
      m30 = (kEXROM|kGAME|kCHARGEN|kHIRAM),
      m29 = (kEXROM|kGAME|kCHARGEN|       kLORAM),
      m28 = (kEXROM|kGAME|kCHARGEN),
      m27 = (kEXROM|kGAME|         kHIRAM|kLORAM),
      m26 = (kEXROM|kGAME|         kHIRAM),
      m25 = (kEXROM|kGAME|                kLORAM),
      m24 = (kEXROM|kGAME),
      m23 = (kEXROM|      kCHARGEN|kHIRAM|kLORAM),
      m22 = (kEXROM|      kCHARGEN|kHIRAM),
      m21 = (kEXROM|      kCHARGEN|       kLORAM),
      m20 = (kEXROM|      kCHARGEN),
      m19 = (kEXROM|               kHIRAM|kLORAM),
      m18 = (kEXROM|               kHIRAM),
      m17 = (kEXROM|                      kLORAM),
      m16 = (kEXROM),
      m15 = (       kGAME|kCHARGEN|kHIRAM|kLORAM),
      m14 = (       kGAME|kCHARGEN|kHIRAM),
      m13 = (       kGAME|kCHARGEN|       kLORAM),
      m12 = (       kGAME|kCHARGEN),
      m11 = (       kGAME|         kHIRAM|kLORAM),
      m10 = (       kGAME|         kHIRAM),
      m09 = (       kGAME|                kLORAM),
      m08 = (       kGAME),
      m07 = (             kCHARGEN|kHIRAM|kLORAM),
      m06 = (             kCHARGEN|kHIRAM),
      m05 = (             kCHARGEN|       kLORAM),
      m04 = (             kCHARGEN),
      m03 = (                      kHIRAM|kLORAM),
      m02 = (                      kHIRAM),
      m01 = (                             kLORAM),
      m00 = 0,
    };

    void glue_c64(mos6510 * _cpu);
    void reset(void);

    void switch_banks(uint8_t v);

    uint_least8_t memory_banks(int v){return banks_[v];};
    void setup_memory_banks(uint8_t v);
    void runtime_bank_switching(uint8_t v);

};


#endif /* _MOS906114_PLA_H */
