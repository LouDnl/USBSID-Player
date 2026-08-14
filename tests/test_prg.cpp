/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_prg.cpp
 * Step 2.10 gate: programs load and run the way they would on a real machine.
 *
 * The Acid800 CPU tests are already run by test_cpu.cpp, but by poking them
 * into memory and jumping to their entry point. Here the same programs go in
 * through the loader: booted machine, file at the address its first two bytes
 * name, BASIC told how far it reaches, RUN typed at the prompt. If the answers
 * still come out right, everything between the keyboard buffer and the SYS is
 * working, which is a good deal of KERNAL and BASIC.
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
#include <cstring>
#include <vector>

#include "machine_harness.h"
#include "player.h"
#include "prgfile.h"
#include "test_common.h"
#include "tests.h"

#ifndef US_TUNE_DIR
#define US_TUNE_DIR "/mnt/loud/DocThierry/retro/Commodore64/sidtunes/favorites"
#endif

using namespace usbsid;
using namespace us_test;

namespace {

/* ---- the file formats ---------------------------------------------------- */

int test_parse(void)
{
  /* a plain PRG: load address, then bytes */
  {
    const data_t prg[] = { 0x00, 0xc0, 0xa9, 0x00, 0x60 };
    PrgFile f;
    US_CHECK(prgfile_parse(prg, sizeof(prg), f), "a PRG parses");
    US_CHECK_EQ_U(f.load_addr, 0xc000u, "the load address is the first word");
    US_CHECK_EQ_U(f.data_size, 3u, "and the rest is the program");
    US_CHECK_EQ_U(f.end_addr, 0xc002u, "the end address follows from it");
    US_CHECK(f.is_basic() == false, "$c000 is not where BASIC lives");
    US_CHECK(f.has_sys_stub == false, "and there is no SYS line");
  }

  /* one with a BASIC stub: 10 SYS2061 */
  {
    const data_t prg[] = {
      0x01, 0x08,                          /* load at $0801 */
      0x0b, 0x08,                          /* link to the next line */
      0x0a, 0x00,                          /* line 10 */
      0x9e, '2', '0', '6', '1',            /* SYS 2061 */
      0x00,                                /* end of line */
      0x00, 0x00,                          /* end of program */
      0xa9, 0x00, 0x60                     /* the code at $080d */
    };
    PrgFile f;
    US_CHECK(prgfile_parse(prg, sizeof(prg), f), "a BASIC stub PRG parses");
    US_CHECK(f.is_basic(), "it loads where BASIC programs go");
    US_CHECK(f.has_sys_stub, "the SYS line is found");
    US_CHECK_EQ_U(f.sys_addr, 2061u, "and it says $080d");
  }

  /* the same thing inside a PC64 container */
  {
    std::vector<data_t> p00(26, 0);
    memcpy(p00.data(), "C64File", 7);
    const char * name = "TESTPROG";
    memcpy(&p00[8], name, strlen(name));
    const data_t body[] = { 0x00, 0xc0, 0xa2, 0x00, 0x60 };
    p00.insert(p00.end(), body, body + sizeof(body));

    PrgFile f;
    US_CHECK(prgfile_parse(p00.data(), p00.size(), f), "a P00 parses");
    US_CHECK(f.is_p00, "and is recognised as one");
    US_CHECK_EQ_STR(f.name, "TESTPROG", "the container carries the C64 name");
    US_CHECK_EQ_U(f.load_addr, 0xc000u, "the load address is inside it");
    US_CHECK_EQ_U(f.data_size, 3u, "and so is the program");
  }

  /* rubbish is refused */
  {
    PrgFile f;
    const data_t two[] = { 0x01, 0x08 };
    US_CHECK(prgfile_parse(two, sizeof(two), f) == false,
             "a file with nothing after the load address is rejected");
    US_CHECK(prgfile_parse(nullptr, 100, f) == false, "and so is nothing at all");

    /* a program that would not fit in memory is not one */
    std::vector<data_t> huge(0x2000, 0x00);
    huge[0] = 0x00; huge[1] = 0xff;      /* load at $ff00, 8 KB of it */
    US_CHECK(prgfile_parse(huge.data(), huge.size(), f) == false,
             "a program that runs off the top of memory is rejected");
  }

  return 0;
}

/* ---- the Acid800 tests, through the loader ------------------------------- */

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

/**
 * @brief Load one Acid800 test through the player and read its verdict.
 *
 * The programs write $00 to $d7ff when they pass and $ff when they fail, and
 * set the border colour to 5 or to 10 with it, then sit in a loop.
 *
 * The border is what is checked, not a sentinel written beforehand. Starting a
 * program now means holding keys down and waiting for the KERNAL to scan them,
 * which takes long enough that these tests are already finished by the time
 * `init_prg()` returns: a sentinel written after it would simply overwrite the
 * answer. The border says the same thing and cannot be mistaken for the state
 * a machine comes up in, which is light blue.
 */
int run_through_loader(const char * name, uint64_t cycle_limit)
{
  constexpr data_t kBorderPassed = 5;
  constexpr data_t kBorderFailed = 10;
  constexpr uint8_t kRegBorder = 0x20;

  char path[512];
  snprintf(path, sizeof(path), "%s/tests/Acid800/%s.prg", US_ASSET_DIR, name);

  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("  skipped %s, not readable\n", name);
    return 0;
  }

  TestC64 c64;
  NullSidBackend backend;
  c64.machine.set_sid_backend(backend);

  Player player(c64.machine);
  ++us_test_checks;
  if (!player.load_prg(bytes.data(), bytes.size())) {
    ++us_test_failures;
    printf("  FAIL %s does not parse\n", name);
    return 1;
  }

  const PrgFile & prg = player.prg();

  ++us_test_checks;
  if (!player.init_prg()) {
    ++us_test_failures;
    printf("  FAIL %s does not start\n", name);
    return 1;
  }

  uint64_t cycles = 0;
  data_t border = c64.machine.vic().peek(kRegBorder);
  while (cycles < cycle_limit &&
         border != kBorderPassed && border != kBorderFailed) {
    c64.machine.tick();
    ++cycles;
    if ((cycles & 0x3ff) == 0) border = c64.machine.vic().peek(kRegBorder);
  }
  border = c64.machine.vic().peek(kRegBorder);

  const addr_t stopped_at = c64.machine.cpu().pc();
  const data_t code = c64.result();

  ++us_test_checks;
  if (border != kBorderPassed || code != 0x00) {
    ++us_test_failures;
    printf("  FAIL %s through the loader: border %u, $d7ff $%02x, "
           "pc $%04x after %llu cycles\n",
           name, border, code, stopped_at,
           static_cast<unsigned long long>(cycles));
    char screen[1200];
    c64.screen_text(screen, sizeof(screen));
    printf("%s\n", screen);
    return 1;
  }

  /* It has to have ended up inside the program it loaded. Anywhere else and
   * the right answer arrived for the wrong reason. */
  US_CHECK(stopped_at >= prg.load_addr && stopped_at <= prg.end_addr,
           "%s finished inside its own code, at $%04x", name, stopped_at);

  printf("  %-22s loaded $%04x-$%04x, SYS %u, passed at $%04x\n",
         name, prg.load_addr, prg.end_addr, prg.sys_addr, stopped_at);
  return 0;
}

int test_acid800_through_loader(void)
{
  /* The quick ones. cpu_insn and cpu_illegal are minutes of emulated time and
   * are already run instruction by instruction in test_cpu.cpp; what is being
   * tested here is the path into them, not the CPU. */
  run_through_loader("cpu_flags", 60ull * 1000ull * 1000ull);
  run_through_loader("cpu_decimal", 200ull * 1000ull * 1000ull);
  run_through_loader("cpu_clisei", 60ull * 1000ull * 1000ull);
  run_through_loader("cpu_bugs", 60ull * 1000ull * 1000ull);

  return 0;
}

/* ---- what the loader leaves behind --------------------------------------- */

int test_loader_state(void)
{
  /* A three byte machine code program at $c000: load address, then an rts.
   * Deliberately not one of the Acid800 tests, because those clear the whole
   * zero page as they start and the pointers being checked here live in it. */
  const data_t prg[] = { 0x00, 0xc0, 0x60 };

  TestC64 c64;
  NullSidBackend backend;
  c64.machine.set_sid_backend(backend);
  Player player(c64.machine);

  US_CHECK(player.load_prg(prg, sizeof(prg)), "the program loads");
  US_CHECK(player.is_prg(), "and the player knows it is a program");
  US_CHECK(player.init_prg(), "and it starts");
  US_CHECK(player.playing(), "and reports that it is running");

  Ram & ram = c64.machine.ram();
  const PrgFile & loaded = player.prg();

  US_CHECK_EQ_U(loaded.load_addr, 0xc000u, "it loads outside BASIC");
  US_CHECK_EQ_U(ram.dma_read(0xc000), 0x60u, "and its byte is where it belongs");

  /* BASIC was told how far it reaches */
  const addr_t vartab = static_cast<addr_t>(
    ram.dma_read(0x2d) | (ram.dma_read(0x2e) << 8));
  US_CHECK_EQ_U(vartab, static_cast<unsigned>(loaded.end_addr + 1),
                "variables start where the program ends");

  /* It was started with SYS, which the editor echoed before running it */
  char screen[1400];
  c64.screen_text(screen, sizeof(screen));
  US_CHECK(strstr(screen, "sys49152") != nullptr,
           "SYS 49152 was typed at the prompt");

  /* and a program is not a tune */
  US_CHECK(player.init_tune(1) == false, "a program cannot be started as a tune");

  return 0;
}

/* ---- the whole collection ------------------------------------------------ */

int test_prg_sweep(void)
{
  /* The same regression net the tune sweep is: load every program there is,
   * start it the way a person would, and count the ones that end up making a
   * sound. A program that stops writing registers has hit a bug somewhere
   * between the keyboard buffer and the SID. */
  unsigned total = 0, parsed = 0, played = 0, silent = 0;
  std::vector<std::string> files;
  const UsDir listing = us_list_dir(US_TUNE_DIR "/prg", ".prg", files);

  for (const std::string & file : files) {
    const char * path = file.c_str();

    std::vector<data_t> bytes;
    if (!read_file(path, bytes)) continue;
    ++total;

    TestC64 c64;
    NullSidBackend backend;
    c64.machine.set_sid_backend(backend);
    Player player(c64.machine);

    /* A file that is not a program at all is not a failure of the loader. The
     * collection has an assembler source with a .prg extension in it, and
     * refusing that is the parser working. */
    if (!player.load_prg(bytes.data(), bytes.size())) {
      printf("    not a program: %s\n", path);
      continue;
    }
    ++parsed;
    if (!player.init_prg()) { ++silent; printf("    will not start: %s\n", path); continue; }

    backend.reset();
    player.run_frames(300);

    if (backend.writes > 50) ++played;
    else { ++silent; printf("    silent: %s\n", path); }
  }

  printf("  %u programs, %u are programs, %u made sound, %u silent\n",
         total, parsed, played, silent);

  /* Missing is a machine without the collection and is a legitimate skip.
   * Empty is a directory that is there and gave nothing, which used to be
   * reported as a skip and passed: see us_list_dir(). */
  if (listing == UsDir::Missing) {
    ++us_test_checks;
    printf("  skipped the sweep, no program directory on this machine\n");
    return 0;
  }
  US_CHECK(listing == UsDir::Listed, "the program directory has programs in it");
  US_CHECK(total > 0, "the sweep found programs to run");
  if (total == 0) return us_test_failures;
  US_CHECK_EQ_U(played, parsed, "every program that parses starts and plays");

  return 0;
}

} /* namespace */

int us_test_prg(void)
{
  US_TEST_BEGIN("prg");

  test_parse();
  test_loader_state();
  test_acid800_through_loader();
  test_prg_sweep();

  US_TEST_END("prg");
}

US_TEST_MAIN(us_test_prg)
