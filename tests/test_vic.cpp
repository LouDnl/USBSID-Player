/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_vic.cpp
 * Step 2.6 gate: cycles per frame for all four video models, the raster
 * interrupt landing on the right cycle, the cycles a bad line and a sprite
 * steal from the CPU, and the Acid800 instruction timing tests, which measure
 * the machine's timing through the raster counter.
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

#include "core/bus.h"
#include "machine_harness.h"
#include "mos6569.h"
#include "test_common.h"
#include "tests.h"
#include "vic_timing.h"

#ifndef US_ASSET_DIR
#define US_ASSET_DIR "assets"
#endif

using namespace usbsid;
using namespace us_test;

namespace {

/* A VIC on a bus with nothing else on it */
struct VicRig {
  Bus bus;
  Mos6569 vic;

  explicit VicRig(VideoModel model = VideoModel::Pal6569) : vic(bus, model) {}

  void run(unsigned cycles) { for (unsigned i = 0; i < cycles; i++) vic.tick(); }
  void write(uint8_t reg, data_t v) { vic.io_write(reg, v); }
  data_t read(uint8_t reg) { return vic.io_read(reg); }

  /* advance to the first cycle of a raster line */
  void goto_line(uint16_t line)
  {
    for (unsigned i = 0; i < 400000; i++) {
      if (vic.raster() == line && vic.cycle_in_line() == 1) return;
      vic.tick();
    }
  }
};

/* ---- the frame ---------------------------------------------------------- */

int test_frame_timing(void)
{
  const VideoModel models[] = {
    VideoModel::Pal6569, VideoModel::Ntsc6567R8,
    VideoModel::Ntsc6567R56A, VideoModel::PalN6572,
  };

  for (VideoModel model : models) {
    const VicTiming & t = vic_timing(model);
    VicRig r(model);

    /* count the cycles between two frame boundaries */
    while (!(r.vic.raster() == 0 && r.vic.cycle_in_line() == 1)) r.run(1);
    const uint64_t frame_before = r.vic.frames();
    unsigned cycles = 0;
    do {
      r.run(1);
      ++cycles;
    } while (r.vic.frames() == frame_before);

    ++us_test_checks;
    const unsigned expected = vic_cycles_per_frame(model);
    if (cycles != expected) {
      ++us_test_failures;
      printf("  FAIL %s: %u cycles per frame, expected %u\n",
             t.name, cycles, expected);
    } else {
      printf("  %-16s %2u cycles x %3u lines = %u per frame, %.2f Hz\n",
             t.name, t.cycles_per_line, t.lines_per_frame, expected,
             static_cast<double>(t.clock_hz) / expected);
    }

    /* the raster register follows the line, including the ninth bit */
    r.goto_line(0x100);
    US_CHECK_EQ_U(r.read(0x12), 0x00u, "raster low byte wraps");
    US_CHECK_EQ_U(r.read(0x11) & 0x80, 0x80u, "raster bit 8 in $d011");
    r.goto_line(0x37);
    US_CHECK_EQ_U(r.read(0x12), 0x37u, "raster low byte");
    US_CHECK_EQ_U(r.read(0x11) & 0x80, 0x00u, "raster bit 8 clear again");
  }

  return 0;
}

/* ---- the raster interrupt ---------------------------------------------- */

int test_raster_irq(void)
{
  {
    VicRig r;
    r.write(0x1a, 0x01);          /* enable the raster interrupt */
    r.write(0x12, 0x64);          /* on line 100 */
    r.write(0x11, 0x1b);          /* raster bit 8 clear */

    r.goto_line(99);
    US_CHECK(r.bus.irq_asserted() == false, "no interrupt before the line");

    /* the compare happens in the first cycle of the line */
    r.run(1);
    while (r.vic.raster() != 100) r.run(1);
    US_CHECK_EQ_U(r.vic.cycle_in_line(), 1u, "stopped at the start of line 100");
    r.run(1);
    US_CHECK(r.bus.irq_asserted(), "the raster interrupt fired on cycle 1");
    US_CHECK_EQ_U(r.read(0x19) & 0x81, 0x81u, "$d019 reports raster and bit 7");

    /* writing a one to the flag acknowledges it */
    r.write(0x19, 0x01);
    US_CHECK(r.bus.irq_asserted() == false, "acknowledged");
    US_CHECK_EQ_U(r.read(0x19) & 0x0f, 0x00u, "flag cleared");
  }

  {
    /* a masked interrupt still records its flag but does not pull the line */
    VicRig r;
    r.write(0x1a, 0x00);
    r.write(0x12, 0x10);
    r.goto_line(0x11);
    US_CHECK(r.bus.irq_asserted() == false, "masked raster does not interrupt");
    US_CHECK_EQ_U(r.read(0x19) & 0x01, 0x01u, "but the flag is set");

    /* unmasking it delivers immediately */
    r.write(0x1a, 0x01);
    US_CHECK(r.bus.irq_asserted(), "unmasking delivers the pending flag");
  }

  {
    /* line 0 is compared one cycle later, because the counter wraps there */
    VicRig r;
    r.write(0x1a, 0x01);
    r.write(0x12, 0x00);
    r.write(0x11, 0x1b);
    /* stop on the last line of the frame and clear anything pending, so the
     * next line 0 is the one being observed */
    r.goto_line(static_cast<uint16_t>(r.vic.timing().lines_per_frame - 1));
    r.write(0x19, 0x01);
    while (r.vic.raster() != 0) r.run(1);
    US_CHECK_EQ_U(r.vic.cycle_in_line(), 1u, "at the first cycle of line 0");
    US_CHECK(r.bus.irq_asserted() == false, "nothing yet");
    r.run(1);                     /* cycle 1 is processed */
    US_CHECK(r.bus.irq_asserted() == false, "line 0 does not fire on cycle 1");
    r.run(1);                     /* cycle 2 is processed */
    US_CHECK(r.bus.irq_asserted(), "line 0 fires on cycle 2");
  }

  return 0;
}

/* ---- bad lines and the cycles they steal -------------------------------- */

int test_badlines(void)
{
  {
    /* with the display off there are no bad lines at all */
    VicRig r;
    r.write(0x11, 0x0b);          /* DEN clear, yscroll 3 */
    r.goto_line(0);
    unsigned ba_low = 0;
    for (unsigned i = 0; i < vic_cycles_per_frame(VideoModel::Pal6569); i++) {
      r.run(1);
      if (!r.bus.ba()) ba_low++;
    }
    US_CHECK_EQ_U(ba_low, 0u, "a blanked screen steals nothing");
  }

  {
    /* with the display on, one bad line every eight raster lines inside the
     * display window, each holding BA low for 43 cycles */
    VicRig r;
    r.write(0x11, 0x1b);          /* DEN set, yscroll 3 */
    r.goto_line(0);

    unsigned ba_low = 0;
    unsigned badlines = 0;
    uint16_t last_line = 0xffff;
    for (unsigned i = 0; i < vic_cycles_per_frame(VideoModel::Pal6569); i++) {
      r.run(1);
      if (!r.bus.ba()) ba_low++;
      if (r.vic.badline() && r.vic.raster() != last_line) {
        last_line = r.vic.raster();
        badlines++;
      }
    }
    US_CHECK_EQ_U(badlines, 25u, "25 bad lines in a PAL frame");
    US_CHECK_EQ_U(ba_low, 25u * 43u, "each bad line holds BA low for 43 cycles");
  }

  {
    /* the vertical scroll picks which line of each block is the bad one */
    VicRig r;
    r.write(0x11, 0x18);          /* DEN set, yscroll 0 */
    r.goto_line(0x30);
    US_CHECK(r.vic.badline(), "line $30 is a bad line with yscroll 0");
    r.goto_line(0x31);
    US_CHECK(r.vic.badline() == false, "line $31 is not");
    r.goto_line(0x38);
    US_CHECK(r.vic.badline(), "line $38 is, eight lines later");
  }

  return 0;
}

/* ---- sprites ------------------------------------------------------------ */

int test_sprite_dma(void)
{
  VicRig r;
  r.write(0x11, 0x0b);            /* display off, so only sprites steal */
  r.write(0x00 + 1, 0x64);        /* sprite 0 Y = 100 */
  r.write(0x15, 0x01);            /* enable sprite 0 */

  r.goto_line(0);
  unsigned ba_low = 0;
  unsigned dma_lines = 0;
  uint16_t last_line = 0xffff;
  for (unsigned i = 0; i < vic_cycles_per_frame(VideoModel::Pal6569); i++) {
    r.run(1);
    if (!r.bus.ba()) ba_low++;
    /* count the lines the sprite is actually fetched on, which is what the
     * DMA state means at cycle 55 */
    /* cycle_in_line() has already moved on by the time run() returns, so 56
     * is the state just after cycle 55, which is where the DMA is decided */
    if (r.vic.cycle_in_line() == 56 && r.vic.sprite_dma(0) &&
        r.vic.raster() != last_line) {
      last_line = r.vic.raster();
      dma_lines++;
    }
  }

  US_CHECK_EQ_U(dma_lines, 21u, "a sprite is fetched for 21 lines");
  US_CHECK_EQ_U(ba_low, 21u * 5u, "and holds BA low for five cycles each");

  {
    /* a disabled sprite steals nothing */
    VicRig r2;
    r2.write(0x11, 0x0b);
    r2.write(0x01, 0x64);
    r2.write(0x15, 0x00);
    r2.goto_line(0);
    unsigned low = 0;
    for (unsigned i = 0; i < vic_cycles_per_frame(VideoModel::Pal6569); i++) {
      r2.run(1);
      if (!r2.bus.ba()) low++;
    }
    US_CHECK_EQ_U(low, 0u, "a disabled sprite steals nothing");
  }

  return 0;
}

/* ---- what the CPU actually loses ---------------------------------------- */

int test_cpu_cycles_per_frame(void)
{
  /* A frame is always the same number of cycles; what changes is how many of
   * them the CPU gets. This is the number every SID write timestamp in the
   * frame depends on. */
  struct Case { const char * what; data_t d011; data_t sprites; };
  const Case cases[] = {
    { "screen off",          0x0b, 0x00 },
    { "screen on",           0x1b, 0x00 },
    { "screen on, 8 sprites", 0x1b, 0xff },
  };

  for (const Case & c : cases) {
    TestC64 c64;
    if (!c64.boot()) {
      ++us_test_failures; ++us_test_checks;
      printf("  FAIL %s: machine did not boot\n", c.what);
      continue;
    }
    /* park the CPU in a tight loop so it only ever does read cycles */
    c64.machine.ram().dma_write(0x0340, 0x4c);
    c64.machine.ram().dma_write(0x0341, 0x40);
    c64.machine.ram().dma_write(0x0342, 0x03);
    c64.sync_to_instruction();
    c64.machine.cpu().pc(0x0340);
    c64.machine.mmu().write(0xd011, c.d011);
    c64.machine.mmu().write(0xd015, c.sprites);
    for (uint8_t n = 0; n < 8; n++) {
      c64.machine.mmu().write(static_cast<addr_t>(0xd001 + 2 * n), 0x64);
    }

    /* count the cycles in which the CPU was allowed to run */
    while (!(c64.machine.vic().raster() == 0 &&
             c64.machine.vic().cycle_in_line() == 1)) c64.machine.tick();

    unsigned cpu_cycles = 0;
    const unsigned frame = vic_cycles_per_frame(VideoModel::Pal6569);
    for (unsigned i = 0; i < frame; i++) {
      const addr_t before = c64.machine.cpu().pc();
      const unsigned insn = c64.machine.cpu().insn_cycle();
      c64.machine.tick();
      if (c64.machine.cpu().pc() != before ||
          c64.machine.cpu().insn_cycle() != insn) cpu_cycles++;
    }

    ++us_test_checks;
    printf("  %-21s cpu ran %u of %u cycles (%u stolen)\n",
           c.what, cpu_cycles, frame, frame - cpu_cycles);
  }

  return 0;
}

/* ---- the Acid800 timing tests ------------------------------------------- */

int run_timing_test(const char * label, const char * path, VideoModel model)
{
  TestC64 c64(model);
  if (!c64.boot()) {
    ++us_test_failures; ++us_test_checks;
    printf("  FAIL %s: the machine did not boot\n", label);
    return 1;
  }

  uint64_t cycles = 0;
  addr_t stopped = 0;
  const TestC64::RunResult r =
    c64.run_prg(path, 60ull * 1000ull * 1000ull, cycles, stopped);

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

int test_acid800_timing(void)
{
  run_timing_test("cpu_timing", US_ASSET_DIR "/tests/Acid800/cpu_timing.prg",
                  VideoModel::Pal6569);
  run_timing_test("cpu_timing_ntsc",
                  US_ASSET_DIR "/tests/Acid800/cpu_timing_ntsc.prg",
                  VideoModel::Ntsc6567R8);
  return 0;
}

/* ---- the KERNAL, now against the real chip ------------------------------ */

int test_kernal_boot_with_vic(void)
{
  /* This is the step 2.4 check again, with the raster stub gone. */
  TestC64 c64;
  const bool booted = c64.boot();
  US_CHECK(booted, "the KERNAL boots against the real VIC");
  if (booted) {
    printf("  kernal booted to READY, raster line %u, frame %llu\n",
           c64.machine.vic().raster(),
           static_cast<unsigned long long>(c64.machine.vic().frames()));
  }
  return 0;
}


/* ---- skipping and stepping have to be the same thing -------------------- */

/**
 * @brief The bus does not clock the VIC every cycle any more.
 *
 * A raster line is sixty three cycles and about four of them do anything: BA
 * dropping for a bad line, BA coming back, the raster compare, and the line
 * boundary. The chip is left alone in between, so it has to be impossible to
 * tell that from clocking it. Two machines are run side by side, one scheduled
 * and one walking every cycle, and everything the rest of the machine can see
 * is compared on every single cycle: the raster line, where in the line it is,
 * the BA line and the interrupt line.
 */
int lockstep(const char * what, void (*setup)(Machine &), unsigned max_percent)
{
  TestC64 scheduled;
  TestC64 stepped;
  stepped.machine.bus().attach_fast(nullptr, nullptr, nullptr, nullptr);

  NullSidBackend sink_a, sink_b;
  scheduled.machine.set_sid_backend(sink_a);
  stepped.machine.set_sid_backend(sink_b);

  if (setup != nullptr) { setup(scheduled.machine); setup(stepped.machine); }

  constexpr uint64_t kCycles = 2ull * 1000ull * 1000ull; /* two seconds */
  uint64_t diverged_at = 0;
  /* Which sprites were seen fetching. Sampled rather than checked every cycle,
   * because a sprite's DMA lasts twenty one lines and this is in the inner
   * loop of a two million cycle comparison. */
  unsigned dma_seen = 0;

  for (uint64_t i = 0; i < kCycles && diverged_at == 0; i++) {
    scheduled.machine.tick();
    stepped.machine.tick();

    if (scheduled.machine.vic().raster() != stepped.machine.vic().raster() ||
        scheduled.machine.vic().cycle_in_line() !=
          stepped.machine.vic().cycle_in_line() ||
        scheduled.machine.bus().ba() != stepped.machine.bus().ba() ||
        scheduled.machine.bus().irq_asserted() !=
          stepped.machine.bus().irq_asserted()) {
      diverged_at = i + 1;
    }
    if ((i & 0x3f) == 0) {
      for (uint8_t n = 0; n < 8; n++) {
        if (scheduled.machine.vic().sprite_dma(n)) dma_seen |= (1u << n);
      }
    }
  }

  US_CHECK(diverged_at == 0, "%s: the two machines never differ (cycle %llu)",
           what, static_cast<unsigned long long>(diverged_at));
  US_CHECK_EQ_U(scheduled.machine.vic().frames(),
                stepped.machine.vic().frames(), "and agree on the frame count");

  /* Every sprite that was switched on has to have been seen fetching, or this
   * is quietly re-testing the case with no sprites in it at all. */
  const unsigned enabled = scheduled.machine.vic().peek(0x15);
  US_CHECK_EQ_U(dma_seen, enabled,
                "every enabled sprite was seen fetching");

  const cycle_t walked = scheduled.machine.vic().real_ticks();
  const cycle_t stepped_walked = stepped.machine.vic().real_ticks();
  const double percent = 100.0 * static_cast<double>(walked) /
                                 static_cast<double>(stepped_walked);
  printf("  %-22s vic walked %8llu of %llu cycles (%.1f%%)\n", what,
         static_cast<unsigned long long>(walked),
         static_cast<unsigned long long>(stepped_walked), percent);
  US_CHECK(percent < static_cast<double>(max_percent),
           "%s: and the scheduled one walked far fewer of them (%.1f%%, "
           "allowed %u%%)", what, percent, max_percent);

  return 0;
}

/**
 * @brief Switch on all eight sprites, spread down the screen.
 *
 * The awkward case on purpose. Sprite seven's fetch window starts at cycle 69,
 * which is past the end of a 63 cycle line, so it wraps into the next one, and
 * neighbouring sprites' windows overlap each other. Their Y positions are set
 * eleven lines apart so that several are being fetched at once for part of the
 * frame and none at all for the rest, and every other one is vertically
 * expanded so its DMA runs for forty two lines rather than twenty one.
 */
void setup_sprites(Machine & m)
{
  m.vic().io_write(0xd011, 0x1b); /* screen on, DEN set */
  for (uint8_t n = 0; n < 8; n++) {
    m.vic().io_write(static_cast<addr_t>(0xd001 + 2 * n),
                     static_cast<data_t>(0x32 + 11 * n));
  }
  m.vic().io_write(0xd017, 0xaa); /* every other one vertically expanded */
  m.vic().io_write(0xd015, 0xff); /* and all of them enabled */
}

/** @brief One sprite, which is what a PRG that is not a demo usually has. */
void setup_one_sprite(Machine & m)
{
  m.vic().io_write(0xd011, 0x1b);
  m.vic().io_write(0xd001, 0x64);
  m.vic().io_write(0xd015, 0x01);
}

int test_skipping_matches_stepping(void)
{
  /* No sprites: four cycles of a sixty three cycle line do anything. */
  lockstep("no sprites", nullptr, 5);

  /* One sprite is fetched on twenty one of three hundred and twelve lines, so
   * the cost of having it at all should be close to nothing. This is the case
   * that matters: a single sprite used to take the VIC from walking 1.8% of
   * cycles to walking every one of them, which is what made programs drag on
   * the device while the same tune as a SID file played. */
  lockstep("one sprite", setup_one_sprite, 10);

  /* Eight, half of them expanded, with overlapping and wrapping windows.
   * The bound was 40% when the scheduler stopped at every window edge of every
   * active sprite. Since it stops only where the *union* of those windows
   * changes it walks 5.2%, so 10 is the tripwire now: the windows are five
   * cycles wide and two apart, and going back to per sprite edges means a stop
   * every two cycles and this number climbing straight back through it. */
  lockstep("eight sprites", setup_sprites, 10);

  return 0;
}

} /* namespace */

int us_test_vic(void)
{
  US_TEST_BEGIN("vic/mos6569");

  test_frame_timing();
  test_raster_irq();
  test_badlines();
  test_sprite_dma();
  test_kernal_boot_with_vic();
  test_cpu_cycles_per_frame();
  test_acid800_timing();
  test_skipping_matches_stepping();

  US_TEST_END("vic/mos6569");
}

US_TEST_MAIN(us_test_vic)
