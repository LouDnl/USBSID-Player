/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * stub_kernal.cpp
 * A free replacement KERNAL for playing tunes without the Commodore ROMs.
 * Layout after libsidplayfp's KernalRomBank (SystemROMBanks.h): the entry
 * points a PSID player and tune rips touch, at their stock addresses.
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

#include <cstring>

#include "rom.h"

namespace usbsid {

namespace {

constexpr addr_t kBase = 0xe000;

/* 6502 opcodes used below */
constexpr data_t kNop    = 0xea;
constexpr data_t kRts    = 0x60;
constexpr data_t kRti    = 0x40;
constexpr data_t kPha    = 0x48;
constexpr data_t kPla    = 0x68;
constexpr data_t kTxa    = 0x8a;
constexpr data_t kTax    = 0xaa;
constexpr data_t kTya    = 0x98;
constexpr data_t kTay    = 0xa8;
constexpr data_t kTsx    = 0xba;
constexpr data_t kTxs    = 0x9a;
constexpr data_t kSei    = 0x78;
constexpr data_t kCld    = 0xd8;
constexpr data_t kDex    = 0xca;
constexpr data_t kLdxImm = 0xa2;
constexpr data_t kLdaAbs = 0xad;
constexpr data_t kLdaAbx = 0xbd;
constexpr data_t kStaAbs = 0x8d;
constexpr data_t kStxAbs = 0x8e;
constexpr data_t kStaZp  = 0x85;
constexpr data_t kLdaImm = 0xa9;
constexpr data_t kOraImm = 0x09;
constexpr data_t kCmpAbx = 0xdd;
constexpr data_t kAndImm = 0x29;
constexpr data_t kBne    = 0xd0;
constexpr data_t kBeq    = 0xf0;
constexpr data_t kJsr    = 0x20;
constexpr data_t kJmp    = 0x4c;
constexpr data_t kJmpInd = 0x6c;

/* Jump table slot and stock routine address of each KERNAL call, see
 * https://sta.c64.org/cbm64krnfunc.html. Both get an RTS, for rips that
 * call either. */
constexpr addr_t kKernalCalls[] = {
  0xff81, 0xff5b,  0xff84, 0xfda3,  0xff87, 0xfd50,  0xff8a, 0xfd15,
  0xff8d, 0xfd1a,  0xff90, 0xfe18,  0xff93, 0xedb9,  0xff96, 0xedc7,
  0xff99, 0xfe25,  0xff9c, 0xfe34,  0xff9f, 0xea87,  0xffa2, 0xfe21,
  0xffa5, 0xee13,  0xffa8, 0xeddd,  0xffab, 0xedef,  0xffae, 0xedfe,
  0xffb1, 0xed0c,  0xffb4, 0xed09,  0xffb7, 0xfe07,  0xffba, 0xfe00,
  0xffbd, 0xfdf9,  0xffc0, 0xf34a,  0xffc3, 0xf291,  0xffc6, 0xf20e,
  0xffc9, 0xf250,  0xffcc, 0xf333,  0xffcf, 0xf157,  0xffd2, 0xf1ca,
  0xffd5, 0xf49e,  0xffd8, 0xf5dd,  0xffdb, 0xf6e4,  0xffde, 0xf6dd,
  0xffe1, 0xf6ed,  0xffe4, 0xf13e,  0xffe7, 0xf32f,  0xffea, 0xf69b,
  0xffed, 0xe505,  0xfff0, 0xe50a,  0xfff3, 0xe500,
};

/**
 * @brief Copy bytes into the image at a C64 address.
 *
 * @param rom   image for $e000-$ffff
 * @param addr  C64 address of the first byte
 * @param bytes code or data
 * @param len   number of bytes
 */
void put(data_t * rom, addr_t addr, const data_t * bytes, size_t len)
{
  memcpy(rom + (addr - kBase), bytes, len);
}

/**
 * @brief Build the stub into rom.
 *
 * @param rom kRomSizeKernal byte destination
 */
void build(data_t * rom)
{
  memset(rom, kNop, kRomSizeKernal);

  for (addr_t addr : kKernalCalls) rom[addr - kBase] = kRts;

  /* $ea31 default IRQ handler: acknowledge CIA1 and return */
  const data_t irq_handler[] = { kJmp, 0x7e, 0xea };
  put(rom, 0xea31, irq_handler, sizeof(irq_handler));
  const data_t irq_exit[] = {
    kLdaAbs, 0x0d, 0xdc,          /* $ea7e acknowledge CIA1 */
    kPla, kTay, kPla, kTax, kPla, /* $ea81 restore registers */
    kRti,
  };
  put(rom, 0xea7e, irq_exit, sizeof(irq_exit));

  /* $fce2 reset: start a cartridge with a CBM80 signature, otherwise set up
   * the IO chips like the stock IOINIT and idle */
  const data_t reset[] = {
    kLdxImm, 0xff, kSei, kTxs, kCld,
    kJsr, 0x02, 0xfd,             /* cartridge check */
    kBne, 0x03,
    kJmpInd, 0x00, 0x80,          /* cartridge cold start vector */
    kStxAbs, 0x16, 0xd0,          /* $fcef */
    kJsr, 0xa3, 0xfd,             /* IOINIT */
    kJmp, 0xf5, 0xfc,             /* $fcf5 idle loop */
  };
  put(rom, 0xfce2, reset, sizeof(reset));

  /* $fda3 IOINIT: CIA ports and timers, SID volume, VIC bank, CPU port,
   * CIA1 timer A at the jiffy rate ($02a6: 0 NTSC, else PAL) */
  const data_t ioinit[] = {
    kLdaImm, 0x7f, kStaAbs, 0x0d, 0xdc, kStaAbs, 0x0d, 0xdd, kStaAbs, 0x00, 0xdc,
    kLdaImm, 0x08, kStaAbs, 0x0e, 0xdc, kStaAbs, 0x0e, 0xdd,
    kStaAbs, 0x0f, 0xdc, kStaAbs, 0x0f, 0xdd,
    kLdxImm, 0x00, kStxAbs, 0x03, 0xdc, kStxAbs, 0x03, 0xdd, kStxAbs, 0x18, 0xd4,
    kDex, kStxAbs, 0x02, 0xdc,
    kLdaImm, 0x07, kStaAbs, 0x00, 0xdd, kLdaImm, 0x3f, kStaAbs, 0x02, 0xdd,
    kLdaImm, 0xe7, kStaZp, 0x01, kLdaImm, 0x2f, kStaZp, 0x00,
    kLdaAbs, 0xa6, 0x02, kBeq, 0x0a,
    kLdaImm, 0x25, kStaAbs, 0x04, 0xdc, kLdaImm, 0x40, kJmp, 0xf3, 0xfd,
    kLdaImm, 0x95, kStaAbs, 0x04, 0xdc, kLdaImm, 0x42,
    kStaAbs, 0x05, 0xdc,          /* $fdf3 */
    kJmp, 0x6e, 0xff,
  };
  put(rom, 0xfda3, ioinit, sizeof(ioinit));
  const data_t timer_start[] = {
    kLdaImm, 0x81, kStaAbs, 0x0d, 0xdc,                /* $ff6e timer A IRQ */
    kLdaAbs, 0x0e, 0xdc, kAndImm, 0x80, kOraImm, 0x11, kStaAbs, 0x0e, 0xdc,
    kJmp, 0x8e, 0xee,
  };
  put(rom, 0xff6e, timer_start, sizeof(timer_start));
  const data_t serial_clock[] = {
    kLdaAbs, 0x00, 0xdd, kOraImm, 0x10, kStaAbs, 0x00, 0xdd, kRts,  /* $ee8e */
  };
  put(rom, 0xee8e, serial_clock, sizeof(serial_clock));

  /* $fd02 cartridge check, Z set when $8004-$8008 holds CBM80 */
  const data_t cart_check[] = {
    kLdxImm, 0x05,
    kLdaAbx, 0x0f, 0xfd,
    kCmpAbx, 0x03, 0x80,
    kBne, 0x03,
    kDex,
    kBne, 0xf5,
    kRts,
    0xc3, 0xc2, 0xcd, 0x38, 0x30, /* $fd10 "CBM80" */
  };
  put(rom, 0xfd02, cart_check, sizeof(cart_check));

  /* $fe43 NMI entry and default handler: acknowledge CIA2 and return */
  const data_t nmi_entry[] = {
    kSei, kJmpInd, 0x18, 0x03,    /* jmp ($0318) */
    kPha, kTxa, kPha, kTya, kPha, /* $fe47 */
    kLdaAbs, 0x0d, 0xdd,          /* acknowledge CIA2 */
    kJmp, 0xbc, 0xfe,
  };
  put(rom, 0xfe43, nmi_entry, sizeof(nmi_entry));
  const data_t nmi_exit[] = { kPla, kTay, kPla, kTax, kPla, kRti };
  put(rom, 0xfebc, nmi_exit, sizeof(nmi_exit));

  /* $ff48 IRQ/BRK entry, the stock sequence for the stock cycle count */
  const data_t irq_entry[] = {
    kPha, kTxa, kPha, kTya, kPha,
    kTsx,
    kLdaAbx, 0x04, 0x01,          /* status byte pushed by the interrupt */
    kAndImm, 0x10,                /* B flag */
    kBeq, 0x03,
    kJmpInd, 0x16, 0x03,          /* BRK: jmp ($0316) */
    kJmpInd, 0x14, 0x03,          /* IRQ: jmp ($0314) */
  };
  put(rom, 0xff48, irq_entry, sizeof(irq_entry));

  /* Hardware vectors */
  const data_t vectors[] = { 0x43, 0xfe, 0xe2, 0xfc, 0x48, 0xff };
  put(rom, 0xfffa, vectors, sizeof(vectors));
}

} /* namespace */

const data_t * stub_kernal(void)
{
  static data_t rom[kRomSizeKernal];
  static bool built = false;
  if (!built) {
    build(rom);
    built = true;
  }
  return rom;
}

} /* namespace usbsid */
