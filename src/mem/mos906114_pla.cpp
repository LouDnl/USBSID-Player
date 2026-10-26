/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * mos906114_pla.cpp
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
 *
 */

#include "mos906114_pla.h"

namespace usbsid {

/* Which bank switching zone each 4 KB page belongs to */
const uint8_t Mos906114Pla::kZoneOfPage[16] = {
  static_cast<uint8_t>(Zone::Ram0),   /* $0000 */
  static_cast<uint8_t>(Zone::Ram1),   /* $1000 */
  static_cast<uint8_t>(Zone::Ram1),   /* $2000 */
  static_cast<uint8_t>(Zone::Ram1),   /* $3000 */
  static_cast<uint8_t>(Zone::Ram1),   /* $4000 */
  static_cast<uint8_t>(Zone::Ram1),   /* $5000 */
  static_cast<uint8_t>(Zone::Ram1),   /* $6000 */
  static_cast<uint8_t>(Zone::Ram1),   /* $7000 */
  static_cast<uint8_t>(Zone::Cart),   /* $8000 */
  static_cast<uint8_t>(Zone::Cart),   /* $9000 */
  static_cast<uint8_t>(Zone::Basic),  /* $a000 */
  static_cast<uint8_t>(Zone::Basic),  /* $b000 */
  static_cast<uint8_t>(Zone::Ram2),   /* $c000 */
  static_cast<uint8_t>(Zone::IoChar), /* $d000 */
  static_cast<uint8_t>(Zone::Kernal), /* $e000 */
  static_cast<uint8_t>(Zone::Kernal), /* $f000 */
};

Mos906114Pla::Mos906114Pla(void)
{
  reset();
}

void Mos906114Pla::reset(void)
{
  /* After reset all three latches read 1 (mode 31, BASIC IO and KERNAL visible) */
  port_ = kPortLoram | kPortHiram | kPortCharen;
  game_ = true;
  exrom_ = true;
  update(port_, game_, exrom_);
}

void Mos906114Pla::set_cartridge_lines(bool game, bool exrom)
{
  update(port_, game, exrom);
}

/**
 * @brief Recompute the bank map
 *
 * Follows the 32 mode table https://www.c64-wiki.com/wiki/Bank_Switching#Mode_Table
 */
void Mos906114Pla::update(data_t port, bool game, bool exrom)
{
  port_ = static_cast<data_t>(port & (kPortLoram | kPortHiram | kPortCharen));
  game_ = game;
  exrom_ = exrom;

  const bool loram  = (port_ & kPortLoram) != 0;
  const bool hiram  = (port_ & kPortHiram) != 0;
  const bool charen = (port_ & kPortCharen) != 0;

  mode_ = static_cast<uint8_t>(port_ |
                               (game_  ? 0x08 : 0) |
                               (exrom_ ? 0x10 : 0));

  Bank * b = banks_;

  if (ultimax()) {
    /* Ultimax: cartridge owns the reset vector, only the first 4 KB of RAM and IO stay mapped */
    b[static_cast<uint8_t>(Zone::Ram0)]   = Bank::Ram;
    b[static_cast<uint8_t>(Zone::Ram1)]   = Bank::Open;
    b[static_cast<uint8_t>(Zone::Cart)]   = Bank::CartLo;
    b[static_cast<uint8_t>(Zone::Basic)]  = Bank::Open;
    b[static_cast<uint8_t>(Zone::Ram2)]   = Bank::Open;
    b[static_cast<uint8_t>(Zone::IoChar)] = Bank::Io;
    b[static_cast<uint8_t>(Zone::Kernal)] = Bank::CartHi;
    return;
  }

  b[static_cast<uint8_t>(Zone::Ram0)] = Bank::Ram;
  b[static_cast<uint8_t>(Zone::Ram1)] = Bank::Ram;
  b[static_cast<uint8_t>(Zone::Ram2)] = Bank::Ram;

  /* $8000: cartridge low ROM needs both latches and an asserted EXROM */
  b[static_cast<uint8_t>(Zone::Cart)] =
    (loram && hiram && !exrom_) ? Bank::CartLo : Bank::Ram;

  /* $a000: 16 KB cartridge overrides BASIC */
  if (hiram && !game_ && !exrom_) {
    b[static_cast<uint8_t>(Zone::Basic)] = Bank::CartHi;
  } else if (loram && hiram) {
    b[static_cast<uint8_t>(Zone::Basic)] = Bank::Basic;
  } else {
    b[static_cast<uint8_t>(Zone::Basic)] = Bank::Ram;
  }

  /* $d000: CHAREN only applies while at least one latch is set */
  if (!loram && !hiram) {
    b[static_cast<uint8_t>(Zone::IoChar)] = Bank::Ram;
  } else {
    b[static_cast<uint8_t>(Zone::IoChar)] = charen ? Bank::Io : Bank::CharRom;
  }

  /* $e000 */
  b[static_cast<uint8_t>(Zone::Kernal)] = hiram ? Bank::Kernal : Bank::Ram;
}

} /* namespace usbsid */
