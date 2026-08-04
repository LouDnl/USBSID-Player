/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * machine_harness.h
 * Test-only helpers: boot a machine, load a PRG, run it to its exit code.
 *
 * Both the Acid800 tests and the Lorenz test suite report the same way: $00
 * to $d7ff means passed, $ff means failed, and then the program stops. That
 * makes one runner enough for both.
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

#pragma once
#ifndef _US_TEST_MACHINE_HARNESS_H_
#define _US_TEST_MACHINE_HARNESS_H_

#include <cstdio>
#include <cstring>
#include <vector>

#include "machine.h"

namespace us_test {

using namespace usbsid;

inline bool load_binary(const char * path, std::vector<data_t> & out)
{
  FILE * f = fopen(path, "rb");
  if (f == nullptr) return false;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 2) { fclose(f); return false; }
  out.resize(static_cast<size_t>(size));
  const size_t got = fread(out.data(), 1, out.size(), f);
  fclose(f);
  return got == out.size();
}

/* PETSCII screen codes, for finding text in screen memory */
inline bool screen_contains(Ram & ram, const data_t * codes, size_t len)
{
  for (addr_t base = 0x0400; base + len < 0x07e8; base++) {
    size_t i = 0;
    while (i < len && ram.dma_read(static_cast<addr_t>(base + i)) == codes[i]) i++;
    if (i == len) return true;
  }
  return false;
}

/**
 * @brief A booted C64 that can run test PRGs.
 */
struct TestC64 {
  Machine machine;

  /* Both suites write their verdict to $d7ff, which is the last register of
   * the SID's mirror image, register $1f. With a SID fitted the write goes to
   * the chip and not to RAM, exactly as on hardware, so the verdict is read
   * back out of the SID's register file. */
  static constexpr addr_t kResultAddr = 0xd7ff;
  static constexpr data_t kResultReg = 0x1f;
  static constexpr data_t kSentinel = 0x55;

  explicit TestC64(VideoModel model = VideoModel::Pal6569)
  {
    machine.set_video_model(model);
    machine.power_on();
  }

  data_t result(void) { return machine.sid().peek(kResultReg); }

  /* Read what the program printed on the screen, as ASCII, for diagnostics */
  void screen_text(char * out, size_t out_len)
  {
    size_t o = 0;
    for (addr_t a = 0x0400; a < 0x07e8 && o + 2 < out_len; a++) {
      const data_t c = machine.ram().dma_read(a);
      char ch = ' ';
      if (c >= 0x01 && c <= 0x1a) ch = static_cast<char>('a' + c - 1);
      else if (c >= 0x30 && c <= 0x39) ch = static_cast<char>(c);
      else if (c == 0x20) ch = ' ';
      else if (c == 0x2d) ch = '-';
      else if (c == 0x2e) ch = '.';
      else if (c == 0x21) ch = '!';
      else ch = (c == 0) ? ' ' : '?';
      out[o++] = ch;
      if (((a - 0x0400) % 40) == 39) out[o++] = '\n';
    }
    out[o] = 0;
  }

  /* Run the KERNAL until BASIC prints its prompt, so the screen editor and
   * the KERNAL vectors are alive for anything the test program calls. */
  /* Finish whatever instruction is in flight, so the pc can be set safely */
  void sync_to_instruction(void)
  {
    while (!machine.cpu().instruction_done()) machine.tick();
  }

  bool boot(uint64_t cycle_limit = 20ull * 1000ull * 1000ull)
  {
    const data_t ready[] = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2e }; /* READY. */
    uint64_t cycles = 0;
    while (cycles < cycle_limit) {
      machine.run(10000);
      cycles += 10000;
      if (machine.cpu().jammed()) return false;
      if (screen_contains(machine.ram(), ready, sizeof(ready))) return true;
    }
    return false;
  }

  /**
   * @brief Load a PRG and find the address its BASIC stub SYSes to.
   */
  bool load_prg(const std::vector<data_t> & prg, addr_t & entry)
  {
    if (prg.size() < 3) return false;
    const addr_t load_addr = static_cast<addr_t>(prg[0] | (prg[1] << 8));
    machine.ram().load(load_addr, prg.data() + 2, prg.size() - 2);

    /* the stub is: link, line number, $9e ("SYS"), decimal digits, 0 */
    entry = 0;
    for (size_t i = 2; i < prg.size() && i < 32; i++) {
      if (prg[i] != 0x9e) continue;
      size_t j = i + 1;
      while (j < prg.size() && prg[j] == ' ') j++;
      unsigned value = 0;
      bool any = false;
      while (j < prg.size() && prg[j] >= '0' && prg[j] <= '9') {
        value = value * 10 + static_cast<unsigned>(prg[j] - '0');
        any = true;
        j++;
      }
      if (any) { entry = static_cast<addr_t>(value); return true; }
    }
    return false;
  }

  enum class RunResult { Passed, Failed, Trapped, Jammed, TimedOut, NoEntry };

  /**
   * @brief Load a PRG, jump to its entry point and wait for the exit code.
   *
   * $d7ff is the convention both suites use. The sentinel written before the
   * run makes "the program never reported anything" distinguishable from
   * "the program reported success".
   */
  RunResult run_prg(const char * path, uint64_t cycle_limit,
                    uint64_t & cycles_used, addr_t & stopped_at)
  {
    std::vector<data_t> prg;
    if (!load_binary(path, prg)) return RunResult::NoEntry;

    addr_t entry = 0;
    if (!load_prg(prg, entry)) return RunResult::NoEntry;

    machine.mmu().write(kResultAddr, kSentinel);
    /* Setting the pc part way through an instruction leaves the state machine
     * finishing the old instruction at the new address, which sends the CPU
     * somewhere random. Always land on an instruction boundary first. */
    while (!machine.cpu().instruction_done()) machine.tick();
    machine.cpu().pc(entry);

    /* A test program signals "finished" by looping on itself. Comparing the
     * pc with the address the instruction came from is not enough: taking an
     * interrupt also leaves those two equal for one boundary, so a program
     * that is merely busy handling NMIs looks stopped. Counting repeats of
     * the same pc tells the two apart. */
    addr_t last_pc = 0xffff;
    unsigned repeats = 0;
    bool in_instruction = false;

    cycles_used = 0;
    while (cycles_used < cycle_limit) {
      machine.tick();
      ++cycles_used;

      /* An instruction boundary is a transition, not a level. While the VIC
       * holds BA low the CPU sits at a boundary for up to forty cycles, and
       * treating each of those as a completed instruction makes a stalled
       * machine look like a program looping on itself. */
      if (!machine.cpu().instruction_done()) { in_instruction = true; continue; }
      if (!in_instruction) continue;
      in_instruction = false;
      if (machine.cpu().jammed()) {
        stopped_at = machine.cpu().instruction_pc();
        return RunResult::Jammed;
      }
      const data_t code = result();
      if (code != kSentinel) {
        stopped_at = machine.cpu().pc();
        return (code == 0x00) ? RunResult::Passed : RunResult::Failed;
      }
      if (machine.cpu().pc() == last_pc) {
        if (++repeats >= 4) {
          stopped_at = machine.cpu().pc();
          return RunResult::Trapped;
        }
      } else {
        last_pc = machine.cpu().pc();
        repeats = 0;
      }
    }
    stopped_at = machine.cpu().pc();
    return RunResult::TimedOut;
  }
};

inline const char * result_name(TestC64::RunResult r)
{
  switch (r) {
    case TestC64::RunResult::Passed:   return "passed";
    case TestC64::RunResult::Failed:   return "reported failure";
    case TestC64::RunResult::Trapped:  return "stopped without a verdict";
    case TestC64::RunResult::Jammed:   return "cpu jammed";
    case TestC64::RunResult::TimedOut: return "timed out";
    default:                           return "could not be loaded";
  }
}

} /* namespace us_test */

#endif /* _US_TEST_MACHINE_HARNESS_H_ */
