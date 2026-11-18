/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * test_vdrive.cpp
 * Virtual drive (src/cart/vdrive.cpp) through the real KERNAL entry points
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
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "machine_harness.h"
#include "test_common.h"
#include "tests.h"
#include "vdrive.h"

using namespace usbsid;

namespace {

constexpr addr_t kSentinel = 0xc000;  /* JMP $c000, where kcall() returns to */
constexpr addr_t kNameBuf  = 0x0340;
constexpr addr_t kSETLFS = 0xffba, kSETNAM = 0xffbd, kOPEN = 0xffc0, kCLOSE = 0xffc3, kCHKIN = 0xffc6,
                 kCLRCHN = 0xffcc, kCHRIN = 0xffcf, kCHROUT = 0xffd2, kLOAD = 0xffd5, kSAVE = 0xffd8;

const uint8_t kBank[] = { 0x00, 0x40, 0x11, 0x22, 0x33, 0x44, 0x55 };   /* loads at $4000 */
const uint8_t kIns[]  = { 0x00, 0x60, 0xa1, 0xa2 };
const VFile kFiles[] = {
  { "PRESETS1.SLO", kBank, sizeof(kBank) },
  { "LEAD.INS", kIns, sizeof(kIns) },
};

struct Regs { data_t a = 0, x = 0, y = 0; bool carry = false; };

/* JSR to a KERNAL entry from C++: return address is the sentinel loop */
bool kcall(us_test::TestC64 & c, addr_t entry, data_t a, data_t x, data_t y, Regs & out)
{
  Machine & m = c.machine;
  Mos6510 & cpu = m.cpu();
  c.sync_to_instruction();
  const addr_t ret = static_cast<addr_t>(kSentinel - 1);
  data_t sp = cpu.sp();
  m.ram().dma_write(static_cast<addr_t>(0x0100 + sp--), static_cast<data_t>(ret >> 8));
  m.ram().dma_write(static_cast<addr_t>(0x0100 + sp--), static_cast<data_t>(ret & 0xff));
  cpu.sp(sp);
  cpu.a(a); cpu.x(x); cpu.y(y);
  cpu.pc(entry);
  for (int i = 0; i < 2000000; i++) {
    m.tick();
    if (cpu.instruction_done() && cpu.pc() == kSentinel) {
      out = Regs{ cpu.a(), cpu.x(), cpu.y(), (cpu.p() & 0x01) != 0 };
      return true;
    }
  }
  return false;
}

void setnam(us_test::TestC64 & c, const char * name)
{
  const size_t n = strlen(name);
  for (size_t i = 0; i < n; i++) c.machine.ram().dma_write(static_cast<addr_t>(kNameBuf + i), static_cast<data_t>(name[i]));
  Regs r;
  kcall(c, kSETNAM, static_cast<data_t>(n), kNameBuf & 0xff, kNameBuf >> 8, r);
}

void setlfs(us_test::TestC64 & c, data_t la, data_t dev, data_t sa)
{
  Regs r;
  kcall(c, kSETLFS, la, dev, sa, r);
}

/* OPEN, CHKIN, CHRIN until READST, CLOSE, CLRCHN */
std::vector<uint8_t> read_file(us_test::TestC64 & c, const char * name, data_t la, data_t sa)
{
  std::vector<uint8_t> out;
  Regs r;
  setnam(c, name);
  setlfs(c, la, 8, sa);
  kcall(c, kOPEN, 0, 0, 0, r);
  kcall(c, kCHKIN, 0, la, 0, r);
  for (int i = 0; i < 4096; i++) {
    kcall(c, kCHRIN, 0, 0, 0, r);
    const data_t st = c.machine.ram().dma_read(0x90);
    if ((st & 0x02) != 0) break;   /* read past the end */
    out.push_back(r.a);
    if ((st & 0x40) != 0) break;   /* EOI on the last byte */
  }
  kcall(c, kCLOSE, la, 0, 0, r);
  kcall(c, kCLRCHN, 0, 0, 0, r);
  return out;
}

bool contains(const std::vector<uint8_t> & v, const char * s)
{
  const size_t n = strlen(s);
  for (size_t i = 0; i + n <= v.size(); i++) if (memcmp(&v[i], s, n) == 0) return true;
  return false;
}

void test_vdrive_kernal(void)
{
  us_test::TestC64 c;
  US_CHECK(c.boot(), "KERNAL boots to READY.");

  VirtualDrive drive(c.machine.ram(), nullptr, kFiles, 2, "TEST DISK", "VD");
  c.machine.mmu().attach_io1(&drive);
  drive.install();
  US_CHECK_EQ_U(c.machine.ram().dma_read(0x0330) | (c.machine.ram().dma_read(0x0331) << 8), 0xde80 + 3 * 9, "LOAD vector");
  c.machine.ram().dma_write(kSentinel, 0x4c);
  c.machine.ram().dma_write(kSentinel + 1, kSentinel & 0xff);
  c.machine.ram().dma_write(kSentinel + 2, kSentinel >> 8);

  Regs r;

  /* Directory as the SID SYNTH browser reads it: OPEN 2,8,0,"$" + CHRIN */
  const std::vector<uint8_t> dir = read_file(c, "$", 2, 0);
  US_CHECK(dir.size() > 8 && dir[0] == 0x01 && dir[1] == 0x04, "directory starts with load address $0401");
  US_CHECK(contains(dir, "\"TEST DISK       \" VD 2A"), "directory header");
  US_CHECK(contains(dir, "\"PRESETS1.SLO\""), "directory lists PRESETS1.SLO");
  US_CHECK(contains(dir, "\"LEAD.INS\""), "directory lists LEAD.INS");
  US_CHECK(contains(dir, "BLOCKS FREE."), "directory footer");
  US_CHECK(dir.size() >= 3 && dir[dir.size() - 1] == 0 && dir[dir.size() - 2] == 0 && dir[dir.size() - 3] == 0,
           "directory ends with the line end and the 00 00 link");
  /* 1541 patterns: * ends the compare, so "*.INS" would list everything */
  const std::vector<uint8_t> filtered = read_file(c, "$:L*", 2, 0);
  US_CHECK(contains(filtered, "LEAD.INS") && !contains(filtered, "PRESETS1"), "directory pattern $:L*");

  /* LOAD "PRESETS1.SLO",8 to $5000 (secondary address 0 relocates) */
  setnam(c, "PRESETS1.SLO");
  setlfs(c, 1, 8, 0);
  US_CHECK(kcall(c, kLOAD, 0, 0x00, 0x50, r), "LOAD returns");
  US_CHECK(!r.carry, "LOAD carry clear");
  US_CHECK_EQ_U(r.x | (r.y << 8), 0x5000 + sizeof(kBank) - 2, "LOAD end address");
  bool same = true;
  for (size_t i = 2; i < sizeof(kBank); i++) same = same && c.machine.ram().dma_read(static_cast<addr_t>(0x5000 + i - 2)) == kBank[i];
  US_CHECK(same, "LOAD data at $5000");

  /* LOAD "PRE*",8,1 uses the file's own address */
  setnam(c, "0:PRE*");
  setlfs(c, 1, 8, 1);
  kcall(c, kLOAD, 0, 0, 0, r);
  US_CHECK(!r.carry && c.machine.ram().dma_read(0x4000) == 0x11 && c.machine.ram().dma_read(0x4004) == 0x55,
           "LOAD with a pattern and secondary address 1");

  /* Missing file */
  setnam(c, "NOPE.SLO");
  setlfs(c, 1, 8, 0);
  kcall(c, kLOAD, 0, 0x00, 0x50, r);
  US_CHECK(r.carry && r.a == 4, "LOAD of a missing file: carry, A=4");
  const std::vector<uint8_t> st_nf = read_file(c, "", 15, 15);
  US_CHECK(contains(st_nf, "62,FILE NOT FOUND,00,00"), "status after a missing file: %.*s",
           static_cast<int>(st_nf.size()), reinterpret_cast<const char *>(st_nf.data()));

  /* Disk management command on the command channel, as SID SYNTH sends it */
  const std::vector<uint8_t> st = read_file(c, "S0:LEAD.INS", 15, 15);
  US_CHECK(contains(st, "26,WRITE PROTECT ON,00,00\r"), "scratch refused: 26, WRITE PROTECT ON");
  const std::vector<uint8_t> st_ok = read_file(c, "I0", 15, 15);
  US_CHECK(contains(st_ok, "00, OK,00,00\r"), "initialize: 00, OK");

  /* SAVE is refused */
  setnam(c, "NEW.SLO");
  setlfs(c, 1, 8, 0);
  c.machine.ram().dma_write(0xfb, 0x00);
  c.machine.ram().dma_write(0xfc, 0x40);
  kcall(c, kSAVE, 0xfb, 0x10, 0x40, r);
  US_CHECK(r.carry, "SAVE carry set");

  /* Other devices fall through to the KERNAL: CHROUT to the screen moves the cursor */
  const data_t col = c.machine.ram().dma_read(0xd3);
  kcall(c, kCHROUT, 'A', 0, 0, r);
  US_CHECK_EQ_U(c.machine.ram().dma_read(0xd3), col + 1u, "CHROUT to the screen passes through");
  setnam(c, "X");
  setlfs(c, 1, 9, 0);
  kcall(c, kLOAD, 0, 0x00, 0x50, r);
  US_CHECK(r.carry && r.a == 5, "LOAD from device 9 reaches the KERNAL: DEVICE NOT PRESENT");
}

} /* namespace */

int us_test_vdrive(void)
{
  US_NEEDS_STOCK_ROMS("cart/vdrive");
  US_TEST_BEGIN("cart/vdrive");

  test_vdrive_kernal();

  US_TEST_END("cart/vdrive");
}

US_TEST_MAIN(us_test_vdrive)
