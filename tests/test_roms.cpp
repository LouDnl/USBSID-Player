/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_roms.cpp
 * The stub KERNAL and playing without the Commodore ROMs.
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

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "machine_harness.h"
#include "player.h"
#include "prgfile.h"
#include "rom.h"
#include "sid_backend.h"
#include "test_common.h"
#include "tests.h"

#ifndef US_TUNE_DIR
#define US_TUNE_DIR "/mnt/loud/DocThierry/retro/Commodore64/sidtunes/favorites"
#endif

using namespace usbsid;
using namespace us_test;

namespace {

/**
 * @brief Counts writes and hashes the write stream (FNV-1a).
 *
 * The delay before the first write is left out: it depends on which
 * instruction the boot was in when the tune was entered, a start offset of
 * a cycle or two, not a difference in the tune.
 */
class HashSidBackend final : public SidBackend
{
  public:
    void write(addr_t reg, data_t value, uint16_t cycles) override
    {
      mix(static_cast<uint32_t>(reg));
      mix(value);
      mix(writes == 0 ? 0 : cycles);
      ++writes;
    }
    void wait(uint16_t cycles) override { mix(0x10000u | cycles); }
    void reset(void) override { writes = 0; hash = 2166136261u; }

    uint32_t writes = 0;
    uint32_t hash = 2166136261u;

  private:
    void mix(uint32_t v)
    {
      for (int i = 0; i < 4; i++) {
        hash ^= (v >> (i * 8)) & 0xff;
        hash *= 16777619u;
      }
    }
};

/**
 * @brief Read a whole file.
 *
 * @return false when it cannot be read or is empty
 */
bool read_file(const char * path, std::vector<data_t> & out)
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

/* ---- the stub image ------------------------------------------------------ */

int test_stub_layout(void)
{
  const data_t * stub = stub_kernal();
  auto at = [stub](addr_t addr) { return stub[addr - 0xe000]; };

  US_CHECK(at(0xfffa) == 0x43 && at(0xfffb) == 0xfe, "NMI vector $fe43");
  US_CHECK(at(0xfffc) == 0xe2 && at(0xfffd) == 0xfc, "reset vector $fce2");
  US_CHECK(at(0xfffe) == 0x48 && at(0xffff) == 0xff, "IRQ vector $ff48");
  US_CHECK(at(0xffd2) == 0x60, "CHROUT slot is an RTS");
  US_CHECK(at(0xea87) == 0x60, "SCNKEY routine is an RTS");
  US_CHECK(at(kStubIdleLoop) == 0x4c &&
           at(kStubIdleLoop + 1) == (kStubIdleLoop & 0xff) &&
           at(kStubIdleLoop + 2) == (kStubIdleLoop >> 8), "idle loop jumps to itself");

#if US_EMBED_ROMS
  /* Where the stub mirrors the stock KERNAL, it has to be byte identical:
   * that is what keeps the cycle counts of reset and interrupt entry the same. */
  struct Range { addr_t first; addr_t last; const char * what; };
  static const Range kSame[] = {
    { 0xfce2, 0xfcf4, "reset up to IOINIT" },
    { 0xfda3, 0xfdf8, "IOINIT" },
    { 0xff6e, 0xff7f, "IOINIT timer start" },
    { 0xee8e, 0xee96, "IOINIT serial clock" },
    { 0xfd02, 0xfd14, "cartridge check and CBM80" },
    { 0xff48, 0xff5a, "IRQ/BRK entry" },
    { 0xea7e, 0xea86, "IRQ exit" },
    { 0xfe43, 0xfe46, "NMI entry" },
    { 0xfebc, 0xfec1, "NMI exit" },
    { 0xfffa, 0xffff, "hardware vectors" },
  };
  for (const Range & r : kSame) {
    bool same = true;
    for (uint32_t a = r.first; a <= r.last; a++) {
      if (at(static_cast<addr_t>(a)) != kRomKernal[a - 0xe000]) same = false;
    }
    US_CHECK(same, "stub matches the stock KERNAL: %s", r.what);
  }
#endif
  return 0;
}

int test_rom_store(void)
{
  std::unique_ptr<RomStore> store(new RomStore());
  store->set(nullptr, nullptr, nullptr);
  Roms none = store->roms();
  US_CHECK(none.basic == nullptr && none.chargen == nullptr, "nothing stored");
  US_CHECK(!none.real_kernal && none.kernal == stub_kernal(), "stub KERNAL");
  US_CHECK(!none.complete(), "not a complete set");

  std::vector<data_t> basic(kRomSizeBasic, 0x11), kernal(kRomSizeKernal, 0x22);
  store->set(basic.data(), kernal.data(), nullptr);
  basic[0] = 0x99;  /* the store keeps its own copy */
  Roms some = store->roms();
  US_CHECK(some.complete(), "BASIC and KERNAL make a complete set");
  US_CHECK(some.basic != nullptr && some.basic[0] == 0x11, "BASIC copied");
  US_CHECK(some.kernal[0] == 0x22 && some.real_kernal, "KERNAL copied");
  US_CHECK(some.chargen == nullptr, "no character ROM");

  /* An absent BASIC reads the RAM underneath */
  TestC64 c64;
  c64.machine.mmu().roms = none;
  c64.machine.ram().dma_write(0xa123, 0x5a);
  c64.machine.mmu().write(0x0001, 0x37);
  US_CHECK(c64.machine.mmu().read(0xa123) == 0x5a, "no BASIC: $a000 reads RAM");
  US_CHECK(c64.machine.mmu().read(0xfffc) == 0xe2, "stub KERNAL mapped at $e000");
  return 0;
}

/* ---- tunes without ROMs -------------------------------------------------- */

struct RunResult { bool started; uint32_t writes; uint32_t hash; };

/**
 * @brief Run a tune for 300 frames.
 *
 * @param bytes  the file
 * @param roms   ROM set, nullptr for the build default
 */
RunResult run_tune(const std::vector<data_t> & bytes, const Roms * roms)
{
  std::unique_ptr<TestC64> c64(new TestC64());
  if (roms != nullptr) c64->machine.mmu().roms = *roms;
  HashSidBackend backend;
  c64->machine.set_sid_backend(backend);
  Player player(c64->machine);
  RunResult r = { false, 0, 0 };
  if (!player.load_sid(bytes.data(), bytes.size())) return r;
  r.started = player.is_prg() ? player.init_prg() : player.init_tune(0);
  if (!r.started) return r;
  backend.reset();
  player.run_frames(300);
  r.writes = backend.writes;
  r.hash = backend.hash;
  return r;
}

int test_tune_sweep_without_roms(void)
{
  static const char * const dirs[] = { US_TUNE_DIR "/psid", US_TUNE_DIR "/rsid" };
  std::vector<std::string> files;
  unsigned present = 0;
  for (const char * dir : dirs) {
    if (us_list_dir(dir, ".sid", files) != UsDir::Missing) ++present;
  }
  if (present == 0) {
    ++us_test_checks;
    printf("  skipped the sweep, no tune directories on this machine\n");
    return 0;
  }

  std::unique_ptr<RomStore> store(new RomStore());
  store->set(nullptr, nullptr, nullptr);
  const Roms stub = store->roms();

  unsigned total = 0, played = 0, silent = 0, needs = 0, same = 0, differ = 0;
  for (const std::string & file : files) {
    std::vector<data_t> bytes;
    if (!read_file(file.c_str(), bytes)) continue;
    ++total;

    const RunResult without = run_tune(bytes, &stub);
    if (!without.started) { ++needs; printf("    needs ROMs: %s\n", file.c_str()); continue; }
    if (without.writes > 50) ++played;
    else { ++silent; printf("    silent without ROMs: %s\n", file.c_str()); }

#if US_EMBED_ROMS
    const RunResult with = run_tune(bytes, nullptr);
    if (with.started && with.hash == without.hash) ++same;
    else { ++differ; printf("    differs without ROMs: %s\n", file.c_str()); }
#endif
  }

  printf("  without ROMs: %u tunes, %u played, %u silent, %u need ROMs\n",
         total, played, silent, needs);
#if US_EMBED_ROMS
  printf("  write streams identical to the stock ROMs: %u, different: %u\n", same, differ);
#else
  (void)same; (void)differ;
#endif
  US_CHECK(total > 0, "the sweep found tunes to run");
  US_CHECK_EQ_U(silent, 0u, "every tune that starts without ROMs plays");
  return 0;
}

int test_prg_without_roms(void)
{
  std::unique_ptr<RomStore> store(new RomStore());
  store->set(nullptr, nullptr, nullptr);

  /* A BASIC line that SYSes into machine code: 10 SYS2061, then a loop that
   * writes the SID volume register forever. */
  const data_t prg[] = {
    0x01, 0x08,                                     /* load address $0801 */
    0x0b, 0x08, 0x0a, 0x00, 0x9e, 0x32, 0x30, 0x36, 0x31, 0x00, 0x00, 0x00,
    0xa9, 0x0f, 0x8d, 0x18, 0xd4, 0x4c, 0x0d, 0x08, /* $080d lda #15, sta $d418, jmp */
  };
  {
    TestC64 c64;
    c64.machine.mmu().roms = store->roms();
    HashSidBackend backend;
    c64.machine.set_sid_backend(backend);
    Player player(c64.machine);
    US_CHECK(player.load_prg(prg, sizeof(prg)), "the program parses");
    US_CHECK(!player.needs_roms(), "a SYS line needs no ROMs");
    US_CHECK(player.init_prg(), "and starts on the stub KERNAL");
    backend.reset();
    player.run_frames(5);
    US_CHECK(backend.writes > 100, "it runs: %u SID writes", backend.writes);
  }

  /* A BASIC program with no SYS line cannot start without the interpreter */
  const data_t basic[] = {
    0x01, 0x08, 0x09, 0x08, 0x0a, 0x00, 0x99, 0x22, 0x41, 0x22, 0x00, 0x00, 0x00,
  };
  {
    TestC64 c64;
    c64.machine.mmu().roms = store->roms();
    NullSidBackend backend;
    c64.machine.set_sid_backend(backend);
    Player player(c64.machine);
    US_CHECK(player.load_prg(basic, sizeof(basic)), "the BASIC program parses");
    US_CHECK(player.needs_roms(), "and needs ROMs");
    US_CHECK(!player.init_prg(), "so it does not start without them");
  }
  return 0;
}

} /* namespace */

int us_test_roms(void)
{
  US_TEST_BEGIN("roms");

  test_stub_layout();
  test_rom_store();
  test_prg_without_roms();
  test_tune_sweep_without_roms();

  US_TEST_END("roms");
}

US_TEST_MAIN(us_test_roms)
