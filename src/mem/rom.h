/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * rom.h
 * Where the machine gets its ROM images from. Defaults to the compiled in
 * stock images, or with US_EMBED_ROMS=0 to the stub KERNAL alone; the
 * pointers let a caller substitute its own set without the MMU knowing.
 *
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
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

#pragma once
#ifndef _US_MEM_ROM_H_
#define _US_MEM_ROM_H_

#include "constants.h"
#include "types.h"

/* 1: compile the stock Commodore ROM images in (CLI, web, firmware).
 * 0: ship no Commodore code; the machine starts on the stub KERNAL with no
 *    BASIC and no character generator until real images are supplied. */
#ifndef US_EMBED_ROMS
#define US_EMBED_ROMS 1
#endif

#if US_EMBED_ROMS
#include "roms/rom_data.h"
#endif

namespace usbsid {

/* Idle loop of the stub KERNAL (`jmp $fcf5`), where an RTS from a program
 * started without ROMs ends up */
constexpr addr_t kStubIdleLoop = 0xfcf5;

#if !EMBEDDED || !US_EMBED_ROMS
/**
 * @brief The stub KERNAL, kRomSizeKernal bytes for $e000-$ffff.
 *
 * Reset, cartridge check, IRQ/NMI entry and exit with the stock instruction
 * sequences (same cycle counts), RTS on the jump table, NOP elsewhere.
 *
 * @return pointer to the image, built on first call
 */
const data_t * stub_kernal(void);
#endif

/**
 * @brief ROM images the MMU maps in.
 *
 * `basic` and `chargen` may be null: the area then reads the RAM underneath.
 * `kernal` is never null; `real_kernal` says whether it is a stock image or
 * the stub.
 */
struct Roms {
#if US_EMBED_ROMS
  const data_t * basic   = kRomBasic;
  const data_t * kernal  = kRomKernal;
  const data_t * chargen = kRomChargen;
  bool real_kernal       = true;
#else
  const data_t * basic   = nullptr;
  const data_t * kernal  = stub_kernal();
  const data_t * chargen = nullptr;
  bool real_kernal       = false;
#endif

  /** @brief True when BASIC and a real KERNAL are both present. */
  bool complete(void) const { return real_kernal && basic != nullptr; }
};

#if !EMBEDDED || !US_EMBED_ROMS
/**
 * @brief Owned copies of ROM images supplied at runtime.
 *
 * Holds the bytes so the caller's buffers can go away; roms() is the view
 * the MMU takes. A missing KERNAL means the stub.
 */
class RomStore
{
  public:
    /**
     * @brief Copy the given images, nullptr for an absent one.
     *
     * @param basic    kRomSizeBasic bytes or nullptr
     * @param kernal   kRomSizeKernal bytes or nullptr (stub)
     * @param chargen  kRomSizeChargen bytes or nullptr
     */
    void set(const data_t * basic, const data_t * kernal, const data_t * chargen)
    {
      has_basic_ = (basic != nullptr);
      has_kernal_ = (kernal != nullptr);
      has_chargen_ = (chargen != nullptr);
      for (size_t i = 0; has_basic_ && i < kRomSizeBasic; i++) basic_[i] = basic[i];
      for (size_t i = 0; has_kernal_ && i < kRomSizeKernal; i++) kernal_[i] = kernal[i];
      for (size_t i = 0; has_chargen_ && i < kRomSizeChargen; i++) chargen_[i] = chargen[i];
    }

    /** @brief The MMU view of the stored images. */
    Roms roms(void) const
    {
      Roms r;
      r.basic = has_basic_ ? basic_ : nullptr;
      r.kernal = has_kernal_ ? kernal_ : stub_kernal();
      r.chargen = has_chargen_ ? chargen_ : nullptr;
      r.real_kernal = has_kernal_;
      return r;
    }

  private:
    data_t basic_[kRomSizeBasic] = { 0 };
    data_t kernal_[kRomSizeKernal] = { 0 };
    data_t chargen_[kRomSizeChargen] = { 0 };
    bool has_basic_ = false;
    bool has_kernal_ = false;
    bool has_chargen_ = false;
};
#endif

} /* namespace usbsid */

#endif /* _US_MEM_ROM_H_ */
