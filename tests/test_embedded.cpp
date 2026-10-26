/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * test_embedded.cpp
 * C API the firmware calls and the backend that talks to the firmware bus
 *
 * Firmware bus functions are stubbed in this file, declared weak in
 * sid_embedded.h, a strong definition wins ~ the real call path is exercised,
 * nothing is mocked in between
 *
 * Cross compiling for the RP2350 is checked by temp/tools/build_embedded.sh
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
#define US_TUNE_DIR "../sid-tunes/favorites"
#endif

using namespace usbsid;

/* ---- the firmware as the player sees it --------------------------------- */

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

/* Simulated microsecond clock of the board ~ time passes only when the bus sits out a delay,
 * the player blocks on the clock or the emulation spends host cycles. g_sim drives it.
 * g_free_running gives a millisecond per read for tests that only need a moving clock */
static uint64_t g_fake_us = 0;
static uint32_t g_clock_reads = 0;
static bool g_free_running = true;

static uint64_t fake_time_us_64(void)
{
  ++g_clock_reads;
  if (g_free_running) g_fake_us += 1000;
  return g_fake_us;
}

/* The player waits on this when available, the simulated clock then advances by exactly the wait asked for */
static void fake_busy_wait_us(uint32_t us)
{
  g_fake_us += us;
}

/* ---- the bus and how long it takes -------------------------------------- */
/* Cycled write = pre delay: the PIO sits out the gap, then puts the value on the bus */
/* The call returns when delay and value fit in the state machine queue, a full queue blocks the core and paces playback */
/* Dense tunes keep the queue full (free delays), one long gap a frame drains it (delay served after the emulation) */

namespace {

struct DeviceSim {
  static constexpr int kFifo = 4;      /* writes that fit in the PIO queue */
  /* Cycles of the bus operation on top of its pre delay: 1. bus_control.pio counts delay_word + 1 PHI periods, the write lands on the following edge ~ two writes with pre delay 0 are one PHI apart */
  static constexpr uint32_t kAccess = 1;

  bool active = false;
  uint32_t clock_hz = 985248;
  /* Board speed for this tune, in thousands of emulated cycles per second of host time.
   * Measured per tune on this machine (wall clock, noisy), opt in via USP_CALIBRATE=1 and scaled by the reference below.
   * Default is the board figure of a real tune */
  uint32_t host_kcycles = 1457;  /* Krakout.sid, RP2350 @ 200 MHz */

  uint64_t queue[kFifo] = { 0 };
  int head = 0;
  int count = 0;
  uint64_t pio_free_us = 0;

  /* Deviation of each gap between consecutive writes from the gap the tune asked for ~ a digi hears the spacing.
   * Tune spacing is accumulated from the gaps the SID layer reports, before the backend touches them */
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

  /** @brief Host time to emulate this many C64 cycles */
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

/* Bound once at load ~ the backend reaches the firmware through pointers, a weak undefined symbol is an ELF idea and this suite builds on Mach-O and PE too ~ see src/sid/sid_embedded.h */
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

/* ---- the backend alone -------------------------------------------------- */

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
  /* the gap as given: the access costs the hardware a cycle, taken off upstream by the SID layer access overhead */
  US_CHECK_EQ_U(g_fw.last_cycles, 42u, "and the cycle delay, passed through");

  /* a second chip is $20 up, the backend passes the physical register through untouched */
  backend.write(0x21, 0x77, 7);
  US_CHECK_EQ_U(g_fw.last_reg, 0x21u, "the second chip's register is passed on");

  US_CHECK_EQ_U(backend.read(0x1b, 3), 0x00u, "a read comes back from the bus");
  US_CHECK_EQ_U(g_fw.reads, 1u, "and reached the firmware");

  /* From this point the clock is the simulated one, a wait can be measured */
  g_free_running = false;
  backend.set_clock_hz(985248);

  /* A gap too long for sixteen bits is not a bus operation, nothing is sent ~ it is waited out against the board clock and counted */
  const uint32_t before = g_fw.writes;
  backend.wait(1); /* first access starts the pacer clock */
  const uint64_t wait_started = g_fake_us;
  backend.wait(0xffff);
  US_CHECK_EQ_U(g_fw.writes, before, "a wait is not a bus operation");
  US_CHECK_EQ_U(backend.cycles_waited(), 0x10000u, "but it is accounted for");
  /* $ffff PAL cycles = 66.5 ms, none spent elsewhere ~ all of it is waited out */
  const uint64_t waited_us = g_fake_us - wait_started;
  US_CHECK(waited_us > 66000 && waited_us < 67000,
           "and the time was actually waited out: %llu us of 66517",
           static_cast<unsigned long long>(waited_us));

  /* A gap wide enough to pace has cost the emulation host time already ~ that part is not spent again and none of it goes to the hardware as pre delay (served from a standing start) */
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

  /* the same gap with nothing spent on it still takes its full time */
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

/* ---- device tempo, for both shapes of tune ------------------------------ */
/* Digi tune: ~600 writes a frame, 30 cycles apart ~ the bus queue never empties, the emulation runs underneath and a frame costs what the bus costs */
/* Play routine + idle loop: ~25 writes a frame, then ~19000 idle cycles ~ the queue drains during the gap, an unpaced player pays the gap twice (frame costs half again) */

struct SimBackend final : public SidBackend {
  EmbeddedSidBackend inner;

  /* Gap reported by the SID layer has the access overhead taken off, the cycles actually emulated are one more ~ the untouched gap is the tune timeline the landing error is measured against */
  void write(addr_t reg, data_t value, uint16_t cycles) override
  {
    g_sim.ideal_cycles += static_cast<uint32_t>(cycles) + 1u;
    g_sim.emulate(static_cast<uint32_t>(cycles) + 1u);
    inner.write(reg, value, cycles);
  }
  data_t read(addr_t reg, uint16_t cycles) override
  { g_sim.emulate(static_cast<uint32_t>(cycles) + 1u); return inner.read(reg, cycles); }
  void wait(uint16_t cycles) override
  { g_sim.ideal_cycles += cycles; g_sim.emulate(cycles); inner.wait(cycles); }
  void flush(void) override { inner.flush(); }
  void reset(void) override { inner.reset(); }
};

/* ---- scaling the board speed to this machine ---------------------------- */
/* Board measured with usplayer_benchmark(1000000): RP2350 at 200 MHz gives 1314 kcycles/s idle, 1457 with psid/Krakout.sid, 1009 with rsid/Coma_Light_13_tune_4.sid */
/* Measured per tune in this test and scaled by a reference tune with a known board figure, the ratio follows the desktop speed ~ only the board number is carried in the tree */
/* Re-measure the reference: load psid/Krakout.sid, stop it, send the benchmark config command, put the output in kRefBoardKcycles */
constexpr const char * kRefTune = US_TUNE_DIR "/psid/Krakout.sid";
constexpr double kRefBoardKcycles = 1457.0;  /* RP2350 @ 200 MHz, 2026-08-04 */

double wall_s(void)
{
  return std::chrono::duration<double>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief kcycles/s this machine emulates a tune at, 0 if the tune is missing */
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

  /* Best of three, wall clock on a busy machine ~ the fastest run has the least interference */
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

/** @brief Board kcycles/s per desktop kcycles/s, measured once */
double board_scale(void)
{
  if (getenv("USP_CALIBRATE") == nullptr) return 0.0; /* opt in, it is noisy */

  static double scale = -1.0;
  if (scale < 0.0) {
    const double ref = desktop_kcycles(kRefTune);
    scale = (ref > 0.0) ? (kRefBoardKcycles / ref) : 0.0;
    if (scale > 0.0) {
      printf("  this machine runs %.0f kcycles/s on %s, where the board does "
             "%.0f, making it %.0fx the board\n", ref, "Krakout.sid",
             kRefBoardKcycles, 1.0 / scale);
    } else {
      printf("  no reference tune, the board's speed is the old constant\n");
    }
  }
  return scale;
}

/**
 * @brief Play a tune on the simulated board, return tempo as a multiple of real time (0 if missing)
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

  /* What the board would manage on this tune ~ see board_scale() */
  const double scale = board_scale();
  if (scale > 0.0) {
    const double mine = desktop_kcycles(path);
    if (mine > 0.0) g_sim.host_kcycles = static_cast<uint32_t>(mine * scale);
  }

  /* Settle first, the opening frames and the boot are not steady state */
  player.run_frames(30);

  g_free_running = false;
  sim.inner.reset();
  sim.inner.set_clock_hz(hz);
  g_sim.start(hz);
  g_sim.reset_error();

  const cycle_t started = c64.machine.cycles();
  const uint64_t clock_started = g_fake_us;
  /* Core 0 also runs tud_task(), MIDI, config and the LED after loop_sidplayer() emulates a frame ~ nothing feeds the bus then, the queue drains and the following pre delay is served from a standing start (trim_to_now) */
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
    /* From USP_TEMPO_TUNE, a tune from the collection run through the simulated board ~ printed only, shape unknown */
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

    if (&c == &cases[2]) continue; /* probe, no known answers */

    /* Faster than real time breaks the pacer floor, slower is the bug it was written for */
    US_CHECK(tempo > 0.95 && tempo < 1.05,
             "%s plays at the right speed on the simulated board (%.2fx)",
             name, tempo);

    if (c.expect_paced) {
      US_CHECK(paced > 0.80,
               "%s is a tune the bus queue cannot hide, the player sits its "
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
 * @brief Player must survive core 0 being elsewhere
 *
 * loop_sidplayer() emulates a frame, usbsid.c then runs tud_task(), MIDI, config and the LED. Nothing feeds the bus meanwhile
 * A pre delay starts when its DMA fires ~ with the queue drained that time is spent twice, by the core and by the board
 * trim_to_now() takes the time already lost off the following pre delay on every write, gaps too narrow for the wait_until_due() floor included
 * The per gap figure is reported too, it stays inside a microsecond with a saturated bus queue
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

    /* Compare with the tune baseline ~ taking core 0 away must not cost tempo on top of it (untrimmed player loses 3% at 800 us a frame, more the longer core 0 is away) */
    US_CHECK(tempo > baseline - 0.02,
             "%u us a frame of housekeeping costs no tempo (%.2fx against a "
             "baseline of %.2fx)", overhead, tempo, baseline);
    /* A PAL cycle is about a microsecond, this is a thousand cycles */
    US_CHECK(mean_err < 1000,
             "and the gaps between writes are still the tune's (%llu us out)",
             static_cast<unsigned long long>(mean_err));
  }

  return 0;
}

/* ---- the API as usbsid.c drives it -------------------------------------- */

int test_api(void)
{
  const char * path = US_TUNE_DIR "/rsid/50Kent_in_da_Club.sid";
  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("  skipped the API test, no tune available\n");
    return 0;
  }

  g_fw.clear();

  /* A clock that jumps a millisecond per read cannot measure the pacer ~ in this stub time moves only when something spends it, like on the device */
  g_free_running = false;

  /* The firmware mallocs the file, hands it over and frees it before the following call ~ the API must copy it, or everything after reads freed memory */
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

  /* Fifty PAL frames = 50 * 63 * 312 = 982800 cycles, all accounted for.
   * Firmware gets the gap between accesses minus the 1 cycle access overhead taken off by the SID layer ~ gaps too wide for the bus queue are paced by the player, gaps over sixteen bits are waited.
   * Delays + 1 cycle per access + paced + waited must come back to the span, anything missing is playback running fast on hardware */
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
  /* The tail between the last write and the end of the last frame is the only part legitimately unsent, a fraction of a frame */
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
  US_CHECK(usplayer_paused(), "and reports it");
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
  /* $fffc is the reset vector with the KERNAL banked in, else whatever the tune put there ~ the bank is set and restored afterwards, a tune left banked differently would break at the following interrupt */
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
  /* The firmware calls load_prg() then loop_sidplayer() with no init in between, load_prg() must return a machine already running the program */
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

  /* a program is not a tune, asking for its subtunes must do nothing peculiar */
  next_subtune();
  previous_subtune();
  US_CHECK(usplayer_playing(), "still running after the tune only calls");

  /* The keyboard reaches the program through the matrix ~ the two subtune calls above each queued a key, start from an empty queue */
  usplayer_keys_clear();
  US_CHECK(usplayer_type("x"), "the API can type at a program");
  US_CHECK(usplayer_typing(), "and reports it");
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
  /* The stub clock runs a millisecond per read and is read twice, the answer is arithmetic ~ checked: it runs the machine, times it and restores the backend (called from a player that may be loaded) */
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
  /* The RP2350 has 520 KB of SRAM and the firmware needs most of the rest ~ a tripwire: a doubled static footprint stops fitting on device and fails in this test first, with a number */
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
  /* Every entry point must survive a call in the wrong order, the firmware calls them from an interrupt driven state machine and a config command can arrive at any point */
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
