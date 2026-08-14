/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * psiddrv_install.cpp
 *
 * The layout of the parameter block after the driver, and the order the
 * fields go in, follow old player ~ src/psiddrv/psid.cpp exactly. The driver
 * reads them from fixed offsets, so this is not a place to be creative.
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

#include <cstring>

#include "psiddrv_install.h"

extern "C" int reloc65(char ** buf, int * fsize, int addr);

namespace usbsid {

namespace {

/* The assembled driver, as an o65 relocatable object */
#include "psiddrv.h"

/* Where a cartridge signature has to live for the machine to autostart it */
constexpr addr_t kCbm80Addr = 0x8000;
constexpr uint16_t kCbm80Size = 9;

/**
 * @brief Install the autostart signature at $8000, keeping what was there.
 *
 * This is the part that is easy to get backwards, and getting it backwards is
 * silent: the nine bytes at `addr` are the driver's `cbm80` field, and the
 * driver's cold start copies them **to** $8000. They are a *backup*, not a
 * signature. What goes to $8000 is the signature, so that a machine coming out
 * of reset finds a cartridge there and jumps into the driver.
 *
 * So the order per byte is: save $8000 into the driver, then overwrite $8000.
 * A tune whose image covers $8000 gets its own nine bytes back the moment the
 * driver starts. Writing the signature into the driver's field instead makes
 * the driver stamp a cartridge header over the first nine bytes of any tune
 * loaded at $8000, which is exactly as fatal as it sounds.
 *
 * Follows old player ~ src/psiddrv/psid.cpp psid_set_cbm80().
 *
 * @param vec   where the signature's cold start vector points
 * @param addr  the driver's backup field, reloc_addr + 12
 * @return how many bytes it took
 */
uint16_t install_cbm80(Ram & ram, addr_t vec, addr_t addr)
{
  const data_t cbm80[kCbm80Size] = {
    static_cast<data_t>(vec & 0xff), static_cast<data_t>(vec >> 8),
    0x00, 0x00,                       /* the NMI vector, unused */
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

  /* Where the driver may live. A file that declares a free page is believed;
   * one that says nothing gets the page straight after its own image.
   *
   * Page 4 was the fallback, and page 4 is screen memory. Anything that writes
   * to the screen writes through the driver, and the KERNAL's own interrupt
   * blinks a cursor there, so a tune that leaves the KERNAL interrupt running
   * eats its own player. `rsid/Thats_All_Folks.sid` did exactly that: it ran
   * for 33 frames, the driver was overwritten underneath it, and the CPU
   * walked out of the wreckage into BASIC ROM. old player puts the driver
   * after the image, and plays it.
   *
   * Straight after the image, not "anywhere free". Relocating *high*, towards
   * $cf00, was tried once and broke 141 of 160 tunes with a crash into the
   * stack page. Whatever the driver assumes about where it lives, it is not
   * that it can live anywhere; the page above the tune is what the reference
   * player uses and is what this follows. */
  uint8_t start_page = tune.start_page;
  if (start_page == 0 || start_page == 0xff) {
    const addr_t image_end = (tune.load_last_addr != 0)
      ? tune.load_last_addr
      : static_cast<addr_t>(tune.load_addr + tune.data_size);

    /* Which pages the driver may not have, following psid64 by way of
     * emulator-repo's Loader::find_free_page():
     *
     *   $00-$03  zero page, the stack, and the KERNAL's workspace and vectors
     *   $a0-$bf  BASIC ROM
     *   $d0-$ff  I/O and the KERNAL
     *   the tune's own image
     *
     * The gap this used to miss entirely is **$c0-$cf**: four kilobytes of
     * plain RAM between BASIC and the I/O, under no ROM at all. A tune that
     * loads high, like `demos/Combustible_Psychic_Mushrooms.sid` at
     * $0801-$a238, leaves nothing above $9f and was therefore given page $04,
     * which is the screen. See TODO 1 for what that costs.
     */
    bool used[0x100];
    for (unsigned i = 0; i < 0x100; i++) used[i] = false;
    const auto mark = [&used](unsigned lo, unsigned hi) {
      for (unsigned i = lo; i <= hi && i < 0x100; i++) used[i] = true;
    };
    mark(0x00, 0x03);
    mark(0xa0, 0xbf);
    mark(0xd0, 0xff);
    mark(tune.load_addr >> 8, image_end >> 8);

    /* The driver is two pages, the same as psid64's minimal one. */
    constexpr unsigned kDriverPages = 2;
    /* $0400-$07e7 is the default screen. The driver fits there and a tune that
     * prints anything then writes over it, so it is the last choice rather than
     * the first. psid64 reaches the same place from the other end: it looks for
     * somewhere to put a screen first, and then puts the driver where the
     * screen is not. */
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
        /* Trim the screen off the front of a run rather than discarding it:
         * a run of $04-$0f is perfectly good from $08 on. */
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

  /* Relocate a private copy: reloc65 rewrites the buffer in place. */
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

  /* The parameter block sits after the driver's JMP and its CBM80 vector */
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

  /* The autostart signature goes in last, because the backup it takes has to
   * hold whatever the tune put at $8000, not what was there before it loaded. */
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

  /* Many tunes read the video standard from here, and it has to be set after
   * the SID flag has been read. */
  ram.dma_write(0x02a6, static_cast<data_t>(is_pal ? 1 : 0));
}

} /* namespace usbsid */
