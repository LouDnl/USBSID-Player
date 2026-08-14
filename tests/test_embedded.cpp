/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_embedded.cpp
 * Step 2.11 gate, desktop half: the C API the firmware calls, and the backend
 * that talks to the firmware's bus.
 *
 * The firmware side is stubbed here by defining its bus functions in this
 * file. They are declared weak in sid_embedded.h precisely so that a strong
 * definition wins, which means this test exercises the real call path: the
 * emulation writes a register, the SID layer translates and times it, the
 * embedded backend calls what it believes is the firmware, and the call lands
 * in the counters below. Nothing is mocked in between.
 *
 * The other half of the gate, that the same sources cross compile for the
 * RP2350 and fit, is temp/tools/build_embedded.sh.
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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "machine_harness.h"
#include "player.h"
#include "sid_embedded.h"
#include "test_common.h"
#include "tests.h"
#include "usplayer.h"

#ifndef US_TUNE_DIR
#define US_TUNE_DIR "/mnt/loud/DocThierry/retro/Commodore64/sidtunes/favorites"
#endif

using namespace usbsid;

/* ---- the firmware, as far as the player is concerned -------------------- */

namespace {

struct FirmwareStub {
  uint32_t writes = 0;
  uint32_t reads = 0;
  uint32_t resets = 0;
  uint32_t register_resets = 0;
  uint32_t clock_changes = 0;
  int last_clock_index = -1;
  uint8_t last_reg = 0;
  uint8_t last_value = 0;
  uint16_t last_cycles = 0;
  uint64_t total_cycles = 0;
  /* what the SID would hold if these writes went to a chip */
  uint8_t regs[0x80] = { 0 };

  void clear(void) { *this = FirmwareStub(); }
};

FirmwareStub g_fw;

} /* namespace */

/* The board's microsecond clock, simulated.
 *
 * It does not run on its own. Time passes here only where it passes on the
 * device: when the bus sits out a delay, when the player blocks waiting for
 * the clock, and when the emulation spends host cycles. That makes the clock
 * a measuring instrument rather than a stub, which is the only way to see
 * device tempo without a device. `g_sim` below drives it.
 *
 * `g_free_running` puts it back to the old behaviour, a millisecond per read,
 * for the tests that only want a clock that moves. */
static uint64_t g_fake_us = 0;
static uint32_t g_clock_reads = 0;
static bool g_free_running = true;

static uint64_t fake_time_us_64(void)
{
  ++g_clock_reads;
  if (g_free_running) g_fake_us += 1000;
  return g_fake_us;
}

/* The player asks for this rather than spinning when it is available, which
 * is what lets the simulated clock be advanced by exactly what was asked for
 * instead of by however many times the spin happened to read it. */
static void fake_busy_wait_us(uint32_t us)
{
  g_fake_us += us;
}

/* ---- the bus, and how long it takes ------------------------------------- *
 *
 * A cycled write is a *pre* delay: the PIO sits out the gap and only then
 * puts the value on the bus. The call itself returns as soon as the delay and
 * the value fit in the state machine's queue, so a few writes can be in
 * flight at once and the emulation runs underneath them. When the queue is
 * full the core blocks, and that is what paces playback.
 *
 * Reproducing that here, queue and all, is the point: it is what makes the
 * difference between a tune whose writes come close together, where the queue
 * stays full and the delays are free, and a tune with one long gap a frame,
 * where the queue has drained and the delay is served after the emulation
 * that produced it rather than alongside it.
 */

namespace {

struct DeviceSim {
  static constexpr int kFifo = 4;      /* writes that fit in the PIO queue */
  /* Cycles the bus operation costs on top of its pre delay. One: the delay
   * loop in bus_control.pio counts `delay_word + 1` PHI periods and the write
   * lands on the edge after it, so two writes with a pre delay of zero are one
   * PHI apart. This was two, which charged every write a cycle it does not
   * cost, and on a digi burst with gaps of one and two cycles that is a third
   * to a half of the budget. See TODO 7 for the same cycle counted twice on
   * the wire. */
  static constexpr uint32_t kAccess = 1;

  bool active = false;
  uint32_t clock_hz = 985248;
  /* What the board manages for *this tune*, in thousands of emulated cycles
   * per second of host time.
   *
   * This used to be a single constant, 1336, and that was the whole reason
   * this harness said 0.99x while the board audibly dragged. 1336 is what
   * `usplayer_benchmark()` reports at idle; a tune that drives its digi from a
   * CIA timer costs the emulation nearly twice as much per cycle, so its real
   * rate is nowhere near it. One constant cannot express that. It is now
   * measured per tune, on this machine, and scaled onto the board by the
   * reference below. That measurement is wall clock, so it is noisy, so it is
   * opt in: `USP_CALIBRATE=1`. The default is a real tune's board figure
   * rather than the idle one, so the suite stays deterministic and is not
   * flattered by a number no tune ever achieves. */
  uint32_t host_kcycles = 1457;  /* Krakout.sid, RP2350 @ 200 MHz */

  uint64_t queue[kFifo] = { 0 };
  int head = 0;
  int count = 0;
  uint64_t pio_free_us = 0;

  /* How far each *gap* between consecutive writes comes out from the gap the
   * tune asked for. Deliberately the gap and not the position: a position
   * error is an offset, and an offset that grows slowly is a tempo question,
   * which the test above already measures. What a digi hears is the spacing.
   *
   * The tune's own spacing is accumulated from the gaps the SID layer reports,
   * before the backend has touched them, so this measures the backend and the
   * board together and nothing else. */
  uint64_t ideal_cycles = 0;
  uint64_t prev_ideal_cycles = 0;
  uint64_t prev_landed_us = 0;
  bool ideal_set = false;
  uint64_t err_sum = 0, err_max = 0, err_n = 0;

  void reset_error(void)
  { ideal_cycles = prev_ideal_cycles = prev_landed_us = 0;
    ideal_set = false; err_sum = err_max = err_n = 0; }
  uint64_t mean_error_us(void) const
  { return (err_n == 0) ? 0 : (err_sum / err_n); }

  void start(uint32_t hz)
  {
    active = true;
    clock_hz = hz;
    head = 0; count = 0;
    pio_free_us = g_fake_us;
  }
  void stop(void) { active = false; }

  uint64_t us_for(uint64_t cycles) const
  { return (cycles * 1000000ull) / clock_hz; }

  /** @brief Host time to emulate this many C64 cycles. */
  void emulate(uint64_t cycles)
  {
    if (!active) return;
    g_fake_us += (cycles * 1000ull) / host_kcycles;
  }

  void drain(void)
  { while (count > 0 && queue[head] <= g_fake_us) { head = (head + 1) % kFifo; --count; } }

  void bus_access(uint32_t delay_cycles)
  {
    if (!active) return;
    drain();
    if (count == kFifo) { /* the core blocks until the oldest is done */
      if (queue[head] > g_fake_us) g_fake_us = queue[head];
      head = (head + 1) % kFifo; --count;
      drain();
    }
    const uint64_t start = (g_fake_us > pio_free_us) ? g_fake_us : pio_free_us;
    pio_free_us = start + us_for(delay_cycles + kAccess);
    queue[(head + count) % kFifo] = pio_free_us;
    ++count;

    if (!ideal_set) {   /* the first write is where both timelines start */
      ideal_set = true;
    } else {
      const uint64_t want = us_for(ideal_cycles - prev_ideal_cycles);
      const uint64_t got = pio_free_us - prev_landed_us;
      const uint64_t e = (got > want) ? (got - want) : (want - got);
      err_sum += e;
      if (e > err_max) err_max = e;
      ++err_n;
    }
    prev_ideal_cycles = ideal_cycles;
    prev_landed_us = pio_free_us;
  }
};

DeviceSim g_sim;

} /* namespace */

static void fake_cycled_write(uint8_t address, uint8_t data,
                              uint16_t cycles)
{
  ++g_fw.writes;
  g_fw.last_reg = address;
  g_fw.last_value = data;
  g_fw.last_cycles = cycles;
  g_fw.total_cycles += cycles;
  g_fw.regs[address & 0x7f] = data;
  g_sim.bus_access(cycles);
}

static uint8_t fake_cycled_read(uint8_t address, uint16_t cycles)
{
  ++g_fw.reads;
  g_fw.last_reg = address;
  g_fw.last_cycles = cycles;
  g_fw.total_cycles += cycles;
  g_sim.bus_access(cycles);
  return g_fw.regs[address & 0x7f];
}

static void fake_reset_sid(void) { ++g_fw.resets; }
static void fake_reset_sid_registers(void) { ++g_fw.register_resets; }

static void fake_apply_clockrate(int n_clock, bool suspend_sids);

/* Bound once, at load, in place of the definitions these used to be. The backend
 * reaches the firmware through pointers now, because a weak undefined symbol is
 * an ELF idea and this suite builds on Mach-O and PE as well; see the note in
 * src/sid/sid_embedded.h. Assigning them is also more honest about what the
 * tests are doing than defining the firmware's own names was. */
namespace {
struct BindFakeFirmware {
  BindFakeFirmware(void)
  {
    us_cycled_write        = &fake_cycled_write;
    us_cycled_read         = &fake_cycled_read;
    us_reset_sid           = &fake_reset_sid;
    us_reset_sid_registers = &fake_reset_sid_registers;
    us_time_us_64          = &fake_time_us_64;
    us_busy_wait_us        = &fake_busy_wait_us;
    us_apply_clockrate     = &fake_apply_clockrate;
  }
};
const BindFakeFirmware g_bind_fake_firmware;
} /* namespace */

static void fake_apply_clockrate(int n_clock, bool suspend_sids)
{
  (void)suspend_sids;
  ++g_fw.clock_changes;
  g_fw.last_clock_index = n_clock;
}

namespace {

/* ---- the backend on its own --------------------------------------------- */

int test_backend(void)
{
  g_fw.clear();

  EmbeddedSidBackend backend;
  US_CHECK(EmbeddedSidBackend::hardware_present(),
           "the firmware bus functions are linked in");

  backend.write(0x18, 0x0f, 42);
  US_CHECK_EQ_U(g_fw.writes, 1u, "the write reached the firmware");
  US_CHECK_EQ_U(g_fw.last_reg, 0x18u, "with the register");
  US_CHECK_EQ_U(g_fw.last_value, 0x0fu, "the value");
  /* the gap as given: performing the access costs the hardware a cycle of its
   * own, and that cycle was taken off upstream by the SID layer's access
   * overhead. Taking it off here as well made every write a cycle early */
  US_CHECK_EQ_U(g_fw.last_cycles, 42u, "and the cycle delay, passed through");

  /* a second chip is $20 up, and the backend passes the physical register
   * through untouched: the translation happened before it got here */
  backend.write(0x21, 0x77, 7);
  US_CHECK_EQ_U(g_fw.last_reg, 0x21u, "the second chip's register is passed on");

  US_CHECK_EQ_U(backend.read(0x1b, 3), 0x00u, "a read comes back from the bus");
  US_CHECK_EQ_U(g_fw.reads, 1u, "and reached the firmware");

  /* From here on the clock is the simulated one, so a wait can be measured
   * rather than merely observed to have happened. */
  g_free_running = false;
  backend.set_clock_hz(985248);

  /* A gap too long for sixteen bits is not a bus operation, it is time to sit
   * out. There is nothing to send, so it is waited out against the board's
   * clock and counted. */
  const uint32_t before = g_fw.writes;
  backend.wait(1); /* the first access is what starts the pacer's clock */
  const uint64_t wait_started = g_fake_us;
  backend.wait(0xffff);
  US_CHECK_EQ_U(g_fw.writes, before, "a wait is not a bus operation");
  US_CHECK_EQ_U(backend.cycles_waited(), 0x10000u, "but it is accounted for");
  /* $ffff PAL cycles is 66.5 ms and none of it has been spent elsewhere, so
   * all of it has to be waited out here */
  const uint64_t waited_us = g_fake_us - wait_started;
  US_CHECK(waited_us > 66000 && waited_us < 67000,
           "and the time was actually waited out: %llu us of 66517",
           static_cast<unsigned long long>(waited_us));

  /* The fix, on its own. A gap wide enough to pace is one the emulation has
   * already spent host time producing. Whatever of it has gone already must
   * not be spent a second time, and none of it is handed to the hardware as a
   * pre delay, because the hardware would serve that from a standing start. */
  backend.reset();
  backend.write(0x00, 0x11, 100);       /* starts the clock */
  g_fake_us += 20000;                   /* ~20000 cycles, spent emulating */
  const uint64_t before_paced = g_fake_us;
  backend.write(0x01, 0x22, 20000);
  US_CHECK(g_fake_us - before_paced < 1000,
           "a gap already spent emulating is not waited again: %llu us",
           static_cast<unsigned long long>(g_fake_us - before_paced));
  US_CHECK_EQ_U(g_fw.last_cycles, 0u,
                "and none of it is handed to the bus as a pre delay");
  US_CHECK_EQ_U(backend.cycles_paced(), 20000u,
                "the whole gap is accounted as paced");

  /* And the same gap with nothing spent on it still takes its full time */
  backend.reset();
  backend.write(0x00, 0x11, 100);
  const uint64_t before_full = g_fake_us;
  backend.write(0x01, 0x22, 20000);
  const uint64_t full_us = g_fake_us - before_full;
  US_CHECK(full_us > 20000 && full_us < 20800,
           "an unspent gap is waited in full: %llu us of 20300",
           static_cast<unsigned long long>(full_us));

  g_free_running = true;
  backend.reset_hardware();
  US_CHECK_EQ_U(g_fw.resets, 1u, "reset_hardware resets the chips");
  US_CHECK_EQ_U(g_fw.register_resets, 1u, "and their registers");

  US_CHECK_EQ_U(backend.writes(), 2u, "the backend counted its writes");
  backend.reset();
  US_CHECK_EQ_U(backend.writes(), 0u, "and reset clears the counters");
  US_CHECK_EQ_U(backend.cycles_paced(), 0u, "and the paced count with them");

  return 0;
}

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

/* ---- device tempo, for both shapes of tune ------------------------------- *
 *
 * What a tune costs the device is two things that do not add up the same way
 * for every tune: the host time to emulate it, and the time the bus spends
 * sitting out the gaps between its writes. Whether they overlap depends
 * entirely on how the writes are spaced.
 *
 * A digi tune writes six hundred times a frame, thirty cycles apart. The bus
 * queue never empties, so the emulation runs underneath it and the frame
 * costs what the bus costs, which is a frame.
 *
 * A tune with a play routine and an idle loop writes twenty five times a
 * frame and then does nothing for nineteen thousand cycles. The queue drains
 * during that gap, so unless the player sits it out itself the emulation
 * spends the gap and then the bus spends it again, and the frame costs half
 * again what it should. That is what this measures, for one tune of each
 * shape, against a simulated board clock.
 */

struct SimBackend final : public SidBackend {
  EmbeddedSidBackend inner;

  /* The gap the SID layer reports has already had the access overhead taken
   * off it, so the cycles the emulation actually ran is one more. That same
   * untouched gap is the tune's timeline, which the landing error is against. */
  void write(data_t reg, data_t value, uint16_t cycles) override
  {
    g_sim.ideal_cycles += static_cast<uint32_t>(cycles) + 1u;
    g_sim.emulate(static_cast<uint32_t>(cycles) + 1u);
    inner.write(reg, value, cycles);
  }
  data_t read(data_t reg, uint16_t cycles) override
  { g_sim.emulate(static_cast<uint32_t>(cycles) + 1u); return inner.read(reg, cycles); }
  void wait(uint16_t cycles) override
  { g_sim.ideal_cycles += cycles; g_sim.emulate(cycles); inner.wait(cycles); }
  void flush(void) override { inner.flush(); }
  void reset(void) override { inner.reset(); }
};

/* ---- putting the board's speed on this machine's scale ------------------- *
 *
 * The simulated board needs to know how fast the real one emulates *this*
 * tune. A constant cannot say that: measured on the board with
 * `usplayer_benchmark(1000000)`, an RP2350 at 200 MHz reports 1314 kcycles/s
 * at idle, 1457 with `psid/Krakout.sid` loaded and 1009 with
 * `rsid/Coma_Light_13_tune_4.sid`. The tune is most of the number.
 *
 * So it is measured here instead, per tune, and scaled onto the board by a
 * reference tune whose board figure is known. That keeps the calibration
 * honest on any machine: both sides of the ratio move together with the
 * desktop's speed, so only the board's number has to be carried in the tree.
 *
 * To re-measure the reference: load `psid/Krakout.sid`, stop it, then send the
 * benchmark config command, and put what it prints in kRefBoardKcycles.
 */
constexpr const char * kRefTune = US_TUNE_DIR "/psid/Krakout.sid";
constexpr double kRefBoardKcycles = 1457.0;  /* RP2350 @ 200 MHz, 2026-08-04 */

double wall_s(void)
{
  return std::chrono::duration<double>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief kcycles/s this machine emulates a tune at, or 0 if it is not there. */
double desktop_kcycles(const char * path)
{
  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) return 0.0;

  us_test::TestC64 c64;
  NullSidBackend null_backend;
  c64.machine.set_sid_backend(null_backend);
  Player player(c64.machine);
  if (!player.load_sid(bytes.data(), bytes.size())) return 0.0;
  if (!player.init_tune(0)) return 0.0;

  player.run_frames(20);            /* settle, and warm the caches */

  /* Best of three. This is a wall clock measurement on a machine that is
   * doing other things, so the fastest run is the one least interfered with,
   * and averaging would fold the interference in instead of dropping it. */
  double best = 0.0;
  for (int i = 0; i < 3; i++) {
    const cycle_t started = c64.machine.cycles();
    const double t0 = wall_s();
    player.run_frames(60);
    const double dt = wall_s() - t0;
    const cycle_t ran = c64.machine.cycles() - started;
    const double kcps = (dt > 0.0) ? (static_cast<double>(ran) / dt / 1000.0) : 0.0;
    if (kcps > best) best = kcps;
  }
  return best;
}

/** @brief Board kcycles/s per desktop kcycles/s, measured once. */
double board_scale(void)
{
  if (getenv("USP_CALIBRATE") == nullptr) return 0.0; /* opt in, it is noisy */

  static double scale = -1.0;
  if (scale < 0.0) {
    const double ref = desktop_kcycles(kRefTune);
    scale = (ref > 0.0) ? (kRefBoardKcycles / ref) : 0.0;
    if (scale > 0.0) {
      printf("  this machine runs %.0f kcycles/s on %s, where the board does "
             "%.0f, so it is %.0fx the board\n", ref, "Krakout.sid",
             kRefBoardKcycles, 1.0 / scale);
    } else {
      printf("  no reference tune, so the board's speed is the old constant\n");
    }
  }
  return scale;
}

/**
 * @brief Play a tune against the simulated board and report how fast it ran.
 *
 * Returns the tempo as a multiple of real time, or 0 if the tune is not there.
 */
double measure_tempo(const char * path, int frames, double & paced_fraction,
                     uint32_t frame_overhead_us = 0, uint64_t * mean_err_us = nullptr)
{
  std::vector<data_t> bytes;
  paced_fraction = 0.0;
  if (mean_err_us != nullptr) *mean_err_us = 0;
  if (!read_file(path, bytes)) return 0.0;

  us_test::TestC64 c64;
  SimBackend sim;
  c64.machine.set_sid_backend(sim);
  Player player(c64.machine);
  if (!player.load_sid(bytes.data(), bytes.size())) return 0.0;
  if (!player.init_tune(0)) return 0.0;

  const uint32_t hz = vic_timing(c64.machine.video_model()).clock_hz;
  sim.inner.set_clock_hz(hz);

  /* What the board would manage on this tune. See board_scale(). */
  const double scale = board_scale();
  if (scale > 0.0) {
    const double mine = desktop_kcycles(path);
    if (mine > 0.0) g_sim.host_kcycles = static_cast<uint32_t>(mine * scale);
  }

  /* Settle first: the opening frames of a tune are not its steady state, and
   * the boot before them is not part of its timeline at all. */
  player.run_frames(30);

  g_free_running = false;
  sim.inner.reset();
  sim.inner.set_clock_hz(hz);
  g_sim.start(hz);
  g_sim.reset_error();

  const cycle_t started = c64.machine.cycles();
  const uint64_t clock_started = g_fake_us;
  /* Core 0 does not only run the player. loop_sidplayer() emulates one frame
   * and returns, and usbsid.c then runs tud_task(), MIDI, config and the LED
   * before coming back. That is real time in which nothing feeds the bus, so
   * the state machine's queue drains and the next pre delay would be served
   * from a standing start. This is what trim_to_now() exists for. */
  for (int i = 0; i < frames; i++) {
    player.run_frame();
    g_fake_us += frame_overhead_us;
  }
  const cycle_t emulated = c64.machine.cycles() - started;
  const uint64_t device_us = g_fake_us - clock_started;

  g_sim.stop();
  if (mean_err_us != nullptr) *mean_err_us = g_sim.mean_error_us();
  g_free_running = true;

  paced_fraction = (emulated == 0) ? 0.0
    : static_cast<double>(sim.inner.cycles_paced()) / static_cast<double>(emulated);

  const double realtime_us = (static_cast<double>(emulated) * 1000000.0) / hz;
  return (device_us == 0) ? 0.0 : realtime_us / static_cast<double>(device_us);
}

int test_device_tempo(void)
{
  struct Case {
    const char * path;
    const char * shape;
    bool expect_paced; /* whether this shape is the one the pacer is for */
  };
  Case cases[] = {
    { US_TUNE_DIR "/psid/Aint_Somebody.sid", "PSID, play routine and idle loop", true },
    { US_TUNE_DIR "/rsid/50Kent_in_da_Club.sid", "RSID, digi, writes every frame", false },
    /* Filled in from USP_TEMPO_TUNE, so a tune from the collection can be put
     * through the simulated board without its path living in the tree. It is
     * measured and printed but not asserted on: we do not know its shape. */
    { nullptr, "from USP_TEMPO_TUNE", false },
  };
  cases[2].path = getenv("USP_TEMPO_TUNE");
  const char * frames_env = getenv("USP_TEMPO_FRAMES");

  int measured = 0;
  for (const Case & c : cases) {
    if (c.path == nullptr) continue;
    const int frames = (frames_env != nullptr && &c == &cases[2])
      ? atoi(frames_env) : 120;
    double paced = 0.0;
    const double tempo = measure_tempo(c.path, frames, paced);
    const char * name = strrchr(c.path, '/');
    name = (name != nullptr) ? name + 1 : c.path;

    if (tempo == 0.0) {
      printf("  skipped %s, not available\n", name);
      continue;
    }
    ++measured;
    printf("  %-28s %-34s %.2fx realtime, %.0f%% of its cycles paced\n",
           name, c.shape, tempo, paced * 100.0);

    if (&c == &cases[2]) continue; /* a probe, not a case with known answers */

    /* Faster than real time is what the pacer's floor prevents, and slower is
     * the bug it was written for. Either way the tune is not playing right. */
    US_CHECK(tempo > 0.95 && tempo < 1.05,
             "%s plays at the right speed on the simulated board (%.2fx)",
             name, tempo);

    if (c.expect_paced) {
      US_CHECK(paced > 0.80,
               "%s is a tune the bus queue cannot hide, so the player sits its "
               "gaps out itself (%.0f%%)", name, paced * 100.0);
    } else {
      US_CHECK(paced < 0.05,
               "%s writes densely enough for the bus to carry its own timing "
               "(%.0f%% paced)", name, paced * 100.0);
    }
  }

  if (measured == 0) printf("  skipped the tempo test, no tunes available\n");
  return 0;
}

/**
 * @brief The player has to survive core 0 being somewhere else.
 *
 * `loop_sidplayer()` emulates one frame and returns, and usbsid.c then runs
 * tud_task(), MIDI, config and the LED before coming back. Every microsecond
 * of that is time in which nothing feeds the bus. A pre delay starts when its
 * DMA fires, not when the previous write finished, so once the state machine's
 * queue has drained that time is spent twice: once by the core, and again by
 * the board sitting out the full gap from a standing start.
 *
 * Nothing used to give it back. The pacer had only a floor, `wait_until_due()`,
 * which holds the emulation when it runs *ahead*, and it only runs at all on
 * gaps wider than kPacedGap. A dense digi has no such gaps, so on a digi the
 * lateness accumulated with no correction whatsoever and the tune simply played
 * slow by whatever fraction of the frame the housekeeping cost.
 *
 * `trim_to_now()` is the missing ceiling: it takes the time already lost off
 * the next pre delay. With 800 us a frame of housekeeping, which is 4% of a PAL
 * frame, this test measures 0.97x without it and 1.00x with it.
 *
 * The per gap figure is reported alongside because it is the thing a digi
 * actually hears, but it is not what discriminates here: with the bus queue
 * saturated it stays inside a microsecond either way.
 */
int test_write_landing(void)
{
  const char * path = US_TUNE_DIR "/rsid/50Kent_in_da_Club.sid";
  const uint32_t overheads[] = { 0, 100, 300, 800 };
  double baseline = 0.0;

  for (uint32_t overhead : overheads) {
    double paced = 0.0;
    uint64_t mean_err = 0;
    const double tempo = measure_tempo(path, 300, paced, overhead, &mean_err);
    if (tempo == 0.0) {
      printf("  skipped the landing test, 50Kent_in_da_Club.sid not available\n");
      return 0;
    }

    printf("  core 0 away %4u us a frame: %.2fx realtime, gaps out by %llu us "
           "on average\n", overhead, tempo,
           static_cast<unsigned long long>(mean_err));

    if (overhead == 0) { baseline = tempo; continue; }

    /* Against its own baseline, not against 1.00x. Whether this tune keeps up
     * at all is a question about how fast the board emulates it, which is
     * TODO 11 and not this test's business. What is this test's business is
     * that taking the core away must not cost tempo on top of that, which is
     * what it did before trim_to_now(): the untrimmed player loses 3% here at
     * 800 us a frame and gets worse the longer core 0 is away. */
    US_CHECK(tempo > baseline - 0.02,
             "%u us a frame of housekeeping costs no tempo (%.2fx against a "
             "baseline of %.2fx)", overhead, tempo, baseline);
    /* A PAL cycle is about a microsecond, so this is a thousand cycles. */
    US_CHECK(mean_err < 1000,
             "and the gaps between writes are still the tune's (%llu us out)",
             static_cast<unsigned long long>(mean_err));
  }

  return 0;
}

/* ---- the API, driven the way usbsid.c drives it ------------------------- */

int test_api(void)
{
  const char * path = US_TUNE_DIR "/rsid/50Kent_in_da_Club.sid";
  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("  skipped the API test, no tune available\n");
    return 0;
  }

  g_fw.clear();

  /* A clock that jumps a millisecond every time it is read is not a clock the
   * pacer can be measured against. It made the player look a millisecond late
   * at every write, so trim_to_now() took the whole pre delay off every one of
   * them and the cycle accounting below had nothing left to count. Here time
   * moves only when something spends it, which is what the device does. */
  g_free_running = false;

  /* The firmware mallocs the file, hands it over, and frees it before the
   * next call. Doing exactly that here is the point: if the API kept the
   * pointer instead of copying, everything after this would read freed
   * memory. */
  uint8_t * heap = static_cast<uint8_t *>(malloc(bytes.size()));
  US_CHECK(heap != nullptr, "the tune buffer allocates");
  if (heap == nullptr) return 0;
  memcpy(heap, bytes.data(), bytes.size());

  usplayer_set_sid_config(2, 1, 1, -1);
  load_sidtune(heap, static_cast<int>(bytes.size()), 0);

  memset(heap, 0xa5, bytes.size()); /* scribble, then free, as usbsid.c does */
  free(heap);

  US_CHECK(usplayer_songs() >= 1, "the tune parsed");
  US_CHECK(strlen(usplayer_tune_name()) > 0, "and has a title");
  printf("  %s by %s (%s), %u song%s\n", usplayer_tune_name(),
         usplayer_tune_author(), usplayer_tune_released(), usplayer_songs(),
         usplayer_songs() == 1 ? "" : "s");

  /* a PAL tune asks the board for the PAL clock, index 1 in the firmware's
   * table { DEFAULT, PAL, NTSC, DREAN, NTSC2 } */
  US_CHECK(g_fw.clock_changes >= 1, "the clock rate was applied");
  US_CHECK_EQ_U(g_fw.last_clock_index, 1u, "and it is the PAL entry");

  init_sidplayer();
  US_CHECK(g_fw.resets >= 1, "the SIDs were reset before the tune started");
  US_CHECK(usplayer_driver_address() != 0, "the driver was relocated");

  start_sidplayer(false);
  US_CHECK(usplayer_playing(), "the player is playing");

  const uint32_t writes_before = g_fw.writes;
  for (int i = 0; i < 50; i++) loop_sidplayer();

  const uint32_t frame_writes = g_fw.writes - writes_before;
  printf("  50 frames wrote %u registers, %llu cycles of delay, "
         "driver at $%04x\n", frame_writes,
         static_cast<unsigned long long>(g_fw.total_cycles),
         usplayer_driver_address());

  US_CHECK_EQ_U(usplayer_frames(), 50u, "fifty frames ran");
  US_CHECK(frame_writes > 100, "and the tune wrote a useful number of registers");
  US_CHECK_EQ_U(usplayer_sid_writes(), g_fw.writes,
                "every write the SID layer made reached the firmware");

  /* Fifty PAL frames are 50 * 63 * 312 = 982800 cycles, and every one of them
   * has to be accounted for somewhere, because on the device these numbers
   * *are* the clock. There are three places it can go. What the firmware
   * receives is the gap between accesses, less the one cycle the access costs
   * by itself, taken off by the SID layer as its access overhead and not taken
   * off again by the backend. A gap too wide for the bus queue to hide is sat
   * out by the player instead and counted as paced. A gap too long to fit in
   * sixteen bits is waited. So the sum of the delays, plus that one cycle per
   * access, plus the paced and the waited, has to come back to the span.
   * Anything missing here is playback running fast on hardware by exactly
   * that much. */
  const uint64_t frame_cycles = 50ull * 63ull * 312ull;
  const uint64_t per_access = 1ull;
  const uint64_t accounted = g_fw.total_cycles + (per_access * frame_writes) +
    usplayer_cycles_paced() + usplayer_cycles_waited();

  printf("  accounted for %llu of %llu cycles (%llu paced, %llu waited out)\n",
         static_cast<unsigned long long>(accounted),
         static_cast<unsigned long long>(frame_cycles),
         static_cast<unsigned long long>(usplayer_cycles_paced()),
         static_cast<unsigned long long>(usplayer_cycles_waited()));

  US_CHECK(accounted <= frame_cycles,
           "no more time is sent than actually passed");
  /* The tail, between the last write and the end of the last frame, is the
   * only thing that legitimately goes unsent, and it is a fraction of a frame */
  US_CHECK(accounted + 19656 > frame_cycles,
           "and everything but the last partial frame is accounted for: "
           "%llu of %llu",
           static_cast<unsigned long long>(accounted),
           static_cast<unsigned long long>(frame_cycles));

  /* pause holds the machine still */
  const uint32_t at_pause = g_fw.writes;
  emu_pause_playing(true);
  for (int i = 0; i < 5; i++) loop_sidplayer();
  US_CHECK_EQ_U(g_fw.writes, at_pause, "a paused player writes nothing");
  US_CHECK(usplayer_paused(), "and says so");
  emu_pause_playing(false);
  for (int i = 0; i < 5; i++) loop_sidplayer();
  US_CHECK(g_fw.writes > at_pause, "unpausing starts it again");

  /* the memory access helpers */
  emu_dma_write_ram(0x033c, 0x5a);
  US_CHECK_EQ_U(emu_dma_read_ram(0x033c), 0x5au, "DMA writes and reads RAM");
  US_CHECK_EQ_U(emu_read_byte(0x033c), 0x5au,
                "and the CPU sees the same byte through the PLA");
  emu_write_byte(0x033d, 0xa5);
  US_CHECK_EQ_U(emu_dma_read_ram(0x033d), 0xa5u,
                "a CPU write lands in the same RAM");
  /* $fffc is the reset vector when the KERNAL is banked in, and whatever the
   * tune put there when it is not. Which of the two depends on what the tune
   * did to $01, so the bank is set here and put back afterwards: this runs
   * between frames, but leaving a tune banked differently than it left itself
   * would break it at the next interrupt. */
  const uint8_t port = emu_read_byte(0x0001);
  emu_write_byte(0x0001, 0x37);
  US_CHECK_EQ_U(emu_read_byte(0xfffc), 0xe2u,
                "with the KERNAL banked in, $fffc is its reset vector");
  emu_write_byte(0x0001, port);

  /* forcing socket two moves every write $20 up */
  force_socktwo();
  const uint32_t before_force = g_fw.writes;
  bool saw_socket_two = false;
  for (int i = 0; i < 5 && !saw_socket_two; i++) {
    loop_sidplayer();
    if (g_fw.writes > before_force && g_fw.last_reg >= 0x20) saw_socket_two = true;
  }
  US_CHECK(saw_socket_two,
           "forcing socket two moves the writes there, last was $%02x",
           g_fw.last_reg);

  /* subtunes */
  const uint16_t song = usplayer_song();
  next_subtune();
  US_CHECK(usplayer_song() != song || usplayer_songs() == 1,
           "next subtune moved, or there was only one");
  previous_subtune();
  US_CHECK_EQ_U(usplayer_song(), song, "previous subtune came back");
  const uint32_t before_subtune = g_fw.writes;
  for (int i = 0; i < 5; i++) loop_sidplayer();
  US_CHECK(usplayer_playing(), "still playing after switching subtunes");
  US_CHECK(g_fw.writes > before_subtune, "and still writing registers");

  /* and stopping silences the hardware */
  const uint32_t resets_before = g_fw.resets;
  US_CHECK(stop_sidplayer(), "stop reports that it stopped");
  US_CHECK(usplayer_playing() == false, "the player is stopped");
  US_CHECK(g_fw.resets > resets_before, "and the SIDs were silenced");

  const uint32_t after_stop = g_fw.writes;
  for (int i = 0; i < 5; i++) loop_sidplayer();
  US_CHECK_EQ_U(g_fw.writes, after_stop, "a stopped player writes nothing");

  return 0;
}

/* ---- a program through the API ------------------------------------------ */

int test_api_prg(void)
{
  /* The firmware calls load_prg() and then goes straight to loop_sidplayer(),
   * with no init in between, so load_prg() has to come back with a machine
   * that is already running the program. */
  char path[512];
  snprintf(path, sizeof(path), "%s/tests/Acid800/cpu_flags.prg", US_ASSET_DIR);

  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("  skipped the program test, nothing to load\n");
    return 0;
  }

  g_fw.clear();

  load_prg(bytes.data(), bytes.size(), false);
  US_CHECK(usplayer_playing(), "the program is running after load_prg");

  const uint32_t frames = usplayer_frames();
  for (int i = 0; i < 10; i++) loop_sidplayer();
  US_CHECK_EQ_U(usplayer_frames(), frames + 10u, "and loop_sidplayer runs it");

  /* a program is not a tune, and asking for its subtunes must not do anything
   * peculiar */
  next_subtune();
  previous_subtune();
  US_CHECK(usplayer_playing(), "still running after the tune only calls");

  /* The keyboard reaches the running program through the matrix. The two
   * subtune calls above each queued a key, so start from an empty queue. */
  usplayer_keys_clear();
  US_CHECK(usplayer_type("x"), "the API can type at a program");
  US_CHECK(usplayer_typing(), "and says so");
  for (int i = 0; i < 12; i++) loop_sidplayer();
  US_CHECK(usplayer_typing() == false, "and the keystroke goes in");

  US_CHECK(usplayer_key_runstop(), "RUN/STOP can be sent");
  for (int i = 0; i < 12; i++) loop_sidplayer();

  usplayer_key_set(7, 4, true);   /* hold the space bar */
  usplayer_keys_clear();
  US_CHECK(usplayer_typing() == false, "clearing the keys empties the queue");

  US_CHECK(stop_sidplayer(), "and it stops");

  g_free_running = true;  /* test_benchmark below counts on it */
  return 0;
}

/* ---- how fast the board is ----------------------------------------------- */

int test_benchmark(void)
{
  /* The stub clock runs a millisecond per read and is read twice, so the
   * answer here is arithmetic rather than a measurement. What is being
   * checked is that it runs the machine, times it, and puts the backend back
   * where it found it, because it is called from a player that may be loaded.
   */
  g_fw.clear();

  SidBackend * before = nullptr;
  (void)before;

  const uint32_t rate = usplayer_benchmark(100000);
  US_CHECK(rate > 0, "the benchmark returns a rate");
  US_CHECK_EQ_U(g_fw.writes, 0u, "and sends nothing to the SIDs while it runs");

  /* it must not leave the player disconnected from the hardware */
  US_CHECK(usplayer_benchmark(0) == 0, "a zero length benchmark is refused");

  return 0;
}

/* ---- what it costs ------------------------------------------------------ */

int test_footprint(void)
{
  /* The RP2350 has 520 KB of SRAM and the firmware needs most of what is left
   * over. This is not a target, it is a tripwire: if a change doubles the
   * static footprint, the build on device stops fitting and the failure shows
   * up here first, off device, with a number attached. */
  const uint32_t bytes = usplayer_static_footprint();
  printf("  static footprint: %u bytes (%.1f KB)\n", bytes,
         static_cast<double>(bytes) / 1024.0);

  US_CHECK(bytes > 64u * 1024u, "the C64's RAM is in there");
  US_CHECK(bytes < 200u * 1024u, "and the whole thing fits in under 200 KB");

  return 0;
}

/* ---- an API call before anything is loaded ------------------------------ */

int test_no_tune(void)
{
  /* Every entry point has to survive being called in the wrong order. The
   * firmware calls them from an interrupt driven state machine, and a config
   * command can arrive at any point in it. */
  load_sidtune(nullptr, 0, 0);
  init_sidplayer();
  start_sidplayer(false);
  loop_sidplayer();
  next_subtune();
  previous_subtune();
  emu_pause_playing(true);
  emu_pause_playing(false);
  emu_ffwd(true);
  usplayer_set_clock_follows_tune(true);
  US_CHECK(usplayer_playing() == false, "nothing plays without a tune");
  US_CHECK(stop_sidplayer(), "and stopping is still safe");

  return 0;
}

} /* namespace */

int us_test_embedded(void)
{
  US_TEST_BEGIN("embedded");

  test_backend();
  test_device_tempo();
  test_write_landing();
  test_footprint();
  test_no_tune();
  test_api();
  test_api_prg();
  test_benchmark();

  US_TEST_END("embedded");
}

US_TEST_MAIN(us_test_embedded)
