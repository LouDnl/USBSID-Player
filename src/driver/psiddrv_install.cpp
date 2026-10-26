/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * psiddrv_install.cpp
 *
 * Parameter block layout and field order, the driver reads them from
 * fixed offsets
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

#include <cstring>

#include "psiddrv_install.h"

extern "C" int reloc65(char ** buf, int * fsize, int addr);

namespace usbsid {

namespace {

/* The assembled driver, as an o65 relocatable object */
#include "psiddrv.h"

/* Cartridge signature address needed for autostart */
constexpr addr_t kCbm80Addr = 0x8000;
constexpr uint16_t kCbm80Size = 9;

/**
 * @brief Install the autostart signature at $8000 and back up what was there
 *
 * The nine bytes at addr are the driver cbm80 field, the driver cold start
 * copies them to $8000. They are a backup, the signature goes to $8000 (per
 * byte save $8000 into the driver, then overwrite $8000)
 *
 * @param vec Cold start vector of the signature
 * @param addr Driver backup field, reloc_addr + 12
 */
uint16_t install_cbm80(Ram & ram, addr_t vec, addr_t addr)
{
  const data_t cbm80[kCbm80Size] = {
    static_cast<data_t>(vec & 0xff), static_cast<data_t>(vec >> 8),
    0x00, 0x00,                       /* NMI vector, unused */
    0xc3, 0xc2, 0xcd, 0x38, 0x30      /* "CBM80" in screen codes */
  };

  for (uint16_t i = 0; i < kCbm80Size; i++) {
    const addr_t at = static_cast<addr_t>(kCbm80Addr + i);
    ram.dma_write(static_cast<addr_t>(addr + i), ram.dma_read(at));
    ram.dma_write(at, cbm80[i]);
  }
  return kCbm80Size;
}

} /* namespace */

bool psiddrv_install(Ram & ram, const SidFile & tune, uint16_t song,
                     bool is_pal, addr_t & reloc_addr_out)
{
  if (!tune.valid || tune.data == nullptr) return false;

  /* Driver page. A file that declares a free page is believed, else use the page
   * after the tune image (old player and psid64 do the same). Page 4 is
   * overwritten by the cursor blink and a high page risks the stack page */
  uint8_t start_page = tune.start_page;
  if (start_page == 0 || start_page == 0xff) {
    const addr_t image_end = (tune.load_last_addr != 0)
      ? tune.load_last_addr
      : static_cast<addr_t>(tune.load_addr + tune.data_size);

    /* Pages the driver may not have (psid64):
     *   $00-$03  zero page, stack, KERNAL workspace and vectors
     *   $a0-$bf  BASIC ROM
     *   $d0-$ff  I/O and KERNAL
     *   the tune image
     * $c0-$cf is left free (RAM between BASIC and I/O) */
    bool used[0x100];
    for (unsigned i = 0; i < 0x100; i++) used[i] = false;
    const auto mark = [&used](unsigned lo, unsigned hi) {
      for (unsigned i = lo; i <= hi && i < 0x100; i++) used[i] = true;
    };
    mark(0x00, 0x03);
    mark(0xa0, 0xbf);
    mark(0xd0, 0xff);
    mark(tune.load_addr >> 8, image_end >> 8);

    /* Driver is two pages, like the psid64 minimal one */
    constexpr unsigned kDriverPages = 2;
    /* $0400-$07e7 is the default screen, last choice for the driver */
    const auto is_screen = [](unsigned page) { return page >= 0x04 && page <= 0x07; };

    unsigned best = 0, best_len = 0;      /* largest gap clear of the screen */
    unsigned fallback = 0;                /* largest gap, screen included */
    unsigned fallback_len = 0;
    unsigned run_start = 0, run = 0;
    for (unsigned i = 0; i <= 0x100; i++) {
      const bool free_here = (i < 0x100) && !used[i];
      if (free_here) { if (run == 0) run_start = i; run++; continue; }
      if (run >= kDriverPages) {
        if (run > fallback_len) { fallback_len = run; fallback = run_start; }
        /* Trim the screen off the front of a run, $04-$0f is good from $08 */
        unsigned s = run_start, n = run;
        while (n > 0 && is_screen(s)) { s++; n--; }
        if (n >= kDriverPages && n > best_len) { best_len = n; best = s; }
      }
      run = 0;
    }

    if (best_len >= kDriverPages)          start_page = static_cast<uint8_t>(best);
    else if (fallback_len >= kDriverPages) start_page = static_cast<uint8_t>(fallback);
    else                                   start_page = 0x04;
  }

  const addr_t reloc_addr = static_cast<addr_t>(start_page << 8);
  reloc_addr_out = reloc_addr;

  /* Relocate a private copy, reloc65 rewrites the buffer */
  data_t driver[sizeof(psid_driver)];
  memcpy(driver, psid_driver, sizeof(psid_driver));

  char * reloc = reinterpret_cast<char *>(driver);
  int size = static_cast<int>(sizeof(psid_driver));
  if (!reloc65(&reloc, &size, reloc_addr)) return false;

  for (int i = 0; i < size; i++) {
    ram.dma_write(static_cast<addr_t>(reloc_addr + i),
                  static_cast<data_t>(reloc[i]));
  }

  /* The tune itself */
  for (size_t i = 0; i < tune.data_size; i++) {
    ram.dma_write(static_cast<addr_t>(tune.load_addr + i), tune.data[i]);
  }

  /* Parameter block sits after the driver JMP and CBM80 vector */
  addr_t addr = static_cast<addr_t>(reloc_addr + 3 + 9 + 9);

  ram.dma_write(addr++, 0x00);
  ram.dma_write(addr++, static_cast<data_t>(tune.songs));
  ram.dma_write(addr++, static_cast<data_t>(tune.load_addr & 0xff));
  ram.dma_write(addr++, static_cast<data_t>(tune.load_addr >> 8));
  ram.dma_write(addr++, static_cast<data_t>(tune.init_addr & 0xff));
  ram.dma_write(addr++, static_cast<data_t>(tune.init_addr >> 8));
  ram.dma_write(addr++, static_cast<data_t>(tune.play_addr & 0xff));
  ram.dma_write(addr++, static_cast<data_t>(tune.play_addr >> 8));
  ram.dma_write(addr++, static_cast<data_t>(tune.speed & 0xff));
  ram.dma_write(addr++, static_cast<data_t>((tune.speed >> 8) & 0xff));
  ram.dma_write(addr++, static_cast<data_t>((tune.speed >> 16) & 0xff));
  ram.dma_write(addr++, static_cast<data_t>(tune.speed >> 24));
  ram.dma_write(addr++, static_cast<data_t>(is_pal ? 1 : 0));
  ram.dma_write(addr++, static_cast<data_t>(tune.load_last_addr & 0xff));
  ram.dma_write(addr++, static_cast<data_t>(tune.load_last_addr >> 8));

  /* Signature goes in last, the backup must hold what the tune put at $8000 */
  install_cbm80(ram, static_cast<addr_t>(reloc_addr + kPsidDrvEntryOffset),
                static_cast<addr_t>(reloc_addr + kPsidDrvCbm80Offset));

  psiddrv_set_song(ram, reloc_addr, song, is_pal);
  return true;
}

void psiddrv_set_song(Ram & ram, addr_t reloc_addr, uint16_t song, bool is_pal)
{
  ram.dma_write(static_cast<addr_t>(reloc_addr + kPsidDrvParamOffset),
                static_cast<data_t>(song));

  /* BASIC tunes read the song number out of the KERNAL's A/X/Y store */
  ram.dma_write(780, static_cast<data_t>(song - 1));
  ram.dma_write(781, static_cast<data_t>(song - 1));
  ram.dma_write(782, static_cast<data_t>(song - 1));

  /* Many tunes read the video standard from $02a6, set after the SID flag is read */
  ram.dma_write(0x02a6, static_cast<data_t>(is_pal ? 1 : 0));
}

} /* namespace usbsid */
