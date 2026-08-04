/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * usbsid_test.cpp
 * Aggregate test runner. Same test functions as the standalone executables,
 * reachable as subcommands:
 *
 *   usbsid-test            run everything
 *   usbsid-test bus        run one component
 *   usbsid-test list       list the subcommands
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

#include "tests.h"

namespace {

struct TestEntry {
  const char * name;
  int (*fn)(void);
};

const TestEntry kTests[] = {
  { "bus", us_test_bus },
  { "ram", us_test_ram },
  { "cpu", us_test_cpu },
  { "mmu", us_test_mmu },
  { "cia", us_test_cia },
  { "vic", us_test_vic },
  { "sid", us_test_sid },
  { "player", us_test_player },
  { "embedded", us_test_embedded },
  { "prg", us_test_prg },
  { "keyboard", us_test_keyboard },
  { "web", us_test_web },
};

constexpr size_t kTestCount = sizeof(kTests) / sizeof(kTests[0]);

void usage(const char * argv0)
{
  printf("usage: %s [subcommand]\n", argv0);
  printf("subcommands:\n");
  printf("  all           run every test (default)\n");
  printf("  list          list the available tests\n");
  for (size_t i = 0; i < kTestCount; i++) {
    printf("  %-13s run the %s test\n", kTests[i].name, kTests[i].name);
  }
}

} /* namespace */

int main(int argc, char ** argv)
{
  const char * what = (argc > 1) ? argv[1] : "all";

  if (strcmp(what, "-h") == 0 || strcmp(what, "--help") == 0) {
    usage(argv[0]);
    return 0;
  }

  if (strcmp(what, "list") == 0) {
    for (size_t i = 0; i < kTestCount; i++) printf("%s\n", kTests[i].name);
    return 0;
  }

  int failures = 0;
  int ran = 0;

  for (size_t i = 0; i < kTestCount; i++) {
    if (strcmp(what, "all") == 0 || strcmp(what, kTests[i].name) == 0) {
      failures += kTests[i].fn();
      ++ran;
    }
  }

  if (ran == 0) {
    printf("unknown test \"%s\"\n", what);
    usage(argv[0]);
    return 2;
  }

  printf("\n%s: %d test%s run, %d failed check%s\n",
         (failures == 0 ? "PASS" : "FAIL"),
         ran, (ran == 1 ? "" : "s"),
         failures, (failures == 1 ? "" : "s"));

  return (failures == 0) ? 0 : 1;
}
