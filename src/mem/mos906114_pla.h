/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos906114_pla.h
 * The 906114 PLA: it turns the three processor port latches and the two
 * cartridge lines into the bank map the MMU decodes against.
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
 *
 */

#pragma once
#ifndef _US_MEM_MOS906114_PLA_H_
#define _US_MEM_MOS906114_PLA_H_

#include "types.h"

namespace usbsid {

/**
 * @brief What a bank switching zone currently contains.
 */
enum class Bank : uint8_t {
  Ram,     /* plain RAM */
  Basic,   /* BASIC ROM at $a000 */
  Kernal,  /* KERNAL ROM at $e000 */
  CharRom, /* character generator at $d000 */
  Io,      /* VIC, SID, colour RAM, CIAs, expansion IO */
  CartLo,  /* cartridge ROML at $8000 */
  CartHi,  /* cartridge ROMH at $a000 or $e000 */
  Open,    /* unmapped, reads float */
};

/**
 * @brief The seven bank switching zones of a C64.
 *
 * https://www.c64-wiki.com/wiki/Bank_Switching#Bank_Switching_Zones
 */
enum class Zone : uint8_t {
  Ram0    = 0, /* $0000-$0fff, always RAM */
  Ram1    = 1, /* $1000-$7fff */
  Cart    = 2, /* $8000-$9fff */
  Basic   = 3, /* $a000-$bfff */
  Ram2    = 4, /* $c000-$cfff */
  IoChar  = 5, /* $d000-$dfff */
  Kernal  = 6, /* $e000-$ffff */
  Count   = 7,
};

/* Processor port bits that reach the PLA */
enum : data_t {
  kPortLoram  = 0x01,
  kPortHiram  = 0x02,
  kPortCharen = 0x04,
};

class Mos906114Pla
{
  public:
    Mos906114Pla(void);
    ~Mos906114Pla(void) = default;

    void reset(void);

    /**
     * @brief Recompute the bank map.
     *
     * @param port    the three low bits of the processor port at $01
     * @param game    expansion port GAME line, 1 = not asserted (no cartridge)
     * @param exrom   expansion port EXROM line, 1 = not asserted
     */
    void update(data_t port, bool game, bool exrom);

    /* Cartridge lines, kept so a later cartridge implementation has a place
     * to drive them from. With no cartridge both are high. */
    void set_cartridge_lines(bool game, bool exrom);

    US_ALWAYS_INLINE Bank bank(Zone zone) const
    {
      return banks_[static_cast<uint8_t>(zone)];
    }
    /* The bank for a 4 KB page index ($0000-$ffff -> 0..15) */
    US_ALWAYS_INLINE Bank bank_for_page(uint8_t page) const
    {
      return banks_[kZoneOfPage[page]];
    }

    US_ALWAYS_INLINE data_t port(void) const { return port_; }
    US_ALWAYS_INLINE bool game(void) const { return game_; }
    US_ALWAYS_INLINE bool exrom(void) const { return exrom_; }
    /* Ultimax is EXROM high and GAME low, the one configuration that unmaps
     * most of the address space */
    US_ALWAYS_INLINE bool ultimax(void) const { return exrom_ && !game_; }

    /* Mode number as used by the documentation tables, 0..31:
     * bit 0 LORAM, 1 HIRAM, 2 CHAREN, 3 GAME, 4 EXROM */
    US_ALWAYS_INLINE uint8_t mode(void) const { return mode_; }

  private:
    static const uint8_t kZoneOfPage[16];

    Bank banks_[static_cast<uint8_t>(Zone::Count)];
    data_t port_ = kPortLoram | kPortHiram | kPortCharen;
    bool game_ = true;
    bool exrom_ = true;
    uint8_t mode_ = 31;
};

} /* namespace usbsid */

#endif /* _US_MEM_MOS906114_PLA_H_ */
