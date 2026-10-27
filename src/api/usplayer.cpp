/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * usplayer.cpp
 * The C API the firmware calls, on top of Machine and Player
 *
 * Everything in this file is statically allocated, no heap on the device
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

#include "usplayer.h"

#include "keyboard.h"
#include "machine.h"
#include "player.h"
#include "types.h"

/* Backend the writes go to differs per target, firmware or browser tab */
#if defined(WEB) && WEB
#include "sid_web.h"
#else
#include "sid_embedded.h"
#endif

using namespace usbsid;

/* Firmware clock switch through a pointer (undefined weak symbols are ELF only,
 * this builds on Mach-O and PE too), index into the firmware table:
 * { DEFAULT, PAL, NTSC, DREAN, NTSC2 }
 * EMBEDDED takes the real symbol by address, elsewhere it is null (no switch) */
#if defined(EMBEDDED) && EMBEDDED
extern "C" void apply_clockrate(int n_clock, bool suspend_sids);
#endif

extern "C" {
#if defined(EMBEDDED) && EMBEDDED
void (*us_apply_clockrate)(int, bool) = &apply_clockrate;
#else
void (*us_apply_clockrate)(int, bool) = nullptr;
#endif
}

namespace {

/* SID file is at most a 64 KB payload plus header, copied because usbsid.c
 * frees its buffer before init_sidplayer() */
constexpr size_t kMaxTuneBytes = 0x10000 + 0x100;

/* Declaration order is construction order, player needs the machine */
Machine g_machine;
#if defined(WEB) && WEB
/* web_api.cpp drains the same ring, both go through web_backend() */
WebSidBackend & g_backend = web_backend();
#else
EmbeddedSidBackend g_backend;
#endif
Player g_player(g_machine);

data_t g_tune_bytes[kMaxTuneBytes];
size_t g_tune_size = 0;

bool g_initialised = false;   /* Backend attached */
bool g_prepared = false;      /* init_sidplayer() succeeded */
bool g_is_prg = false;
bool g_clock_follows_tune = true;
uint16_t g_song = 0;          /* 1 based, 0 is the file default */

/* Fast forward wanted, set by emu_ffwd() from the config handler on core 0
 * loop_sidplayer() on core 1 applies it to the backend at a frame boundary
 * A level and not an edge, a change between read and apply is never lost */
volatile bool g_ffwd_want = false;

void attach_once(void)
{
  if (g_initialised) return;
  g_machine.set_sid_backend(g_backend);
  g_initialised = true;
}

/**
 * @brief Ask the board for the clock of the tune
 *
 * Firmware takes an index into its clock table (sid_defs.h), unknown rates are left alone
 */
void apply_tune_clock(void)
{
  static const uint32_t kRates[] = { 1000000, 985248, 1022727, 1023440, 1022730 };
  const uint32_t want = vic_timing(g_machine.video_model()).clock_hz;

  /* Backend always needs the rate (cycle gap to microseconds) */
  g_backend.set_clock_hz(want);

  if (!g_clock_follows_tune || us_apply_clockrate == nullptr) return;

  for (int i = 0; i < static_cast<int>(sizeof(kRates) / sizeof(kRates[0])); i++) {
    if (kRates[i] == want) { us_apply_clockrate(i, true); return; }
  }
}

/**
 * @brief End fast forward, the request and the backend
 *
 * Call on the core that runs the player, before set_pacing(false)
 * (switching fast forward off turns pacing on)
 */
void end_ffwd(void)
{
  g_ffwd_want = false;
  g_backend.set_ffwd(false);
}

/**
 * @brief Run a BASIC tune song change (reset, load, RUN) at normal speed and unpaced
 *
 * Boot is silent emulation of seconds, same ordering as init_sidplayer()
 */
template <typename F> bool basic_restart(F change)
{
  end_ffwd();
  g_backend.reset_hardware();
  g_backend.set_pacing(false); /* Boot is not playback */
  const bool ok = change();
  g_backend.set_pacing(true);
  return ok;
}

} /* namespace */

/* ------------------------------------------------------------------------ *
 * Loading and streaming upload
 *
 * load_sidtune()/load_prg() are built on these, the firmware feeds USB
 * packets straight into g_tune_bytes
 * ------------------------------------------------------------------------ */

void usplayer_upload_start(void)
{
  attach_once();
  g_prepared = false;
  g_is_prg = false; /* Set by finish_*() */
  g_tune_size = 0;
  /* Request only, the config handler runs this
   * finish_prg() and init_sidplayer() end the backend side */
  g_ffwd_want = false;
}

bool usplayer_upload_feed(const uint8_t * buf, size_t len)
{
  if (buf == nullptr || len == 0) return true;

  const size_t room = kMaxTuneBytes - g_tune_size;
  const size_t n = (len < room) ? len : room;
  for (size_t i = 0; i < n; i++) g_tune_bytes[g_tune_size + i] = buf[i];
  g_tune_size += n;

  /* False when part of the packet did not fit and was dropped */
  return n == len;
}

void usplayer_upload_finish_tune(char subt)
{
  g_is_prg = false;
  g_prepared = false;

  if (g_tune_size == 0) return;

  /* Firmware counts subtunes from zero, zero is the tune default */
  const int sub = static_cast<int>(static_cast<unsigned char>(subt));
  g_song = (sub == 0) ? 0 : static_cast<uint16_t>(sub + 1);

  if (!g_player.load_sid(g_tune_bytes, g_tune_size, g_song)) {
    g_tune_size = 0;
    return;
  }

  apply_tune_clock();
}

/* Same as usplayer_upload_finish_tune() with a song counted from one, 0 is the file default
 * Song 1 has no spelling in the zero based form above, where 0 means the default */
void usplayer_upload_finish_song(int song)
{
  g_is_prg = false;
  g_prepared = false;

  if (g_tune_size == 0) return;

  g_song = (song < 0) ? 0 : static_cast<uint16_t>(song);

  if (!g_player.load_sid(g_tune_bytes, g_tune_size, g_song)) {
    g_tune_size = 0;
    return;
  }

  apply_tune_clock();
}

/**
 * @brief Finish an upload as a program and start it
 *
 * No separate init step, boot, load and RUN all happen in this call (slow), loop is ignored
 */
void usplayer_upload_finish_prg(bool loop)
{
  (void)loop;
  g_is_prg = true;
  g_prepared = false;

  if (g_tune_size < 3) return;

  if (!g_player.load_prg(g_tune_bytes, g_tune_size)) {
    g_tune_size = 0;
    return;
  }

  g_backend.reset_hardware();
  /* Same clock as a tune */
  apply_tune_clock();

  /* A new program starts at normal speed */
  end_ffwd();
  /* Boot and load are not part of the program timeline, no pacing */
  g_backend.set_pacing(false);
  g_prepared = g_player.init_prg();
  /* No separate start call for a program, pacer starts after init */
  g_backend.set_pacing(true);
}

void load_sidtune(uint8_t * sidfile, int sidfilesize, char subt)
{
  usplayer_upload_start();
  if (sidfile != nullptr && sidfilesize > 0) {
    usplayer_upload_feed(sidfile, static_cast<size_t>(sidfilesize));
  }
  usplayer_upload_finish_tune(subt);
}

void load_prg(uint8_t * binary_, size_t binsize_, bool loop)
{
  usplayer_upload_start();
  if (binary_ != nullptr) {
    usplayer_upload_feed(binary_, binsize_);
  }
  usplayer_upload_finish_prg(loop);
}

/* ------------------------------------------------------------------------ *
 * Playing
 * ------------------------------------------------------------------------ */

void init_sidplayer(void)
{
  attach_once();

  /* A program is already started by load_prg() */
  if (g_is_prg) return;

  g_prepared = false;
  if (g_tune_size == 0) return;

  g_backend.reset_hardware();
  end_ffwd(); /* A new tune starts at normal speed */
  g_backend.set_pacing(false); /* Setup is not playback */
  /* load_sid turns a BASIC RSID into a program */
  g_prepared = g_player.is_prg() ? g_player.init_prg() : g_player.init_tune(g_song);
  g_backend.set_pacing(true);
}

void start_sidplayer(bool loop)
{
  (void)loop; /* Firmware drives the loop, one frame per call */
  if (!g_prepared) return;
  /* Pacer measures the timeline from its first access, init_sidplayer() setup is excluded */
  g_backend.set_pacing(true);
  g_player.start();
}

void loop_sidplayer(void)
{
  /* Apply fast forward between frames, on the core that drives the backend */
  const bool ffwd = g_ffwd_want;
  if (ffwd != g_backend.ffwd()) g_backend.set_ffwd(ffwd);

  /* One frame per call, always returns (VIC keeps counting, also on a jammed tune) */
  g_player.run_frame();
}

bool stop_sidplayer(void)
{
  g_player.stop();
  end_ffwd();
  g_backend.reset_hardware();
  g_prepared = false;
  return true;
}

void next_subtune(void)
{
  if (g_player.is_basic_tune() && g_player.songs() > 1) {
    basic_restart([] { g_player.next_subtune(); return true; });
    return;
  }
  g_player.next_subtune();
}

void previous_subtune(void)
{
  if (g_player.is_basic_tune() && g_player.songs() > 1) {
    basic_restart([] { g_player.previous_subtune(); return true; });
    return;
  }
  g_player.previous_subtune();
}

void force_socktwo(void)
{
  attach_once();
  g_machine.sid().config().force_socket_two = true;
}

/* ------------------------------------------------------------------------ *
 * Control
 * ------------------------------------------------------------------------ */

void emu_pause_playing(bool pause)
{
  g_player.pause(pause);
}

void emu_ffwd(bool enable)
{
  /* Request only, loop_sidplayer() applies it at a frame boundary */
  g_ffwd_want = enable;
}

uint8_t emu_dma_read_ram(uint16_t address)
{
  return g_machine.ram().dma_read(address);
}

void emu_dma_write_ram(uint16_t address, uint8_t data)
{
  g_machine.ram().dma_write(address, data);
}

uint8_t emu_read_byte(uint16_t address)
{
  return g_machine.mmu().read(address);
}

void emu_write_byte(uint16_t address, uint8_t data)
{
  g_machine.mmu().write(address, data);
}

/* ------------------------------------------------------------------------ *
 * Keyboard
 * ------------------------------------------------------------------------ */

bool usplayer_type(const char * text)
{
  attach_once();
  return g_player.type(text);
}

bool usplayer_key_runstop(void)
{
  attach_once();
  return g_player.run_stop();
}

void usplayer_key_set(uint8_t row, uint8_t col, bool pressed)
{
  attach_once();
  g_machine.keyboard().set(KeyPos{ row, col, false }, pressed);
}

void usplayer_keys_clear(void)
{
  attach_once();
  g_machine.keyboard().reset();
}

bool usplayer_typing(void)
{
  return g_player.typing();
}

/* ------------------------------------------------------------------------ *
 * Configuration and state
 * ------------------------------------------------------------------------ */

void usplayer_set_sid_config(uint8_t numsids, uint8_t sids_socket_one,
                             uint8_t sids_socket_two, int8_t fmopl_sid)
{
  attach_once();
  SidConfig & cfg = g_machine.sid().config();
  cfg.sids_socket_one = sids_socket_one;
  cfg.sids_socket_two = sids_socket_two;
  cfg.fmopl_sid = fmopl_sid;

  /* numsids is not applied, the tune decides the chip count */
  (void)numsids;
}

/**
 * @brief The machine, for the software audio backend (C++ only)
 */
namespace usbsid {
Machine & usplayer_machine(void)
{
  attach_once();
  return g_machine;
}
} /* namespace usbsid */

void usplayer_set_clock_follows_tune(bool enable)
{
  g_clock_follows_tune = enable;
}

bool usplayer_playing(void) { return g_player.playing(); }
bool usplayer_paused(void) { return g_player.paused(); }
bool usplayer_loaded(void) { return g_tune_size != 0; }
bool usplayer_is_prg(void) { return g_is_prg; }

uint32_t usplayer_irq_sources(void)
{
  uint32_t out = 0;
  if (g_tune_size == 0) return 0;

  /* Armed = mask bit set and timer started (control register bit 0) */
  struct { Mos6526 & cia; uint32_t ta, tb, tod; } cias[] = {
    { g_machine.cia1(), USP_IRQ_CIA1_TA, USP_IRQ_CIA1_TB, USP_IRQ_CIA1_TOD },
    { g_machine.cia2(), USP_IRQ_CIA2_TA, USP_IRQ_CIA2_TB, USP_IRQ_CIA2_TOD },
  };
  for (auto & c : cias) {
    const data_t imr = c.cia.imr();
    if ((imr & 0x01) != 0 && (c.cia.peek(0x0e) & 0x01) != 0) out |= c.ta;
    if ((imr & 0x02) != 0 && (c.cia.peek(0x0f) & 0x01) != 0) out |= c.tb;
    if ((imr & 0x04) != 0) out |= c.tod;
  }

  /* VIC interrupt enable, bit 0 is the raster compare */
  if ((g_machine.vic().irq_enable() & 0x01) != 0) out |= USP_IRQ_VIC_RASTER;
  return out;
}

uint16_t usplayer_cia_latch(uint8_t cia, uint8_t timer)
{
  Mos6526 & c = (cia == 2) ? g_machine.cia2() : g_machine.cia1();
  return (timer == 1) ? c.latch_b() : c.latch_a();
}

int usplayer_start_mode(void)
{
  if (!g_is_prg) return USP_START_DRIVER;
  /* A BASIC RSID is loaded as a program, told apart by a parsed tune */
  return g_player.tune().valid ? USP_START_BASIC : USP_START_PRG;
}

uint32_t usplayer_clock_hz(void)
{
  return vic_timing(g_machine.video_model()).clock_hz;
}

bool usplayer_is_pal(void)
{
  const VideoModel model = g_machine.video_model();
  return model == VideoModel::Pal6569 || model == VideoModel::PalN6572;
}

/**
 * @brief Frames per second of the tune video model
 *
 * PAL is 50.125 (19656 cycles/frame @ 985248 Hz), drifts a frame every ~8 seconds if rounded
 */
double usplayer_refresh_hz(void)
{
  const VideoModel model = g_machine.video_model();
  return static_cast<double>(vic_timing(model).clock_hz) /
         static_cast<double>(vic_cycles_per_frame(model));
}

uint16_t usplayer_song(void) { return g_player.song(); }
uint16_t usplayer_songs(void) { return g_player.songs(); }
uint32_t usplayer_frames(void) { return g_player.frames_played(); }

bool usplayer_restart_song(uint16_t song)
{
  attach_once();
  if (g_player.is_basic_tune()) {
    return basic_restart([song] { return g_player.restart_song(song); });
  }
  return g_player.restart_song(song);
}

uint32_t usplayer_playtime_ms(void)
{
  const double hz = usplayer_refresh_hz();
  if (hz <= 0.0) return 0;
  return static_cast<uint32_t>(
    (static_cast<double>(g_player.frames_played()) * 1000.0) / hz);
}

void usplayer_set_voice_mute(uint8_t chip, uint8_t voice, bool muted)
{
  attach_once();
  g_machine.sid().set_voice_mute(chip, voice, muted);
}

uint8_t usplayer_voice_mute(uint8_t chip)
{
  return g_machine.sid().voice_mute(chip);
}

void usplayer_set_chip_mute(uint8_t chip, bool muted)
{
  attach_once();
  g_machine.sid().set_chip_mute(chip, muted);
}

uint8_t usplayer_chip_mute(void)
{
  return g_machine.sid().chip_mute();
}
uint16_t usplayer_driver_address(void) { return g_player.driver_address(); }
const char * usplayer_tune_name(void) { return g_player.tune().name; }
const char * usplayer_tune_author(void) { return g_player.tune().author; }
const char * usplayer_tune_released(void) { return g_player.tune().released; }

/* v5 chip count and addresses of the tune, raw and informational
 * The cap of 4 is applied in Player::load_sid */
uint8_t usplayer_sid_count(void) { return g_player.tune().sid_count; }
uint16_t usplayer_sid_addr(uint8_t chip)
{
  const SidFile & t = g_player.tune();
  return (chip < t.sid_count) ? t.sid_addr[chip] : 0;
}
uint8_t usplayer_sid_pan(uint8_t chip)
{
  const SidFile & t = g_player.tune();
  if (chip >= t.sid_count) return static_cast<uint8_t>(SidPan::Center);
  return static_cast<uint8_t>(t.sid_pan[chip]);
}
uint8_t usplayer_pan_layout(void) { return static_cast<uint8_t>(g_player.tune().pan_layout); }
uint8_t usplayer_pan_mode(void) { return static_cast<uint8_t>(g_player.tune().pan_mode); }
bool usplayer_has_fm_opl(void) { return g_player.tune().has_fm_opl; }
bool usplayer_has_embedded_songlengths(void) { return g_player.tune().has_embedded_song_lengths; }
uint32_t usplayer_embedded_songlength_ms(uint16_t song)
{
  return g_player.tune().embedded_song_length_ms(song);
}

uint32_t usplayer_sid_writes(void) { return g_machine.sid().writes(); }
uint64_t usplayer_cycles_waited(void) { return g_backend.cycles_waited(); }
uint64_t usplayer_cycles_paced(void) { return g_backend.cycles_paced(); }

uint32_t usplayer_benchmark(uint32_t cycles)
{
  attach_once();

  if (us_time_us_64 == nullptr || cycles == 0) return 0;

  /* Nothing may reach the SIDs, previous backend is restored after */
  static NullSidBackend measuring;
  SidBackend & previous = g_machine.sid().backend();
  g_machine.set_sid_backend(measuring);

  const uint64_t started = us_time_us_64();
  g_machine.run(cycles);
  const uint64_t elapsed = us_time_us_64() - started;

  g_machine.set_sid_backend(previous);
  g_machine.sid().resync();

  if (elapsed == 0) return 0;
  /* Cycles per microsecond = kcycles per second */
  return static_cast<uint32_t>((static_cast<uint64_t>(cycles) * 1000ull) /
                               elapsed);
}

uint32_t usplayer_static_footprint(void)
{
  return static_cast<uint32_t>(sizeof(g_machine) + sizeof(g_player) +
                               sizeof(g_backend) + sizeof(g_tune_bytes));
}

bool usplayer_needs_roms(void) { return g_player.needs_roms(); }
bool usplayer_stuck_without_roms(void) { return g_player.stuck_without_roms(); }

#if !defined(EMBEDDED) || !EMBEDDED
namespace {
RomStore g_rom_store;
} /* namespace */

void usplayer_set_roms(const uint8_t * basic, const uint8_t * kernal,
                       const uint8_t * chargen)
{
  g_rom_store.set(basic, kernal, chargen);
  g_machine.mmu().roms = g_rom_store.roms();
}

void usplayer_default_roms(void)
{
  g_machine.mmu().roms = Roms{};
}

uint8_t usplayer_roms_present(void)
{
  const Roms & r = g_machine.mmu().roms;
  return static_cast<uint8_t>((r.basic != nullptr ? 1 : 0) |
                              (r.real_kernal ? 2 : 0) |
                              (r.chargen != nullptr ? 4 : 0));
}
#endif
