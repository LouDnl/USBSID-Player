/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_mmu.cpp
 * Step 2.4 gate: the full 32 entry PLA mode table, the processor port, and
 * the KERNAL booting far enough to print the BASIC banner.
 *
 * The expected bank map below is written out literally from the documented
 * mode table (https://www.c64-wiki.com/wiki/Bank_Switching#Mode_Table) while
 * the PLA itself derives the same thing from rules, so a mistake in one
 * cannot hide in the other.
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

#include <cstdio>

#include "machine.h"
#include "mmu.h"
#include "mos906114_pla.h"
#include "ram.h"
#include "rom.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;

namespace {

/* shorthand for the table below */
constexpr Bank R = Bank::Ram;
constexpr Bank B = Bank::Basic;
constexpr Bank K = Bank::Kernal;
constexpr Bank C = Bank::CharRom;
constexpr Bank I = Bank::Io;
constexpr Bank L = Bank::CartLo;
constexpr Bank H = Bank::CartHi;
constexpr Bank O = Bank::Open;

/* Zones in order: $0000, $1000-$7fff, $8000, $a000, $c000, $d000, $e000 */
const Bank kExpected[32][7] = {
  /* --- EXROM low, GAME low: 16 KB cartridge --------------------------- */
  /* 0  */ { R, R, R, R, R, R, R },
  /* 1  */ { R, R, R, R, R, C, R },
  /* 2  */ { R, R, R, H, R, C, K },
  /* 3  */ { R, R, L, H, R, C, K },
  /* 4  */ { R, R, R, R, R, R, R },
  /* 5  */ { R, R, R, R, R, I, R },
  /* 6  */ { R, R, R, H, R, I, K },
  /* 7  */ { R, R, L, H, R, I, K },
  /* --- EXROM low, GAME high: 8 KB cartridge --------------------------- */
  /* 8  */ { R, R, R, R, R, R, R },
  /* 9  */ { R, R, R, R, R, C, R },
  /* 10 */ { R, R, R, R, R, C, K },
  /* 11 */ { R, R, L, B, R, C, K },
  /* 12 */ { R, R, R, R, R, R, R },
  /* 13 */ { R, R, R, R, R, I, R },
  /* 14 */ { R, R, R, R, R, I, K },
  /* 15 */ { R, R, L, B, R, I, K },
  /* --- EXROM high, GAME low: ultimax, the latches do not matter ------- */
  /* 16 */ { R, O, L, O, O, I, H },
  /* 17 */ { R, O, L, O, O, I, H },
  /* 18 */ { R, O, L, O, O, I, H },
  /* 19 */ { R, O, L, O, O, I, H },
  /* 20 */ { R, O, L, O, O, I, H },
  /* 21 */ { R, O, L, O, O, I, H },
  /* 22 */ { R, O, L, O, O, I, H },
  /* 23 */ { R, O, L, O, O, I, H },
  /* --- EXROM high, GAME high: no cartridge, the normal machine -------- */
  /* 24 */ { R, R, R, R, R, R, R },
  /* 25 */ { R, R, R, R, R, C, R },
  /* 26 */ { R, R, R, R, R, C, K },
  /* 27 */ { R, R, R, B, R, C, K },
  /* 28 */ { R, R, R, R, R, R, R },
  /* 29 */ { R, R, R, R, R, I, R },
  /* 30 */ { R, R, R, R, R, I, K },
  /* 31 */ { R, R, R, B, R, I, K },
};

const char * bank_name(Bank b)
{
  switch (b) {
    case Bank::Ram:     return "RAM";
    case Bank::Basic:   return "BASIC";
    case Bank::Kernal:  return "KERNAL";
    case Bank::CharRom: return "CHAR";
    case Bank::Io:      return "IO";
    case Bank::CartLo:  return "ROML";
    case Bank::CartHi:  return "ROMH";
    default:            return "OPEN";
  }
}

int test_banking_matrix(void)
{
  Mos906114Pla pla;

  for (unsigned mode = 0; mode < 32; mode++) {
    const data_t port = static_cast<data_t>(mode & 0x07);
    const bool game  = (mode & 0x08) != 0;
    const bool exrom = (mode & 0x10) != 0;

    pla.update(port, game, exrom);

    US_CHECK_EQ_U(pla.mode(), mode, "pla reports its own mode number");

    for (unsigned z = 0; z < 7; z++) {
      const Bank got = pla.bank(static_cast<Zone>(z));
      const Bank want = kExpected[mode][z];
      ++us_test_checks;
      if (got != want) {
        ++us_test_failures;
        printf("  FAIL mode %2u zone %u: got %s want %s\n",
               mode, z, bank_name(got), bank_name(want));
      }
    }
  }

  /* the page to zone mapping, spot checked at every boundary */
  Mos906114Pla p2;
  p2.update(0x07, true, true); /* mode 31 */
  US_CHECK(p2.bank_for_page(0x0) == Bank::Ram, "page $0000 is RAM");
  US_CHECK(p2.bank_for_page(0x7) == Bank::Ram, "page $7000 is RAM");
  US_CHECK(p2.bank_for_page(0x9) == Bank::Ram, "page $9000 is the cart zone");
  US_CHECK(p2.bank_for_page(0xa) == Bank::Basic, "page $a000 is BASIC");
  US_CHECK(p2.bank_for_page(0xb) == Bank::Basic, "page $b000 is BASIC");
  US_CHECK(p2.bank_for_page(0xc) == Bank::Ram, "page $c000 is RAM");
  US_CHECK(p2.bank_for_page(0xd) == Bank::Io, "page $d000 is IO");
  US_CHECK(p2.bank_for_page(0xe) == Bank::Kernal, "page $e000 is KERNAL");
  US_CHECK(p2.bank_for_page(0xf) == Bank::Kernal, "page $f000 is KERNAL");

  return 0;
}

/* ---- processor port ---------------------------------------------------- */

int test_processor_port(void)
{
  Machine m;

  US_CHECK_EQ_U(m.mmu().port_dir(), 0x00u, "port direction is input after reset");
  US_CHECK_EQ_U(m.mmu().port_read(), 0x17u, "undriven port reads the pull ups");
  US_CHECK_EQ_U(m.pla().mode(), 31u, "reset lands in mode 31");

  /* what the KERNAL does at boot */
  m.mmu().write(0x0000, 0x2f);
  m.mmu().write(0x0001, 0x37);
  US_CHECK_EQ_U(m.mmu().read(0x0000), 0x2fu, "direction register reads back");
  US_CHECK_EQ_U(m.mmu().read(0x0001), 0x37u, "port reads back what was driven");
  US_CHECK_EQ_U(m.pla().mode(), 31u, "$37 is still mode 31");

  /* bank out the ROMs completely */
  m.mmu().write(0x0001, 0x30);
  US_CHECK_EQ_U(m.mmu().read(0x0001), 0x30u, "port with the latches low");
  US_CHECK_EQ_U(m.pla().mode(), 24u, "all three latches low is mode 24");
  US_CHECK(m.pla().bank(Zone::Kernal) == Bank::Ram, "KERNAL banked out");
  US_CHECK(m.pla().bank(Zone::IoChar) == Bank::Ram, "IO banked out");

  /* input bits keep reading the pull ups even after a write */
  m.mmu().write(0x0000, 0x00);
  m.mmu().write(0x0001, 0x00);
  US_CHECK_EQ_U(m.mmu().read(0x0001), 0x17u, "all inputs read the pull ups again");
  US_CHECK_EQ_U(m.pla().mode(), 31u, "and that is mode 31 again");

  /* the VIC sees plain RAM at $0000 and $0001 */
  US_CHECK_EQ_U(m.ram().dma_read(0x0001), 0x00u, "the RAM under the port keeps a copy");

  return 0;
}

/* ---- what the CPU actually reads --------------------------------------- */

int test_memory_map(void)
{
  Machine m;
  Roms roms;

  /* mode 31: BASIC, IO and KERNAL visible */
  US_CHECK_EQ_U(m.mmu().read(0xa000), roms.basic[0], "BASIC ROM visible at $a000");
  US_CHECK_EQ_U(m.mmu().read(0xbfff), roms.basic[kRomSizeBasic - 1], "end of BASIC");
  US_CHECK_EQ_U(m.mmu().read(0xe000), roms.kernal[0], "KERNAL visible at $e000");
  US_CHECK_EQ_U(m.mmu().read(0xffff), roms.kernal[kRomSizeKernal - 1], "end of KERNAL");
  /* A chip that is fitted answers for its own page. The machine has a VIC,
   * so $d000 is the VIC and not the RAM underneath. */
  m.ram().dma_write(0xd012, 0x5a);
  US_CHECK(m.mmu().read(0xd012) != 0x5au, "the VIC answers for its own page");

  /* An empty socket does not drive the bus, so the access falls through to
   * the RAM underneath instead of floating. The expansion IO page has
   * nothing in it. */
  m.ram().dma_write(0xde00, 0x5a);
  US_CHECK_EQ_U(m.mmu().read(0xde00), 0x5au, "an empty IO slot reads the RAM under it");
  m.mmu().write(0xde01, 0x3c);
  US_CHECK_EQ_U(m.ram().dma_read(0xde01), 0x3cu, "and writes land there too");

  /* the reset vector the CPU came up on */
  const addr_t reset_vector = static_cast<addr_t>(
    roms.kernal[0xfffc - kAddrKernalFirstPage] |
    (roms.kernal[0xfffd - kAddrKernalFirstPage] << 8));
  US_CHECK_EQ_U(m.cpu().pc(), reset_vector, "cpu started at the KERNAL reset vector");
  US_CHECK_EQ_U(reset_vector, 0xfce2u, "the stock KERNAL reset vector");

  /* writes go through the ROM into the RAM underneath */
  m.mmu().write(0xe000, 0x5a);
  US_CHECK_EQ_U(m.mmu().read(0xe000), roms.kernal[0], "the ROM still reads back");
  US_CHECK_EQ_U(m.ram().read(0xe000), 0x5au, "the write landed in RAM");

  m.mmu().write(0x0000, 0x2f);
  m.mmu().write(0x0001, 0x30);                     /* everything RAM */
  US_CHECK_EQ_U(m.mmu().read(0xe000), 0x5au, "and it is visible once banked out");

  /* character generator */
  m.mmu().write(0x0001, 0x33);                     /* loram+hiram, charen low */
  US_CHECK_EQ_U(m.pla().mode(), 27u, "charen low is mode 27");
  US_CHECK_EQ_U(m.mmu().read(0xd000), roms.chargen[0], "character ROM at $d000");

  /* colour RAM lives inside the IO block and is four bits wide */
  m.mmu().write(0x0001, 0x37);
  m.mmu().write(0xd800, 0xff);
  US_CHECK_EQ_U(m.mmu().read(0xd800), 0x0fu, "colour RAM is four bits wide");

  return 0;
}

/* ---- the VIC side of memory -------------------------------------------- */

int test_vic_view(void)
{
  Machine m;
  Roms roms;

  m.ram().dma_write(0x1000, 0x11);
  m.ram().dma_write(0x5000, 0x22);

  /* bank 0: the character generator covers $1000-$1fff */
  m.mmu().set_vic_bank(0);
  US_CHECK_EQ_U(m.mmu().vic_read(0x1000), roms.chargen[0],
                "the VIC sees the character ROM in bank 0");
  US_CHECK_EQ_U(m.mmu().vic_read(0x0000), m.ram().dma_read(0x0000),
                "and plain RAM elsewhere");

  /* bank 1: no character generator, RAM all the way */
  m.mmu().set_vic_bank(1);
  US_CHECK_EQ_U(m.mmu().vic_read(0x1000), 0x22u,
                "bank 1 has no character ROM, so $5000 shows through");

  return 0;
}

/* ---- the KERNAL boot --------------------------------------------------- */

/* PETSCII screen codes for the strings the boot writes */
bool screen_contains(Ram & ram, const data_t * codes, size_t len)
{
  for (addr_t base = 0x0400; base + len < 0x07e8; base++) {
    size_t i = 0;
    while (i < len && ram.dma_read(static_cast<addr_t>(base + i)) == codes[i]) i++;
    if (i == len) return true;
  }
  return false;
}

int test_kernal_boot(void)
{
  /* The machine is complete apart from the SID now, so this is the real boot
   * path: KERNAL init, the screen editor's raster wait, BASIC's banner. */
  constexpr uint64_t kCycleLimit = 20ull * 1000ull * 1000ull;

  /* "COMMODORE 64 BASIC" and "READY." in screen codes */
  const data_t kBasic[] = { 0x02, 0x01, 0x13, 0x09, 0x03 };            /* BASIC */
  const data_t kReady[] = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2e };      /* READY. */

  Machine m;

  bool saw_basic = false;
  bool saw_ready = false;
  uint64_t cycles = 0;

  while (cycles < kCycleLimit) {
    m.run(10000);
    cycles += 10000;
    if (m.cpu().jammed()) break;
    if (!saw_basic) saw_basic = screen_contains(m.ram(), kBasic, sizeof(kBasic));
    if (!saw_ready) saw_ready = screen_contains(m.ram(), kReady, sizeof(kReady));
    if (saw_basic && saw_ready) break;
  }

  if (m.cpu().jammed()) {
    ++us_test_failures;
    ++us_test_checks;
    printf("  FAIL kernal boot: cpu jammed on $%02x at $%04x\n",
           m.cpu().opcode(), m.cpu().instruction_pc());
    return 1;
  }

  US_CHECK(saw_basic, "the BASIC banner reached screen memory");
  US_CHECK(saw_ready, "the READY prompt reached screen memory");

  if (saw_basic && saw_ready) {
    printf("  kernal booted to READY after %llu cycles, pc $%04x\n",
           static_cast<unsigned long long>(cycles), m.cpu().pc());
  } else {
    printf("  boot stopped at pc $%04x after %llu cycles "
           "(banner %s, ready %s)\n",
           m.cpu().pc(), static_cast<unsigned long long>(cycles),
           saw_basic ? "yes" : "no", saw_ready ? "yes" : "no");
  }

  return 0;
}

} /* namespace */

int us_test_mmu(void)
{
  US_TEST_BEGIN("mem/mmu");

  test_banking_matrix();
  test_processor_port();
  test_memory_map();
  test_vic_view();
  test_kernal_boot();

  US_TEST_END("mem/mmu");
}

US_TEST_MAIN(us_test_mmu)
