/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_cia.cpp
 * Step 2.5 gate: the delay pipeline behaviours the model exists for, the two
 * Acid800 tests that were deferred from step 2.3 because they need real CIA
 * interrupts, and the CIA timing tests from the Lorenz suite.
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

#include "core/bus.h"
#include "machine_harness.h"
#include "mos6526.h"
#include "player.h"
#include "sid_backend.h"
#include "sid_trace.h"
#include "test_common.h"
#include "tests.h"

#ifndef US_ASSET_DIR
#define US_ASSET_DIR "assets"
#endif

using namespace usbsid;
using namespace us_test;

namespace {

/* A CIA on a bus with nothing else on it */
struct CiaRig {
  Bus bus;
  Mos6526 cia{bus, CiaLine::Irq};

  void run(unsigned cycles) { for (unsigned i = 0; i < cycles; i++) cia.tick(); }
  void write(uint8_t reg, data_t v) { cia.io_write(reg, v); }
  data_t read(uint8_t reg) { return cia.io_read(reg); }
};

/* ---- timers ------------------------------------------------------------ */

int test_timer_basics(void)
{
  /* The latencies below are the timer state machine's, and they are what the
   * Lorenz timer tests validate. */
  {
    /* a stopped timer takes the new latch immediately when the high byte is
     * written, no delay stage involved */
    CiaRig r;
    r.write(0x04, 0x34);
    r.write(0x05, 0x12);
    US_CHECK_EQ_U(r.cia.latch_a(), 0x1234u, "latch A takes both bytes");
    US_CHECK_EQ_U(r.cia.counter_a(), 0x1234u, "a stopped timer loads at once");
    r.run(2);
    US_CHECK_EQ_U(r.cia.counter_a(), 0x1234u, "and a stopped timer stays put");
  }

  {
    /* starting the timer counts one per phi2 once the pipeline has filled */
    CiaRig r;
    r.write(0x04, 0x10);
    r.write(0x05, 0x00);
    r.run(2);                     /* let the load land */
    r.write(0x0e, 0x01);          /* start, continuous */
    r.run(2);
    US_CHECK_EQ_U(r.cia.counter_a(), 0x10u, "no count while the start travels");
    r.run(1);
    US_CHECK_EQ_U(r.cia.counter_a(), 0x0fu, "the first count lands on clock 3");
    r.run(4);
    US_CHECK_EQ_U(r.cia.counter_a(), 0x0bu, "and one count per clock after it");
  }

  {
    /* force load reloads without needing the timer to be stopped */
    CiaRig r;
    r.write(0x04, 0x08);
    r.write(0x05, 0x00);
    r.run(2);
    r.write(0x0e, 0x01);
    r.run(6);
    US_CHECK(r.cia.counter_a() < 0x08u, "timer moved");
    r.write(0x0e, 0x11);          /* start + force load */
    r.run(2);
    US_CHECK_EQ_U(r.cia.counter_a(), 0x08u, "force load restored the latch");
    US_CHECK_EQ_U(r.read(0x0e) & 0x10, 0x00u, "force load reads back as zero");
  }

  {
    /* one shot stops itself on underflow and clears the start bit */
    CiaRig r;
    r.write(0x04, 0x03);
    r.write(0x05, 0x00);
    r.write(0x0e, 0x09);          /* start, one shot */
    r.run(20);
    US_CHECK_EQ_U(r.read(0x0e) & 0x01, 0x00u, "one shot cleared its start bit");
    US_CHECK_EQ_U(r.cia.counter_a(), 0x03u, "and reloaded the latch");
    US_CHECK_EQ_U(r.cia.icr() & 0x01, 0x01u, "the underflow was recorded");
  }

  {
    /* timer B cascaded on timer A underflows */
    CiaRig r;
    r.write(0x04, 0x02); r.write(0x05, 0x00);   /* timer A period 2 */
    r.write(0x06, 0x03); r.write(0x07, 0x00);   /* timer B counts 3 of them */
    r.write(0x0e, 0x01);                        /* A: start, phi2 */
    r.write(0x0f, 0x41);                        /* B: start, count A underflow */
    r.run(2);
    const uint16_t before = r.cia.counter_b();
    US_CHECK_EQ_U(before, 0x0003u, "timer B loaded its latch");
    r.run(2);
    US_CHECK_EQ_U(r.cia.counter_b(), before, "timer B ignores phi2 in cascade mode");
    r.run(20);
    US_CHECK(r.cia.counter_b() != before, "timer B counted timer A underflows");
  }

  return 0;
}

/* ---- interrupts, the part the pipeline exists for ---------------------- */

int test_interrupts(void)
{
  {
    /* the interrupt line goes low one clock after the underflow, and reading
     * the ICR releases it and clears every event in one go */
    CiaRig r;
    r.write(0x0d, 0x81);          /* enable timer A interrupts */
    r.write(0x04, 0x02); r.write(0x05, 0x00);
    r.write(0x0e, 0x01);

    unsigned cycles = 0;
    while (!r.cia.int_asserted() && cycles < 100) { r.run(1); cycles++; }
    US_CHECK(r.cia.int_asserted(), "the interrupt line went low");

    const data_t icr = r.read(0x0d);
    US_CHECK_EQ_U(icr & 0x81, 0x81u, "ICR reports the event and bit 7");
    US_CHECK(r.cia.int_asserted() == false, "reading the ICR released the line");
    US_CHECK_EQ_U(r.read(0x0d), 0x00u, "and cleared every event");
  }

  {
    /* an event that arrives while masked is remembered, and unmasking it
     * afterwards delivers the interrupt */
    CiaRig r;
    r.write(0x04, 0x02); r.write(0x05, 0x00);
    r.write(0x0e, 0x01);
    r.run(20);
    US_CHECK(r.cia.int_asserted() == false, "masked events do not interrupt");
    US_CHECK_EQ_U(r.cia.icr() & 0x01, 0x01u, "but they are recorded");

    r.write(0x0d, 0x81);          /* now enable it */
    r.run(2);
    US_CHECK(r.cia.int_asserted(), "unmasking a pending event interrupts");
  }

  {
    /* FLAG interrupts on the falling edge only */
    CiaRig r;
    r.write(0x0d, 0x90);          /* enable FLAG */
    r.cia.set_flag(true);
    r.run(2);
    US_CHECK(r.cia.int_asserted() == false, "a high FLAG does nothing");
    r.cia.set_flag(false);
    r.run(2);
    US_CHECK(r.cia.int_asserted(), "the falling edge of FLAG interrupts");
  }

  {
    /* the CIA drives the right line: CIA1 the IRQ, CIA2 the NMI */
    Bus bus;
    Mos6526 cia1(bus, CiaLine::Irq);
    Mos6526 cia2(bus, CiaLine::Nmi);

    cia1.io_write(0x0d, 0x81);
    cia1.io_write(0x04, 0x02); cia1.io_write(0x05, 0x00);
    cia1.io_write(0x0e, 0x01);
    for (int i = 0; i < 20; i++) { cia1.tick(); cia2.tick(); }
    US_CHECK(bus.irq_asserted(), "CIA1 pulls the IRQ line");
    US_CHECK(bus.nmi_asserted() == false, "and not the NMI line");

    cia2.io_write(0x0d, 0x81);
    cia2.io_write(0x04, 0x02); cia2.io_write(0x05, 0x00);
    cia2.io_write(0x0e, 0x01);
    for (int i = 0; i < 20; i++) { cia1.tick(); cia2.tick(); }
    US_CHECK(bus.nmi_asserted(), "CIA2 pulls the NMI line");
  }

  return 0;
}

/* ---- ports ------------------------------------------------------------- */

int test_ports(void)
{
  {
    /* outputs read back the latch, inputs read the outside world */
    CiaRig r;
    r.cia.set_port_a_input(0xff);
    r.write(0x02, 0xf0);          /* high nybble output */
    r.write(0x00, 0xa5);
    US_CHECK_EQ_U(r.read(0x00), 0xafu, "outputs latch, inputs float high");

    r.cia.set_port_a_input(0x0f);
    US_CHECK_EQ_U(r.read(0x00), 0xafu & 0x0fu, "input bits follow the outside");
  }

  {
    /* PB6 goes high for one clock on a timer A underflow in pulse mode */
    CiaRig r;
    r.write(0x03, 0xff);          /* port B all outputs */
    r.write(0x04, 0x02); r.write(0x05, 0x00);
    r.write(0x0e, 0x03);          /* start, PB6 pulse mode */

    bool saw_high = false;
    for (int i = 0; i < 40 && !saw_high; i++) {
      r.run(1);
      if ((r.cia.port_b() & 0x40) != 0) saw_high = true;
    }
    US_CHECK(saw_high, "PB6 pulsed high on the underflow");
    r.run(2);
    US_CHECK_EQ_U(r.cia.port_b() & 0x40, 0x00u, "and went back low");
  }

  {
    /* toggle mode flips PB7 on every timer B underflow */
    CiaRig r;
    r.write(0x03, 0xff);
    r.write(0x06, 0x02); r.write(0x07, 0x00);
    r.write(0x0f, 0x07);          /* start, PB7 output, toggle */
    const data_t first = static_cast<data_t>(r.cia.port_b() & 0x80);
    bool toggled = false;
    for (int i = 0; i < 40 && !toggled; i++) {
      r.run(1);
      if ((r.cia.port_b() & 0x80) != first) toggled = true;
    }
    US_CHECK(toggled, "PB7 toggled on the underflow");
  }

  {
    /* the keyboard matrix: a row driven low pulls its columns low on port B */
    CiaRig r;
    r.write(0x02, 0xff);          /* port A all outputs, drives the rows */
    r.write(0x03, 0x00);          /* port B all inputs, reads the columns */
    r.cia.set_key(7, 7, true);    /* RUN/STOP */
    r.write(0x00, 0xff);
    US_CHECK_EQ_U(r.read(0x01), 0xffu, "no row driven, no key seen");
    r.write(0x00, 0x7f);          /* drive row 7 low */
    US_CHECK_EQ_U(r.read(0x01), 0x7fu, "the pressed key pulls its column low");
    r.cia.clear_keys();
    US_CHECK_EQ_U(r.read(0x01), 0xffu, "and releasing it lets go");
  }

  return 0;
}

/* ---- time of day ------------------------------------------------------- */

int test_tod(void)
{
  CiaRig r;
  r.cia.set_clock_hz(985248);
  r.write(0x0f, 0x00);            /* CRB bit 7 clear: writes set the clock */
  r.write(0x0b, 0x01);            /* hours, also stops the clock */
  r.write(0x0a, 0x00);
  r.write(0x09, 0x00);
  r.write(0x08, 0x00);            /* tenths, starts the clock */

  /* one tenth of a second at 60 Hz mains */
  r.run(985248 / 600 + 2);
  US_CHECK_EQ_U(r.read(0x08), 0x01u, "the tenths advanced");

  /* reading the hours freezes the time until the tenths are read */
  const data_t hours = r.read(0x0b);
  US_CHECK_EQ_U(hours, 0x01u, "hours read back");
  const data_t tenths_a = r.read(0x09);
  r.run(985248 / 60);
  const data_t tenths_b = r.read(0x09);
  US_CHECK_EQ_U(tenths_a, tenths_b, "the time stays frozen after reading hours");
  r.read(0x08);                   /* unfreeze */
  r.run(985248 / 600 + 2);
  US_CHECK(r.read(0x08) != 0x00u, "and runs again after reading the tenths");

  return 0;
}

/* ---- the test programs ------------------------------------------------- */

int run_program(const char * label, const char * path, uint64_t limit)
{
  TestC64 c64;
  if (!c64.boot()) {
    ++us_test_failures;
    ++us_test_checks;
    printf("  FAIL %s: the machine did not boot\n", label);
    return 1;
  }

  uint64_t cycles = 0;
  addr_t stopped = 0;
  const TestC64::RunResult r = c64.run_prg(path, limit, cycles, stopped);

  ++us_test_checks;
  if (r == TestC64::RunResult::Passed) {
    printf("  %-16s passed (%llu cycles)\n", label,
           static_cast<unsigned long long>(cycles));
    return 0;
  }

  ++us_test_failures;
  printf("  FAIL %-11s %s at $%04x after %llu cycles\n",
         label, result_name(r), stopped,
         static_cast<unsigned long long>(cycles));
  return 1;
}

int test_acid800_interrupts(void)
{
  /* These two were deferred from step 2.3: cpu_clisei needs a CIA1 timer
   * interrupt, cpu_bugs needs a CIA2 timer NMI. */
  run_program("cpu_clisei", US_ASSET_DIR "/tests/Acid800/cpu_clisei.prg",
              50ull * 1000ull * 1000ull);
  run_program("cpu_bugs", US_ASSET_DIR "/tests/Acid800/cpu_bugs.prg",
              50ull * 1000ull * 1000ull);
  return 0;
}

int test_lorenz_cia(void)
{
  static const char * const kTests[] = {
    "cia1ta", "cia1tb", "cia2ta", "cia2tb",
    "cia1tab", "cia1tb123", "cia2tb123",
    "cia1pb6", "cia1pb7", "cia2pb6", "cia2pb7",
    "icr01", "imr", "oneshot", "flipos", "cnto2", "cntdef", "loadth",
  };

  for (const char * name : kTests) {
    char path[512];
    snprintf(path, sizeof(path), "%s/tests/Lorenz-2.15/%s.prg",
             US_ASSET_DIR, name);
    run_program(name, path, 60ull * 1000ull * 1000ull);
  }
  return 0;
}


/* ---- skipping and stepping have to be the same thing -------------------- */

/**
 * @brief The bus does not clock a CIA every cycle any more.
 *
 * It asks how long the chip can be left alone and leaves it alone, catching it
 * up whenever anything looks. That is only allowed if it cannot be told apart
 * from walking every cycle, so here the same tune is run both ways and the two
 * register traces are compared event by event: what was written, what value,
 * and how many cycles after the last one.
 */
int test_skipping_matches_stepping(void)
{
  const char * path =
    "/mnt/loud/DocThierry/retro/Commodore64/sidtunes/favorites/rsid/"
    "50Kent_in_da_Club.sid";

  std::vector<data_t> bytes;
  FILE * f = fopen(path, "rb");
  if (f == nullptr) {
    printf("  skipped the equivalence check, no tune available\n");
    return 0;
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  bytes.resize(static_cast<size_t>(size));
  const bool read_ok = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
  fclose(f);
  if (!read_ok || bytes.empty()) {
    printf("  skipped the equivalence check, tune unreadable\n");
    return 0;
  }

  std::vector<TraceSidBackend::Event> buf_fast(200000), buf_slow(200000);
  TraceSidBackend fast(buf_fast.data(), buf_fast.size());
  TraceSidBackend slow(buf_slow.data(), buf_slow.size());

  size_t counts[2] = { 0, 0 };
  cycle_t real_ticks[2] = { 0, 0 };

  for (int pass = 0; pass < 2; pass++) {
    Machine machine;
    TraceSidBackend & trace = (pass == 0) ? fast : slow;
    machine.set_sid_backend(trace);

    /* the second pass gives up the schedule and walks every cycle */
    if (pass == 1) machine.bus().attach_fast(nullptr, nullptr, nullptr, nullptr);

    Player player(machine);
    if (!player.load_sid(bytes.data(), bytes.size())) return 0;
    if (!player.init_tune(0)) return 0;
    trace.reset();
    player.run_frames(50);

    counts[pass] = trace.count();
    real_ticks[pass] = machine.cia1().real_ticks();
  }

  printf("  %zu events either way, cia1 walked %llu cycles scheduled "
         "against %llu stepped\n", counts[0],
         static_cast<unsigned long long>(real_ticks[0]),
         static_cast<unsigned long long>(real_ticks[1]));

  US_CHECK_EQ_U(counts[0], counts[1], "the same number of register events");
  US_CHECK(real_ticks[0] < real_ticks[1] / 4,
           "and the scheduled run walked far fewer cycles");

  size_t differences = 0;
  const size_t n = (counts[0] < counts[1]) ? counts[0] : counts[1];
  for (size_t i = 0; i < n; i++) {
    const TraceSidBackend::Event & a = fast.at(i);
    const TraceSidBackend::Event & b = slow.at(i);
    if (a.kind != b.kind || a.reg != b.reg || a.value != b.value ||
        a.delta != b.delta || a.cycle != b.cycle) {
      if (differences == 0) {
        printf("  first difference at event %zu: scheduled %c $%02x:%02x "
               "[C]%u @%llu, stepped %c $%02x:%02x [C]%u @%llu\n",
               i, a.kind, a.reg, a.value, a.delta,
               static_cast<unsigned long long>(a.cycle),
               b.kind, b.reg, b.value, b.delta,
               static_cast<unsigned long long>(b.cycle));
      }
      ++differences;
    }
  }
  US_CHECK_EQ_U(differences, 0u, "every event is identical");

  return 0;
}


/**
 * @brief Timer A prescaling timer B, scheduled against stepped.
 *
 * The shape `prg/Musik_Run_Stop.prg` runs: timer A free running on phi2 with a
 * latch of two, so it underflows every three cycles, as a prescaler for timer
 * B cascaded off it. Every clock does something, so the scheduler correctly
 * refused to skip and the program walked 97% of its cycles. `cascade_skip()`
 * applies a whole run of timer A underflows as arithmetic instead, which is
 * only sound if that run really is invisible from outside.
 *
 * The chip is driven directly rather than through a Machine, because a Machine
 * runs the KERNAL and the KERNAL reprograms CIA1 out from under the test: the
 * first version of this had timer A on a latch of 16421 and was not testing the
 * cascade at all. One instance is caught up in strides, the other is ticked
 * every clock, and they are compared on a stride that is prime so it never
 * lands in step with the three clock period.
 */
struct CascadeRig {
  Bus bus;
  Mos6526 cia;
  CascadeRig(void) : cia(bus, CiaLine::Irq) {}

  void program(void)
  {
    cia.io_write(0x0d, 0x7f);   /* clear every interrupt mask */
    cia.io_write(0x04, 0x02);   /* timer A latch 2, an underflow every three */
    cia.io_write(0x05, 0x00);
    cia.io_write(0x06, 0x40);   /* timer B counts 0x0140 of them */
    cia.io_write(0x07, 0x01);
    cia.io_write(0x0d, 0x82);   /* timer B interrupt on, timer A's left off */
    cia.io_write(0x0e, 0x11);   /* timer A: force load, start, count phi2 */
    cia.io_write(0x0f, 0x51);   /* timer B: force load, start, count timer A */
  }
};

int test_cascade_matches_stepping(void)
{
  CascadeRig fast, slow;
  fast.program();
  slow.program();

  constexpr uint64_t kCycles = 2ull * 1000ull * 1000ull;
  constexpr uint64_t kStride = 997; /* prime, never in step with the period */
  uint64_t diverged_at = 0;
  unsigned irqs = 0;
  bool irq_was = false;

  for (uint64_t at = kStride; at <= kCycles && diverged_at == 0; at += kStride) {
    /* the scheduled one is allowed to get there however it likes */
    fast.cia.catch_up(static_cast<cycle_t>(at));
    while (slow.cia.ticks() < static_cast<cycle_t>(at)) slow.cia.tick();

    if (fast.cia.counter_a() != slow.cia.counter_a() ||
        fast.cia.counter_b() != slow.cia.counter_b() ||
        fast.cia.icr() != slow.cia.icr() ||
        fast.cia.int_asserted() != slow.cia.int_asserted()) {
      diverged_at = at;
      printf("  diverged at %llu: scheduled A=%u B=%u icr=%02x irq=%d, "
             "stepped A=%u B=%u icr=%02x irq=%d\n",
             static_cast<unsigned long long>(diverged_at),
             fast.cia.counter_a(), fast.cia.counter_b(), fast.cia.icr(),
             fast.cia.int_asserted() ? 1 : 0,
             slow.cia.counter_a(), slow.cia.counter_b(), slow.cia.icr(),
             slow.cia.int_asserted() ? 1 : 0);
    }

    /* Reading ICR clears it and lets the interrupt arm again, so this both
     * counts timer B's underflows and keeps the line from simply staying low
     * after the first one. Both chips are read so they stay symmetrical. */
    const data_t flags = fast.cia.io_read(0x0d);
    const data_t other = slow.cia.io_read(0x0d);
    if (flags != other) diverged_at = at;
    if ((flags & 0x02) != 0) ++irqs;
  }
  (void)irq_was;

  US_CHECK(diverged_at == 0,
           "the cascade scheduled and stepped never differ (cycle %llu)",
           static_cast<unsigned long long>(diverged_at));

  /* If timer B never underflowed this proved nothing: that underflow is the
   * one event the run has to stop short of. */
  US_CHECK(irqs > 2, "and timer B underflowed while it ran (%u times)", irqs);

  const cycle_t walked = fast.cia.real_ticks();
  printf("  cascade: cia walked %llu of %llu cycles (%.1f%%)\n",
         static_cast<unsigned long long>(walked),
         static_cast<unsigned long long>(kCycles),
         100.0 * static_cast<double>(walked) / static_cast<double>(kCycles));

  /* It used to walk every one of them. Anything under a fifth means the run is
   * being applied in bulk rather than counted out. */
  US_CHECK(walked < kCycles / 5,
           "and the prescaler run is applied in bulk (%llu of %llu walked)",
           static_cast<unsigned long long>(walked),
           static_cast<unsigned long long>(kCycles));

  return 0;
}

} /* namespace */

int us_test_cia(void)
{
  US_TEST_BEGIN("cia/mos6526");

  test_timer_basics();
  test_interrupts();
  test_ports();
  test_tod();
  test_acid800_interrupts();
  test_lorenz_cia();
  test_skipping_matches_stepping();
  test_cascade_matches_stepping();

  US_TEST_END("cia/mos6526");
}

US_TEST_MAIN(us_test_cia)
