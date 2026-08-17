/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_cpu.cpp
 * Step 2.2 gate: the Klaus Dormann 6502 functional test must reach its
 * success trap. The cycle level checks around it are there so a failure says
 * which addressing mode broke instead of only "the test trapped".
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
#include <cstdlib>
#include <vector>

#include "core/bus.h"
#include "flat_memory.h"
#include "mos6510.h"
#include "ram.h"
#include "test_common.h"
#include "tests.h"

#ifndef US_ASSET_DIR
#define US_ASSET_DIR "assets"
#endif

using namespace usbsid;

namespace {

/* A bare CPU on 64 KB of RAM, which is all the 6502 test suites need */
struct TestMachine {
  Ram ram;
  CountingMemory mem{ram};
  Bus bus;
  Mos6510 cpu{mem, bus};

  TestMachine(void)
  {
    ram.fill(0x00);
    bus.attach_cpu(&cpu);
  }

  /* Put an instruction stream at addr and point the CPU at it */
  void code(addr_t addr, const std::vector<data_t> & bytes)
  {
    for (size_t i = 0; i < bytes.size(); i++) {
      ram.write(static_cast<addr_t>(addr + i), bytes[i]);
    }
    cpu.pc(addr);
  }

  /* Run exactly one instruction, return the cycles it took */
  unsigned step(void)
  {
    unsigned cycles = 0;
    do {
      bus.tick();
      ++cycles;
    } while (!cpu.instruction_done());
    return cycles;
  }

  void reset_regs(void)
  {
    cpu.a(0); cpu.x(0); cpu.y(0);
    cpu.sp(0xfd);
    cpu.p(0);
  }
};

/* Run until the CPU is about to execute at target, or give up */
bool run_until_pc(TestMachine & m, addr_t target, unsigned max_cycles = 10000)
{
  for (unsigned i = 0; i < max_cycles; i++) {
    m.bus.tick();
    if (m.cpu.instruction_done() && m.cpu.pc() == target) return true;
  }
  return false;
}

/* ---- cycle counts and addressing ------------------------------------- */

int test_addressing(void)
{
  {
    TestMachine m;
    m.code(0x1000, { 0xa9, 0x42 });          /* LDA #$42 */
    US_CHECK_EQ_U(m.step(), 2u, "LDA # cycles");
    US_CHECK_EQ_U(m.cpu.a(), 0x42u, "LDA # result");
    US_CHECK(m.cpu.flag(kFlagZ) == false, "LDA # clears Z");
    US_CHECK(m.cpu.flag(kFlagN) == false, "LDA # clears N");
  }

  {
    TestMachine m;
    m.ram.write(0x2000, 0x80);
    m.code(0x1000, { 0xad, 0x00, 0x20 });    /* LDA $2000 */
    US_CHECK_EQ_U(m.step(), 4u, "LDA abs cycles");
    US_CHECK_EQ_U(m.cpu.a(), 0x80u, "LDA abs result");
    US_CHECK(m.cpu.flag(kFlagN) == true, "LDA abs sets N");
  }

  {
    /* no page cross: 4 cycles */
    TestMachine m;
    m.ram.write(0x2005, 0x11);
    m.code(0x1000, { 0xbd, 0x00, 0x20 });    /* LDA $2000,X */
    m.cpu.x(0x05);
    US_CHECK_EQ_U(m.step(), 4u, "LDA abs,X without page cross");
    US_CHECK_EQ_U(m.cpu.a(), 0x11u, "LDA abs,X value");
  }

  {
    /* page cross: 5 cycles, and the wrong address is read first */
    TestMachine m;
    m.ram.write(0x2105, 0x22);
    m.code(0x1000, { 0xbd, 0xff, 0x20 });    /* LDA $20ff,X */
    m.cpu.x(0x06);
    US_CHECK_EQ_U(m.step(), 5u, "LDA abs,X with page cross");
    US_CHECK_EQ_U(m.cpu.a(), 0x22u, "LDA abs,X page cross value");
  }

  {
    /* stores always pay the extra cycle and always do the dummy read */
    TestMachine m;
    m.code(0x1000, { 0x9d, 0x00, 0x20 });    /* STA $2000,X */
    m.cpu.a(0x33);
    m.cpu.x(0x01);
    const unsigned reads_before = m.mem.reads;
    US_CHECK_EQ_U(m.step(), 5u, "STA abs,X always 5 cycles");
    US_CHECK_EQ_U(m.ram.read(0x2001), 0x33u, "STA abs,X wrote");
    /* opcode + lo + hi + dummy = 4 reads, 1 write */
    US_CHECK_EQ_U(m.mem.reads - reads_before, 4u, "STA abs,X dummy read happened");
  }

  {
    TestMachine m;
    m.ram.write(0x0080, 0x44);
    m.code(0x1000, { 0xb5, 0x7e });          /* LDA $7e,X */
    m.cpu.x(0x02);
    US_CHECK_EQ_U(m.step(), 4u, "LDA zp,X cycles");
    US_CHECK_EQ_U(m.cpu.a(), 0x44u, "LDA zp,X value");
  }

  {
    /* zero page indexing wraps inside page zero */
    TestMachine m;
    m.ram.write(0x007f, 0x55);
    m.code(0x1000, { 0xb5, 0x80 });          /* LDA $80,X with X=$ff */
    m.cpu.x(0xff);
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), 0x55u, "zp,X wraps at $ff");
  }

  {
    /* (zp,X) */
    TestMachine m;
    m.ram.write(0x0024, 0x00);
    m.ram.write(0x0025, 0x30);
    m.ram.write(0x3000, 0x66);
    m.code(0x1000, { 0xa1, 0x20 });          /* LDA ($20,X) */
    m.cpu.x(0x04);
    US_CHECK_EQ_U(m.step(), 6u, "LDA (zp,X) cycles");
    US_CHECK_EQ_U(m.cpu.a(), 0x66u, "LDA (zp,X) value");
  }

  {
    /* (zp),Y without page cross */
    TestMachine m;
    m.ram.write(0x0020, 0x00);
    m.ram.write(0x0021, 0x30);
    m.ram.write(0x3002, 0x77);
    m.code(0x1000, { 0xb1, 0x20 });          /* LDA ($20),Y */
    m.cpu.y(0x02);
    US_CHECK_EQ_U(m.step(), 5u, "LDA (zp),Y without page cross");
    US_CHECK_EQ_U(m.cpu.a(), 0x77u, "LDA (zp),Y value");
  }

  {
    /* (zp),Y with page cross */
    TestMachine m;
    m.ram.write(0x0020, 0xff);
    m.ram.write(0x0021, 0x30);
    m.ram.write(0x3100, 0x88);
    m.code(0x1000, { 0xb1, 0x20 });
    m.cpu.y(0x01);
    US_CHECK_EQ_U(m.step(), 6u, "LDA (zp),Y with page cross");
    US_CHECK_EQ_U(m.cpu.a(), 0x88u, "LDA (zp),Y page cross value");
  }

  {
    /* the (zp),Y pointer high byte wraps inside page zero */
    TestMachine m;
    m.ram.write(0x00ff, 0x00);
    m.ram.write(0x0000, 0x40);
    m.ram.write(0x4000, 0x99);
    m.code(0x1000, { 0xb1, 0xff });
    m.cpu.y(0x00);
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), 0x99u, "(zp),Y pointer wraps in page zero");
  }

  return 0;
}

/* ---- read modify write, including the dummy write --------------------- */

int test_rmw(void)
{
  {
    TestMachine m;
    m.ram.write(0x0040, 0x0f);
    m.code(0x1000, { 0xe6, 0x40 });          /* INC $40 */
    const unsigned writes_before = m.mem.writes;
    US_CHECK_EQ_U(m.step(), 5u, "INC zp cycles");
    US_CHECK_EQ_U(m.ram.read(0x0040), 0x10u, "INC zp result");
    US_CHECK_EQ_U(m.mem.writes - writes_before, 2u, "INC zp does two writes");
  }

  {
    TestMachine m;
    m.ram.write(0x2000, 0x81);
    m.code(0x1000, { 0x0e, 0x00, 0x20 });    /* ASL $2000 */
    US_CHECK_EQ_U(m.step(), 6u, "ASL abs cycles");
    US_CHECK_EQ_U(m.ram.read(0x2000), 0x02u, "ASL abs result");
    US_CHECK(m.cpu.flag(kFlagC) == true, "ASL abs sets carry");
  }

  {
    /* abs,X read-modify-write is 7 cycles whether or not the page is crossed */
    TestMachine m;
    m.ram.write(0x2001, 0x01);
    m.code(0x1000, { 0xfe, 0x00, 0x20 });    /* INC $2000,X */
    m.cpu.x(0x01);
    US_CHECK_EQ_U(m.step(), 7u, "INC abs,X cycles without page cross");
    US_CHECK_EQ_U(m.ram.read(0x2001), 0x02u, "INC abs,X result");
  }

  {
    TestMachine m;
    m.ram.write(0x2100, 0x01);
    m.code(0x1000, { 0xfe, 0xff, 0x20 });    /* INC $20ff,X */
    m.cpu.x(0x01);
    US_CHECK_EQ_U(m.step(), 7u, "INC abs,X cycles with page cross");
    US_CHECK_EQ_U(m.ram.read(0x2100), 0x02u, "INC abs,X page cross result");
  }

  return 0;
}

/* ---- branches --------------------------------------------------------- */

int test_branches(void)
{
  {
    TestMachine m;
    m.code(0x1000, { 0xd0, 0x10 });          /* BNE +$10, Z set so not taken */
    m.cpu.p(kFlagZ);
    US_CHECK_EQ_U(m.step(), 2u, "branch not taken is 2 cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x1002u, "branch not taken pc");
  }

  {
    TestMachine m;
    m.code(0x1000, { 0xd0, 0x10 });
    m.cpu.p(0);
    US_CHECK_EQ_U(m.step(), 3u, "branch taken same page is 3 cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x1012u, "branch taken pc");
  }

  {
    /* forward across a page boundary */
    TestMachine m;
    m.code(0x10f0, { 0xd0, 0x7f });
    m.cpu.p(0);
    US_CHECK_EQ_U(m.step(), 4u, "branch across a page is 4 cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x1171u, "branch across a page pc");
  }

  {
    /* backwards */
    TestMachine m;
    m.code(0x1010, { 0xd0, 0xfc });          /* BNE -4 */
    m.cpu.p(0);
    US_CHECK_EQ_U(m.step(), 3u, "backwards branch cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x100eu, "backwards branch pc");
  }

  return 0;
}

/* ---- subroutines, stack and interrupts -------------------------------- */

int test_stack_and_jumps(void)
{
  {
    TestMachine m;
    m.code(0x1000, { 0x20, 0x00, 0x20 });    /* JSR $2000 */
    m.cpu.sp(0xff);
    US_CHECK_EQ_U(m.step(), 6u, "JSR cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x2000u, "JSR jumped");
    US_CHECK_EQ_U(m.cpu.sp(), 0xfdu, "JSR pushed two bytes");
    /* the pushed address is the last byte of the JSR, not the next opcode */
    US_CHECK_EQ_U(m.ram.read(0x01ff), 0x10u, "JSR pushed pch");
    US_CHECK_EQ_U(m.ram.read(0x01fe), 0x02u, "JSR pushed pcl");

    /* and back */
    m.ram.write(0x2000, 0x60);               /* RTS */
    US_CHECK_EQ_U(m.step(), 6u, "RTS cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x1003u, "RTS returns after the JSR");
    US_CHECK_EQ_U(m.cpu.sp(), 0xffu, "RTS popped two bytes");
  }

  {
    /* JMP (indirect) with the page wrap bug */
    TestMachine m;
    m.ram.write(0x30ff, 0x34);
    m.ram.write(0x3000, 0x12);               /* the bug reads here, not $3100 */
    m.ram.write(0x3100, 0xff);
    m.code(0x1000, { 0x6c, 0xff, 0x30 });    /* JMP ($30ff) */
    US_CHECK_EQ_U(m.step(), 5u, "JMP (ind) cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x1234u, "JMP (ind) page wrap bug");
  }

  {
    /* PHP pushes B and the unused bit, PLP ignores them */
    TestMachine m;
    m.code(0x1000, { 0x08 });                /* PHP */
    m.cpu.sp(0xff);
    m.cpu.p(kFlagC | kFlagZ);
    US_CHECK_EQ_U(m.step(), 3u, "PHP cycles");
    US_CHECK_EQ_U(m.ram.read(0x01ff), (kFlagC | kFlagZ | kFlagB | kFlagU),
                  "PHP pushes B and unused set");

    m.code(0x1001, { 0x28 });                /* PLP */
    m.ram.write(0x01ff, 0x00);
    US_CHECK_EQ_U(m.step(), 4u, "PLP cycles");
    US_CHECK_EQ_U(m.cpu.p(), kFlagU, "PLP forces the unused bit set");
    US_CHECK(m.cpu.flag(kFlagB) == false, "PLP does not set B");
  }

  {
    /* BRK is 7 cycles, pushes B set, and vectors through $fffe */
    TestMachine m;
    m.ram.write(0xfffe, 0x00);
    m.ram.write(0xffff, 0x40);
    m.code(0x1000, { 0x00, 0xaa });           /* BRK, signature byte */
    m.cpu.sp(0xff);
    m.cpu.p(0);
    US_CHECK_EQ_U(m.step(), 7u, "BRK cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x4000u, "BRK vector");
    US_CHECK_EQ_U(m.ram.read(0x01ff), 0x10u, "BRK pushed pch");
    US_CHECK_EQ_U(m.ram.read(0x01fe), 0x02u, "BRK pushed pcl past the signature");
    US_CHECK_EQ_U(m.ram.read(0x01fd), (kFlagB | kFlagU), "BRK pushed B set");
    US_CHECK(m.cpu.flag(kFlagI) == true, "BRK sets I");

    /* RTI restores the flags and the address, without the extra bump */
    m.code(0x4000, { 0x40 });
    US_CHECK_EQ_U(m.step(), 6u, "RTI cycles");
    US_CHECK_EQ_U(m.cpu.pc(), 0x1002u, "RTI returns to the pushed address");
  }

  return 0;
}

/* ---- arithmetic, including decimal ------------------------------------ */

int test_arithmetic(void)
{
  struct AdcCase { data_t a, v; bool carry_in, decimal; data_t result; bool c, v_flag; };
  const AdcCase adc_cases[] = {
    /* binary */
    { 0x01, 0x01, false, false, 0x02, false, false },
    { 0xff, 0x01, false, false, 0x00, true,  false },
    { 0x7f, 0x01, false, false, 0x80, false, true  }, /* signed overflow */
    { 0x80, 0xff, false, false, 0x7f, true,  true  },
    { 0x3f, 0x40, true,  false, 0x80, false, true  },
    /* decimal. N and V come from the binary intermediate before the +$60
     * correction, which is why $50+$50 and $58+$46 set V on a real 6502. */
    { 0x09, 0x01, false, true,  0x10, false, false },
    { 0x99, 0x01, false, true,  0x00, true,  false },
    { 0x50, 0x50, false, true,  0x00, true,  true  },
    { 0x12, 0x34, false, true,  0x46, false, false },
    { 0x58, 0x46, true,  true,  0x05, true,  true  },
  };
  for (const AdcCase & c : adc_cases) {
    TestMachine m;
    m.code(0x1000, { 0x69, c.v });           /* ADC #v */
    m.cpu.a(c.a);
    m.cpu.p(static_cast<data_t>((c.carry_in ? kFlagC : 0) |
                                (c.decimal ? kFlagD : 0)));
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), c.result, "ADC result");
    US_CHECK(m.cpu.flag(kFlagC) == c.c, "ADC carry out");
    US_CHECK(m.cpu.flag(kFlagV) == c.v_flag, "ADC overflow");
  }

  struct SbcCase { data_t a, v; bool carry_in, decimal; data_t result; bool c, v_flag; };
  const SbcCase sbc_cases[] = {
    { 0x05, 0x03, true,  false, 0x02, true,  false },
    { 0x05, 0x06, true,  false, 0xff, false, false },
    { 0x80, 0x01, true,  false, 0x7f, true,  true  },
    { 0x00, 0x01, false, false, 0xfe, false, false },
    /* decimal */
    { 0x46, 0x12, true,  true,  0x34, true,  false },
    { 0x12, 0x21, true,  true,  0x91, false, false },
    { 0x00, 0x01, true,  true,  0x99, false, false },
  };
  for (const SbcCase & c : sbc_cases) {
    TestMachine m;
    m.code(0x1000, { 0xe9, c.v });           /* SBC #v */
    m.cpu.a(c.a);
    m.cpu.p(static_cast<data_t>((c.carry_in ? kFlagC : 0) |
                                (c.decimal ? kFlagD : 0)));
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), c.result, "SBC result");
    US_CHECK(m.cpu.flag(kFlagC) == c.c, "SBC carry out");
    US_CHECK(m.cpu.flag(kFlagV) == c.v_flag, "SBC overflow");
  }

  {
    /* CMP sets carry on greater or equal and does not touch A */
    TestMachine m;
    m.code(0x1000, { 0xc9, 0x10 });
    m.cpu.a(0x20);
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), 0x20u, "CMP leaves A alone");
    US_CHECK(m.cpu.flag(kFlagC) == true, "CMP carry when A >= operand");
    US_CHECK(m.cpu.flag(kFlagZ) == false, "CMP zero clear when different");
  }

  {
    /* BIT copies bits 7 and 6 of the operand into N and V */
    TestMachine m;
    m.ram.write(0x0040, 0xc0);
    m.code(0x1000, { 0x24, 0x40 });
    m.cpu.a(0x01);
    US_CHECK_EQ_U(m.step(), 3u, "BIT zp cycles");
    US_CHECK(m.cpu.flag(kFlagN) == true, "BIT sets N from bit 7");
    US_CHECK(m.cpu.flag(kFlagV) == true, "BIT sets V from bit 6");
    US_CHECK(m.cpu.flag(kFlagZ) == true, "BIT sets Z from A and operand");
  }

  return 0;
}

/* ---- BA stalling ------------------------------------------------------ */

int test_ba_stall(void)
{
  TestMachine m;
  m.code(0x1000, { 0xa9, 0x01, 0xa9, 0x02, 0xa9, 0x03, 0xa9, 0x04,
                   0xa9, 0x05, 0xa9, 0x06 });

  /* BA low: a read cycle waits, and it waits from the first cycle.
   *
   * No grace period here. The three cycles a 6510 keeps running after BA drops
   * are the VIC's doing: it pulls BA low three cycles before it takes the bus,
   * so by the time the fetches start the CPU has had them. Counting them again
   * in the CPU steals 40 cycles from it where the hardware steals 43, which is
   * three cycles a bad line, every bad line. See TODO 33 and
   * `Mos6510::ba_allows_cycle()`. */
  m.bus.set_ba(false);
  const addr_t pc_stalled = m.cpu.pc();
  m.bus.run(20);
  US_CHECK_EQ_U(m.cpu.pc(), pc_stalled, "a read waits from the first BA low cycle");

  m.bus.set_ba(true);
  m.bus.run(2);
  US_CHECK(m.cpu.pc() != pc_stalled, "cpu resumes when BA returns");

  /* A write goes through: the CPU drives the bus for those, whatever the VIC
   * wants. BA has to drop once the store's own read cycles are behind it,
   * because those wait like any other read; it is the write cycle itself that
   * is allowed through. */
  {
    TestMachine w;
    w.ram.write(0x0040, 0x00);
    w.code(0x1000, { 0xa9, 0x77, 0x85, 0x40 });  /* LDA #$77 / STA $40 */
    w.bus.run(4);            /* LDA, then the store's opcode and operand */
    w.bus.set_ba(false);
    w.bus.run(1);            /* the write cycle */
    US_CHECK_EQ_U(w.ram.read(0x0040), 0x77u, "a write still reaches memory");
  }

  return 0;
}

/* ---- undocumented opcodes --------------------------------------------- */

int test_illegal_opcodes(void)
{
  {
    /* SLO zp: ASL the memory, ORA the result into A, flags from A */
    TestMachine m;
    m.ram.write(0x0040, 0x81);
    m.code(0x1000, { 0x07, 0x40 });
    m.cpu.a(0x01);
    US_CHECK_EQ_U(m.step(), 5u, "SLO zp cycles");
    US_CHECK_EQ_U(m.ram.read(0x0040), 0x02u, "SLO shifted memory");
    US_CHECK_EQ_U(m.cpu.a(), 0x03u, "SLO or'ed into A");
    US_CHECK(m.cpu.flag(kFlagC) == true, "SLO carry from the shift");
  }

  {
    /* RLA abs,X: 7 cycles whatever happens */
    TestMachine m;
    m.ram.write(0x2001, 0x40);
    m.code(0x1000, { 0x3f, 0x00, 0x20 });
    m.cpu.a(0xff);
    m.cpu.x(0x01);
    US_CHECK_EQ_U(m.step(), 7u, "RLA abs,X cycles");
    US_CHECK_EQ_U(m.ram.read(0x2001), 0x80u, "RLA rotated memory");
    US_CHECK_EQ_U(m.cpu.a(), 0x80u, "RLA and'ed into A");
  }

  {
    /* SRE and RRA */
    TestMachine m;
    m.ram.write(0x0040, 0x03);
    m.code(0x1000, { 0x47, 0x40 });          /* SRE $40 */
    m.cpu.a(0xf0);
    m.step();
    US_CHECK_EQ_U(m.ram.read(0x0040), 0x01u, "SRE shifted memory");
    US_CHECK_EQ_U(m.cpu.a(), 0xf1u, "SRE eor'ed into A");
    US_CHECK(m.cpu.flag(kFlagC) == true, "SRE carry from bit 0");

    m.ram.write(0x0041, 0x02);
    m.code(0x1001, { 0x67, 0x41 });          /* RRA $41 */
    m.cpu.a(0x10);
    m.cpu.p(0);
    m.step();
    US_CHECK_EQ_U(m.ram.read(0x0041), 0x01u, "RRA rotated memory");
    US_CHECK_EQ_U(m.cpu.a(), 0x11u, "RRA added into A");
  }

  {
    /* SAX stores A AND X and touches no flags */
    TestMachine m;
    m.code(0x1000, { 0x87, 0x40 });
    m.cpu.a(0xf0);
    m.cpu.x(0x3c);
    m.cpu.p(0);
    US_CHECK_EQ_U(m.step(), 3u, "SAX zp cycles");
    US_CHECK_EQ_U(m.ram.read(0x0040), 0x30u, "SAX stored A and X");
    US_CHECK_EQ_U(m.cpu.p(), kFlagU, "SAX leaves the flags alone");
  }

  {
    /* LAX loads both registers, with the usual page cross penalty */
    TestMachine m;
    m.ram.write(0x2000, 0x8f);
    m.code(0x1000, { 0xaf, 0x00, 0x20 });    /* LAX $2000 */
    US_CHECK_EQ_U(m.step(), 4u, "LAX abs cycles");
    US_CHECK_EQ_U(m.cpu.a(), 0x8fu, "LAX loaded A");
    US_CHECK_EQ_U(m.cpu.x(), 0x8fu, "LAX loaded X");
    US_CHECK(m.cpu.flag(kFlagN) == true, "LAX sets N");
  }

  {
    /* DCP and ISC */
    TestMachine m;
    m.ram.write(0x0040, 0x10);
    m.code(0x1000, { 0xc7, 0x40 });          /* DCP $40 */
    m.cpu.a(0x0f);
    m.step();
    US_CHECK_EQ_U(m.ram.read(0x0040), 0x0fu, "DCP decremented memory");
    US_CHECK(m.cpu.flag(kFlagZ) == true, "DCP compared A with the result");
    US_CHECK(m.cpu.flag(kFlagC) == true, "DCP carry when equal");

    m.ram.write(0x0041, 0x0f);
    m.code(0x1001, { 0xe7, 0x41 });          /* ISC $41 */
    m.cpu.a(0x20);
    m.cpu.p(kFlagC);
    m.step();
    US_CHECK_EQ_U(m.ram.read(0x0041), 0x10u, "ISC incremented memory");
    US_CHECK_EQ_U(m.cpu.a(), 0x10u, "ISC subtracted the result");
  }

  {
    /* ANC: AND immediate, then the carry copies the sign bit */
    TestMachine m;
    m.code(0x1000, { 0x0b, 0xff });
    m.cpu.a(0x80);
    m.cpu.p(0);
    US_CHECK_EQ_U(m.step(), 2u, "ANC cycles");
    US_CHECK_EQ_U(m.cpu.a(), 0x80u, "ANC result");
    US_CHECK(m.cpu.flag(kFlagC) == true, "ANC copies N into C");
  }

  {
    /* ALR: AND immediate then LSR A */
    TestMachine m;
    m.code(0x1000, { 0x4b, 0xff });
    m.cpu.a(0x03);
    m.cpu.p(0);
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), 0x01u, "ALR result");
    US_CHECK(m.cpu.flag(kFlagC) == true, "ALR carry from bit 0");
  }

  {
    /* ARR in binary mode: C is bit 6 of the result, V is bit 6 xor bit 5.
     * $c0 rotates to $60, where both bits are set, so V ends up clear. */
    TestMachine m;
    m.code(0x1000, { 0x6b, 0xff });
    m.cpu.a(0xc0);
    m.cpu.p(0);
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), 0x60u, "ARR result");
    US_CHECK(m.cpu.flag(kFlagC) == true, "ARR carry from bit 6");
    US_CHECK(m.cpu.flag(kFlagV) == false, "ARR overflow clear when bits 6 and 5 match");

    /* $80 rotates to $40, bit 6 set and bit 5 clear, so V is set */
    m.code(0x1001, { 0x6b, 0xff });
    m.cpu.a(0x80);
    m.cpu.p(0);
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), 0x40u, "ARR result, second case");
    US_CHECK(m.cpu.flag(kFlagV) == true, "ARR overflow set when bits 6 and 5 differ");
  }

  {
    /* SBX: X = (A AND X) - immediate, carry like a compare, no overflow */
    TestMachine m;
    m.code(0x1000, { 0xcb, 0x10 });
    m.cpu.a(0xff);
    m.cpu.x(0x30);
    m.cpu.p(0);
    m.step();
    US_CHECK_EQ_U(m.cpu.x(), 0x20u, "SBX result");
    US_CHECK(m.cpu.flag(kFlagC) == true, "SBX carry when no borrow");
  }

  {
    /* LAS ands memory with the stack pointer into A, X and SP */
    TestMachine m;
    m.ram.write(0x2000, 0x3c);
    m.code(0x1000, { 0xbb, 0x00, 0x20 });
    m.cpu.sp(0xf0);
    m.cpu.y(0x00);
    m.step();
    US_CHECK_EQ_U(m.cpu.a(), 0x30u, "LAS result in A");
    US_CHECK_EQ_U(m.cpu.x(), 0x30u, "LAS result in X");
    US_CHECK_EQ_U(m.cpu.sp(), 0x30u, "LAS result in SP");
  }

  {
    /* SHX stores X AND (high byte + 1) */
    TestMachine m;
    m.code(0x1000, { 0x9e, 0x00, 0x20 });    /* SHX $2000,Y */
    m.cpu.x(0xff);
    m.cpu.y(0x05);
    US_CHECK_EQ_U(m.step(), 5u, "SHX cycles");
    US_CHECK_EQ_U(m.ram.read(0x2005), 0x21u, "SHX value is X and (high + 1)");
  }

  {
    /* TAS also loads the stack pointer with A AND X */
    TestMachine m;
    m.code(0x1000, { 0x9b, 0x00, 0x20 });    /* TAS $2000,Y */
    m.cpu.a(0xff);
    m.cpu.x(0x0f);
    m.cpu.y(0x01);
    m.step();
    US_CHECK_EQ_U(m.cpu.sp(), 0x0fu, "TAS sets SP to A and X");
    US_CHECK_EQ_U(m.ram.read(0x2001), 0x01u, "TAS stored value");
  }

  {
    /* the undocumented NOPs still read, so they still cost their cycles */
    TestMachine m;
    m.code(0x1000, { 0x04, 0x40 });          /* NOP zp */
    US_CHECK_EQ_U(m.step(), 3u, "NOP zp cycles");
    m.code(0x1000, { 0x14, 0x40 });          /* NOP zp,X */
    US_CHECK_EQ_U(m.step(), 4u, "NOP zp,X cycles");
    m.code(0x1000, { 0x0c, 0x00, 0x20 });    /* NOP abs */
    US_CHECK_EQ_U(m.step(), 4u, "NOP abs cycles");
    m.code(0x1000, { 0x1c, 0x00, 0x20 });    /* NOP abs,X no page cross */
    m.cpu.x(0x01);
    US_CHECK_EQ_U(m.step(), 4u, "NOP abs,X cycles");
    m.code(0x1000, { 0x1c, 0xff, 0x20 });    /* NOP abs,X page cross */
    m.cpu.x(0x01);
    US_CHECK_EQ_U(m.step(), 5u, "NOP abs,X page cross cycles");
    m.code(0x1000, { 0x1a });                /* NOP implied */
    US_CHECK_EQ_U(m.step(), 2u, "NOP implied cycles");
    m.code(0x1000, { 0x80, 0x00 });          /* NOP immediate */
    US_CHECK_EQ_U(m.step(), 2u, "NOP immediate cycles");
  }

  {
    /* only the twelve KIL opcodes stop the CPU */
    TestMachine m;
    m.code(0x1000, { 0x02 });
    m.bus.run(4);
    US_CHECK(m.cpu.jammed() == true, "$02 jams the CPU");

    unsigned decoded = 0;
    for (unsigned op = 0; op < 256; op++) {
      if (Mos6510::decode(static_cast<data_t>(op)).kind != Kind::Kil) decoded++;
    }
    US_CHECK_EQ_U(decoded, 244u, "244 of 256 opcodes are implemented");
  }

  return 0;
}

/* ---- interrupt timing -------------------------------------------------- */

int test_interrupts(void)
{
  /* The three CLI/SEI/RTI cases the Acid800 cpu_clisei test checks. That test
   * itself needs a CIA to raise the interrupt, so it runs from step 2.5; here
   * the interrupt line is driven straight from the bus instead. */

  {
    /* CLI: one more instruction executes before the interrupt is taken */
    TestMachine m;
    m.ram.write(0xfffe, 0x00);
    m.ram.write(0xffff, 0x30);
    m.ram.write(0x3000, 0x4c);               /* JMP $3000, a trap */
    m.ram.write(0x3001, 0x00);
    m.ram.write(0x3002, 0x30);
    m.code(0x1000, { 0x78, 0x58, 0xe8, 0xe8, 0xe8 }); /* SEI CLI INX INX INX */
    m.cpu.x(0x00);
    m.cpu.sp(0xff);
    m.bus.set_irq(IrqSource::Cia1, true);

    US_CHECK(run_until_pc(m, 0x3000), "irq handler reached after CLI");
    US_CHECK_EQ_U(m.cpu.x(), 0x01u, "exactly one instruction ran after CLI");
  }

  {
    /* CLI immediately followed by SEI: the interrupt still happens, and the
     * flags pushed on the stack already have I set */
    TestMachine m;
    m.ram.write(0xfffe, 0x00);
    m.ram.write(0xffff, 0x30);
    m.ram.write(0x3000, 0x4c);
    m.ram.write(0x3001, 0x00);
    m.ram.write(0x3002, 0x30);
    m.code(0x1000, { 0x78, 0x58, 0x78, 0xe8 }); /* SEI CLI SEI INX */
    m.cpu.x(0x00);
    m.cpu.sp(0xff);
    m.bus.set_irq(IrqSource::Cia1, true);

    US_CHECK(run_until_pc(m, 0x3000), "irq taken inside a CLI/SEI pair");
    US_CHECK_EQ_U(m.cpu.x(), 0x00u, "no instruction ran after the SEI");
    const data_t pushed_p = m.ram.read(0x01fd);
    US_CHECK((pushed_p & kFlagI) != 0, "I is set in the flags pushed by the irq");
    US_CHECK((pushed_p & kFlagB) == 0, "B is clear for a hardware interrupt");
  }

  {
    /* RTI followed by SEI: the interrupt lands between the two, so the
     * flags pushed still have I clear */
    TestMachine m;
    m.ram.write(0xfffe, 0x00);
    m.ram.write(0xffff, 0x30);
    m.ram.write(0x3000, 0x4c);
    m.ram.write(0x3001, 0x00);
    m.ram.write(0x3002, 0x30);
    /* stack holds: P (I clear), then the return address $1010 */
    m.cpu.sp(0xfc);
    m.ram.write(0x01fd, kFlagU);             /* P with I clear */
    m.ram.write(0x01fe, 0x10);               /* pcl */
    m.ram.write(0x01ff, 0x10);               /* pch */
    m.code(0x1000, { 0x40 });                /* RTI */
    m.ram.write(0x1010, 0x78);               /* SEI */
    m.ram.write(0x1011, 0xe8);               /* INX */
    m.cpu.x(0x00);
    m.cpu.p(kFlagI);
    m.bus.set_irq(IrqSource::Cia1, true);

    US_CHECK(run_until_pc(m, 0x3000), "irq taken between RTI and SEI");
    const data_t pushed_p = m.ram.read(0x01fd);
    US_CHECK((pushed_p & kFlagI) == 0, "I is clear in the flags pushed after RTI");
  }

  {
    /* an interrupt is not taken at all while I is set. The program loops on
     * itself so it never falls into the $00 bytes, which are BRK and would
     * reach the vector without any interrupt being involved. */
    TestMachine m;
    m.ram.write(0xfffe, 0x00);
    m.ram.write(0xffff, 0x30);
    m.code(0x1000, { 0x78, 0xe8, 0x4c, 0x01, 0x10 }); /* SEI, INX, JMP $1001 */
    m.cpu.x(0x00);
    m.bus.set_irq(IrqSource::Cia1, true);
    m.bus.run(60);
    US_CHECK(m.cpu.pc() < 0x2000u, "irq stays masked while I is set");
    US_CHECK(m.cpu.x() > 5u, "the masked loop kept running");
  }

  {
    /* NMI is edge sensitive: holding the line low only fires once */
    TestMachine m;
    m.ram.write(0xfffa, 0x00);
    m.ram.write(0xfffb, 0x40);
    m.ram.write(0x4000, 0xe8);               /* INX */
    m.ram.write(0x4001, 0x40);               /* RTI */
    m.code(0x1000, { 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea });
    m.cpu.sp(0xff);
    m.cpu.x(0x00);
    m.bus.set_nmi(NmiSource::Cia2, true);    /* and never released */
    m.bus.run(80);
    US_CHECK_EQ_U(m.cpu.x(), 0x01u, "a held NMI line fires exactly once");
  }

  {
    /* an NMI during a BRK steals the vector, but the pushed flags still
     * have B set, which is the classic "BRK swallowed by NMI" case */
    TestMachine m;
    m.ram.write(0xfffa, 0x00);
    m.ram.write(0xfffb, 0x50);               /* nmi vector $5000 */
    m.ram.write(0xfffe, 0x00);
    m.ram.write(0xffff, 0x60);               /* irq vector $6000 */
    m.ram.write(0x5000, 0x4c);
    m.ram.write(0x5001, 0x00);
    m.ram.write(0x5002, 0x50);               /* trap at $5000 */
    m.code(0x1000, { 0x00, 0xaa });          /* BRK */
    m.cpu.sp(0xff);
    m.cpu.p(0);

    /* cycle 1 and 2 of the BRK, then assert NMI before the vector fetch */
    m.bus.tick();
    m.bus.tick();
    m.bus.set_nmi(NmiSource::Cia2, true);
    for (int i = 0; i < 5; i++) m.bus.tick();

    US_CHECK_EQ_U(m.cpu.pc(), 0x5000u, "NMI stole the BRK vector");
    US_CHECK((m.ram.read(0x01fd) & kFlagB) != 0,
             "the flags pushed by the hijacked BRK still have B set");
  }

  return 0;
}

/* ---- the Acid800 CPU tests -------------------------------------------- */

bool load_file(const char * path, std::vector<data_t> & out)
{
  FILE * f = fopen(path, "rb");
  if (f == nullptr) return false;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) { fclose(f); return false; }
  out.resize(static_cast<size_t>(size));
  const size_t got = fread(out.data(), 1, out.size(), f);
  fclose(f);
  return got == out.size();
}

/**
 * @brief Run one Acid800 CPU test.
 *
 * The harness in assets/tests/Acid800/common.s starts at the BASIC SYS entry
 * $080d, writes $00 to $d7ff on success and $ff on failure, and then loops on
 * itself. On failure the number of the failed check ends up in the screen
 * memory the harness uses, so it can be reported instead of only "failed".
 *
 * IO is plain RAM here: the machine is still a bare CPU. Only the tests that
 * do not need a real CIA or VIC are run at this step; cpu_clisei and cpu_bugs
 * need CIA timer interrupts (step 2.5) and cpu_timing needs the raster
 * counter (step 2.6).
 */
int run_acid800(const char * name)
{
  constexpr addr_t kEntry       = 0x080d; /* SYS 2061 */
  constexpr addr_t kResultAddr  = 0xd7ff;
  constexpr addr_t kErrorCount  = 0x0002;
  constexpr addr_t kScreenBase  = 0x0400 + (24 * 40);
  constexpr uint64_t kCycleLimit = 200ull * 1000ull * 1000ull;

  char path[512];
  snprintf(path, sizeof(path), "%s/tests/Acid800/%s.prg", US_ASSET_DIR, name);

  std::vector<data_t> prg;
  if (!load_file(path, prg) || prg.size() < 3) {
    ++us_test_failures;
    printf("  FAIL cannot read %s\n", path);
    return 1;
  }

  const addr_t load_addr = static_cast<addr_t>(prg[0] | (prg[1] << 8));

  Ram ram;
  FlatMemory mem(ram);
  Bus bus;
  Mos6510 cpu(mem, bus);
  bus.attach_cpu(&cpu);

  ram.fill(0x00);
  ram.load(load_addr, prg.data() + 2, prg.size() - 2);
  ram.write(kResultAddr, 0x55); /* neither pass nor fail */
  cpu.sp(0xfd);
  cpu.pc(kEntry);

  uint64_t cycles = 0;
  bool trapped = false;
  while (cycles < kCycleLimit) {
    bus.tick();
    ++cycles;
    if (!cpu.instruction_done()) continue;
    if (cpu.jammed()) break;
    if (cpu.pc() == cpu.instruction_pc()) { trapped = true; break; }
  }

  ++us_test_checks;

  if (cpu.jammed()) {
    ++us_test_failures;
    printf("  FAIL %s: cpu jammed on opcode $%02x at $%04x\n",
           name, cpu.opcode(), cpu.instruction_pc());
    return 1;
  }
  if (!trapped) {
    ++us_test_failures;
    printf("  FAIL %s: no trap within %llu cycles, pc $%04x\n",
           name, static_cast<unsigned long long>(kCycleLimit), cpu.pc());
    return 1;
  }

  const data_t result = ram.read(kResultAddr);
  if (result == 0x00) {
    printf("  %-12s passed (%llu cycles)\n",
           name, static_cast<unsigned long long>(cycles));
    return 0;
  }

  ++us_test_failures;
  printf("  FAIL %s: result $%02x, error count %u, check $%02x "
         "(x $%02x y $%02x), trapped at $%04x\n",
         name, result, ram.read(kErrorCount),
         ram.read(kScreenBase + 1), ram.read(kScreenBase + 2),
         ram.read(kScreenBase + 3), cpu.pc());
  return 1;
}

int test_acid800(void)
{
  /* Pure CPU tests, no chips needed */
  run_acid800("cpu_insn");
  run_acid800("cpu_illegal");
  run_acid800("cpu_flags");
  run_acid800("cpu_decimal");
  return 0;
}

int test_klaus_functional(void)
{
  /* Assembled with code_segment = $0400, zero_page = $0a, data_segment = $0200,
   * ROM_vectors = 1, load_data_direct = 0, I_flag = 3, disable_decimal = 0.
   * The image covers $0400..$388a and the success trap is a jmp to itself. */
  constexpr addr_t kLoadAddress   = 0x0400;
  constexpr addr_t kStartAddress  = 0x0400;
  constexpr addr_t kSuccessTrap   = 0x3463;
  constexpr uint64_t kCycleLimit  = 500ull * 1000ull * 1000ull;

  const char * path = US_ASSET_DIR "/tests/kdormann/6502_functional_test.bin";
  std::vector<data_t> image;
  if (!load_file(path, image)) {
    ++us_test_failures;
    printf("  FAIL cannot read %s\n", path);
    return 1;
  }
  US_CHECK_EQ_U(image.size(), 13451u, "functional test image size");

  Ram ram;
  FlatMemory mem(ram);
  Bus bus;
  Mos6510 cpu(mem, bus);
  bus.attach_cpu(&cpu);

  ram.fill(0x00);
  ram.load(kLoadAddress, image.data(), image.size());
  cpu.pc(kStartAddress);

  addr_t trap_pc = 0;
  bool trapped = false;
  uint64_t cycles = 0;

  while (cycles < kCycleLimit) {
    bus.tick();
    ++cycles;
    if (!cpu.instruction_done()) continue;
    if (cpu.jammed()) break;
    /* every trap in this test is a jmp to itself */
    if (cpu.pc() == cpu.instruction_pc()) {
      trap_pc = cpu.pc();
      trapped = true;
      break;
    }
  }

  if (cpu.jammed()) {
    ++us_test_failures;
    printf("  FAIL cpu jammed on opcode $%02x at $%04x (undocumented opcode, "
           "step 2.3)\n", cpu.opcode(), cpu.instruction_pc());
    return 1;
  }
  if (!trapped) {
    ++us_test_failures;
    printf("  FAIL no trap reached within %llu cycles, pc $%04x\n",
           static_cast<unsigned long long>(kCycleLimit), cpu.pc());
    return 1;
  }

  ++us_test_checks;
  if (trap_pc != kSuccessTrap) {
    ++us_test_failures;
    printf("  FAIL functional test trapped at $%04x, expected the success "
           "trap at $%04x (look the address up in "
           "assets/tests/6502_functional_test.lst)\n",
           trap_pc, kSuccessTrap);
    return 1;
  }

  printf("  functional test passed at $%04x after %llu cycles\n",
         trap_pc, static_cast<unsigned long long>(cycles));
  return 0;
}

} /* namespace */

int us_test_cpu(void)
{
  US_TEST_BEGIN("cpu/mos6510");

  test_addressing();
  test_rmw();
  test_branches();
  test_stack_and_jumps();
  test_arithmetic();
  test_illegal_opcodes();
  test_interrupts();
  test_ba_stall();
  test_klaus_functional();
  test_acid800();

  US_TEST_END("cpu/mos6510");
}

US_TEST_MAIN(us_test_cpu)
