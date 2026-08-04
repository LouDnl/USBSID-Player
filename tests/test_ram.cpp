/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_ram.cpp
 * Step 2.1 gate: RAM storage, power on pattern, colour RAM width and bulk load.
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

#include "ram.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;

int us_test_ram(void)
{
  US_TEST_BEGIN("mem/ram");

  Ram ram;

  /* ---- power on pattern: 64 bytes $00, 64 bytes $ff, repeating ---- */
  US_CHECK_EQ_U(ram.read(0x0000), 0x00u, "block 0 is $00");
  US_CHECK_EQ_U(ram.read(0x003f), 0x00u, "end of block 0");
  US_CHECK_EQ_U(ram.read(0x0040), 0xffu, "block 1 is $ff");
  US_CHECK_EQ_U(ram.read(0x007f), 0xffu, "end of block 1");
  US_CHECK_EQ_U(ram.read(0x0080), 0x00u, "block 2 is $00 again");
  US_CHECK_EQ_U(ram.read(0xffff), 0xffu, "last byte follows the pattern");

  /* ---- plain storage ---- */
  ram.write(0x1000, 0x5a);
  US_CHECK_EQ_U(ram.read(0x1000), 0x5au, "write then read");
  US_CHECK_EQ_U(ram.dma_read(0x1000), 0x5au, "dma_read sees the same byte");
  ram.dma_write(0x1001, 0xa5);
  US_CHECK_EQ_U(ram.read(0x1001), 0xa5u, "dma_write sees the same byte");

  /* no aliasing between neighbours. $0fff is the last byte of block 63, which
   * is an odd block, so the power on pattern leaves it at $ff. */
  US_CHECK_EQ_U(ram.read(0x0fff), 0xffu, "byte below untouched");
  US_CHECK_EQ_U(ram.read(0x1002), 0x00u, "byte above untouched");

  /* ---- fill, used by the all-RAM CPU test machine ---- */
  ram.fill(0x00);
  US_CHECK_EQ_U(ram.read(0x0000), 0x00u, "fill start");
  US_CHECK_EQ_U(ram.read(0x0040), 0x00u, "fill overwrites the pattern");
  US_CHECK_EQ_U(ram.read(0xffff), 0x00u, "fill end");

  /* ---- reset restores the pattern ---- */
  ram.reset();
  US_CHECK_EQ_U(ram.read(0x0040), 0xffu, "reset restores the pattern");

  /* ---- colour RAM is 4 bits wide and 1 KB long ---- */
  ram.write_color(0x0000, 0xff);
  US_CHECK_EQ_U(ram.read_color(0x0000), 0x0fu, "colour ram masks to 4 bits");
  ram.write_color(0x03ff, 0x07);
  US_CHECK_EQ_U(ram.read_color(0x03ff), 0x07u, "last colour ram byte");
  /* $d800 style addresses and bare offsets must land in the same place */
  ram.write_color(0x0123, 0x0c);
  US_CHECK_EQ_U(ram.read_color(0x0400 + 0x0123), 0x0cu, "colour ram wraps at 1 KB");

  /* ---- bulk load ---- */
  const data_t blob[4] = { 0x01, 0x02, 0x03, 0x04 };
  US_CHECK_EQ_U(ram.load(0x2000, blob, 4), 4u, "load returns bytes written");
  US_CHECK_EQ_U(ram.read(0x2000), 0x01u, "loaded first byte");
  US_CHECK_EQ_U(ram.read(0x2003), 0x04u, "loaded last byte");

  /* clamped at the top of memory instead of running off the end */
  US_CHECK_EQ_U(ram.load(0xfffe, blob, 4), 2u, "load clamps at $ffff");
  US_CHECK_EQ_U(ram.read(0xfffe), 0x01u, "clamped load wrote the first byte");
  US_CHECK_EQ_U(ram.read(0xffff), 0x02u, "clamped load wrote the last byte");

  US_CHECK_EQ_U(ram.load(0x3000, nullptr, 4), 0u, "null load is a no-op");
  US_CHECK_EQ_U(ram.load(0x3000, blob, 0), 0u, "zero length load is a no-op");

  US_TEST_END("mem/ram");
}

US_TEST_MAIN(us_test_ram)
