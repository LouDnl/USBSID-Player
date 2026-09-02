/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * usplayer.cpp
 * The C API the firmware calls, on top of Machine and Player.
 *
 * Everything here is statically allocated. There is no heap on the device
 * worth the name, the machine is needed for the whole life of the firmware
 * anyway, and a static object is one less thing that can fail at three in the
 * morning halfway through a tune.
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

#include "usplayer.h"

#include "keyboard.h"
#include "machine.h"
#include "player.h"
#include "types.h"

/* Which backend the writes go to is the one thing that differs per target.
 * Everything below is written against the pair of them, so the names and the
 * call order are the same whether this is the firmware or a browser tab. */
#if defined(WEB) && WEB
#include "sid_web.h"
#else
#include "sid_embedded.h"
#endif

using namespace usbsid;

/* The firmware's clock switch, reached through a pointer for the same reason as
 * the bus functions in sid_embedded.h: an undefined weak symbol is an ELF idea
 * and this builds on Mach-O and PE as well. The index is into the firmware's own
 * table: { DEFAULT, PAL, NTSC, DREAN, NTSC2 }.
 *
 * Under EMBEDDED the real symbol is there and is taken by address, so the
 * firmware needs no edit; everywhere else it starts null and the clock is simply
 * not switched, which is what a null weak did. */
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

/* A SID file is at most a 64 KB payload plus its header. Copying it is not a
 * luxury: usbsid.c frees the buffer it handed us before init_sidplayer() is
 * called, and the payload has to survive until the driver is installed and,
 * for a subtune restart, well past that. */
constexpr size_t kMaxTuneBytes = 0x10000 + 0x100;

/* Declaration order is construction order, and the player needs the machine */
Machine g_machine;
#if defined(WEB) && WEB
/* The exports in src/host/web_api.cpp drain the same ring the player fills,
 * and they cannot see this file's statics, so both go through web_backend(). */
WebSidBackend & g_backend = web_backend();
#else
EmbeddedSidBackend g_backend;
#endif
Player g_player(g_machine);

data_t g_tune_bytes[kMaxTuneBytes];
size_t g_tune_size = 0;

bool g_initialised = false;   /* the backend has been attached */
bool g_prepared = false;      /* init_sidplayer() has run and succeeded */
bool g_is_prg = false;
bool g_clock_follows_tune = true;
uint16_t g_song = 0;          /* 1 based, 0 means the file's default */

void attach_once(void)
{
  if (g_initialised) return;
  g_machine.set_sid_backend(g_backend);
  g_initialised = true;
}

/**
 * @brief Ask the board for the clock the tune was written for.
 *
 * The firmware indexes its clock table rather than taking a frequency, and
 * the table is the one in sid_defs.h. Anything not in it is left alone: a
 * wrong clock is worse than the current one.
 */
void apply_tune_clock(void)
{
  static const uint32_t kRates[] = { 1000000, 985248, 1022727, 1023440, 1022730 };
  const uint32_t want = vic_timing(g_machine.video_model()).clock_hz;

  /* The backend needs the rate whether or not the board is allowed to follow
   * it: it is what turns a cycle gap into microseconds to wait out. */
  g_backend.set_clock_hz(want);

  if (!g_clock_follows_tune || us_apply_clockrate == nullptr) return;

  for (int i = 0; i < static_cast<int>(sizeof(kRates) / sizeof(kRates[0])); i++) {
    if (kRates[i] == want) { us_apply_clockrate(i, true); return; }
  }
}

} /* namespace */

/* ------------------------------------------------------------------------ *
 * loading and streaming upload
 *
 * The three primitives load_sidtune()/load_prg() are themselves now built
 * on: a caller with the whole file in one buffer already (load_sidtune/
 * load_prg) copies it in with a single usplayer_upload_feed() instead of a
 * private loop, and a caller receiving it in USB packets (the firmware) can
 * feed it straight into g_tune_bytes as each packet arrives, with no
 * firmware-owned staging buffer at all.
 * ------------------------------------------------------------------------ */

void usplayer_upload_start(void)
{
  attach_once();
  g_prepared = false;
  g_is_prg = false; /* set for real by whichever finish_*() is eventually called */
  g_tune_size = 0;
}

bool usplayer_upload_feed(const uint8_t * buf, size_t len)
{
  if (buf == nullptr || len == 0) return true;

  const size_t room = kMaxTuneBytes - g_tune_size;
  const size_t n = (len < room) ? len : room;
  for (size_t i = 0; i < n; i++) g_tune_bytes[g_tune_size + i] = buf[i];
  g_tune_size += n;

  /* false means some of this packet did not fit and was dropped, the same
   * truncation load_sidtune()/load_prg() already did for an oversized
   * single buffer, just discoverable per packet instead of silent. */
  return n == len;
}

void usplayer_upload_finish_tune(char subt)
{
  g_is_prg = false;
  g_prepared = false;

  if (g_tune_size == 0) return;

  /* The firmware counts subtunes from zero and uses zero for "the tune's own
   * default", which is how the old player read it too. */
  const int sub = static_cast<int>(static_cast<unsigned char>(subt));
  g_song = (sub == 0) ? 0 : static_cast<uint16_t>(sub + 1);

  if (!g_player.load_sid(g_tune_bytes, g_tune_size, g_song)) {
    g_tune_size = 0;
    return;
  }

  apply_tune_clock();
}

/**
 * @brief Finish an upload as a program and start it.
 *
 * Unlike a tune, a program has no separate init step in this API: the firmware
 * calls this and then goes straight to loop_sidplayer(). So the boot, the
 * load and the RUN all happen here, which makes this the slow call. The `loop`
 * argument is what the old player restarted a finished program with; nothing
 * here decides that a program has finished, so it is accepted and ignored.
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
  /* The same clock a tune gets. This used to tell the backend the rate and
   * stop there, so the board kept whatever clock the last tune left it on and
   * a program ran at the wrong speed, on hardware, for as long as it played. */
  apply_tune_clock();

  /* Booting the machine and loading the program is not part of the program's
   * timeline. Pacing it would sit out every gap in it in real time, which for
   * a PRG is nearly three seconds of waiting for work that takes two. */
  g_backend.set_pacing(false);
  g_prepared = g_player.init_prg();
  /* A program has no separate start call, so the pacer starts here instead */
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
 * playing
 * ------------------------------------------------------------------------ */

void init_sidplayer(void)
{
  attach_once();

  /* A program was already booted, loaded and started by load_prg(), because
   * that is the order the firmware calls things in. Nothing to do. */
  if (g_is_prg) return;

  g_prepared = false;
  if (g_tune_size == 0) return;

  g_backend.reset_hardware();
  g_backend.set_pacing(false); /* see load_prg: setup is not playback */
  /* load_sid turns a BASIC RSID into a program, since that is what it is. */
  g_prepared = g_player.is_prg() ? g_player.init_prg() : g_player.init_tune(g_song);
  g_backend.set_pacing(true);
}

void start_sidplayer(bool loop)
{
  (void)loop; /* the firmware drives the loop itself, one frame per call */
  if (!g_prepared) return;
  /* The pacer measures the tune's timeline from its first access. Start it
   * here, so the setup that init_sidplayer() just ran is not part of it. */
  g_backend.set_pacing(true);
  g_player.start();
}

void loop_sidplayer(void)
{
  /* One frame per call. The firmware's core 1 loop checks its own flags
   * between calls, so this has to return promptly and cannot be the whole of
   * playback. A frame always ends: the VIC keeps counting whatever the CPU
   * does, so even a tune that has jammed comes back from here. */
  g_player.run_frame();
}

bool stop_sidplayer(void)
{
  g_player.stop();
  g_backend.reset_hardware();
  g_prepared = false;
  return true;
}

void next_subtune(void)
{
  g_player.next_subtune();
}

void previous_subtune(void)
{
  g_player.previous_subtune();
}

void force_socktwo(void)
{
  attach_once();
  g_machine.sid().config().force_socket_two = true;
}

/* ------------------------------------------------------------------------ *
 * control
 * ------------------------------------------------------------------------ */

void emu_pause_playing(bool pause)
{
  g_player.pause(pause);
}

void emu_ffwd(bool enable)
{
  /* Nothing to do yet: on the device the SID writes themselves are the pacing,
   * so "as fast as possible" is what already happens. It becomes real when the
   * pacer arrives for the embedded build. */
  (void)enable;
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
 * the keyboard
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
 * configuration and state
 * ------------------------------------------------------------------------ */

void usplayer_set_sid_config(uint8_t numsids, uint8_t sids_socket_one,
                             uint8_t sids_socket_two, int8_t fmopl_sid)
{
  attach_once();
  SidConfig & cfg = g_machine.sid().config();
  cfg.sids_socket_one = sids_socket_one;
  cfg.sids_socket_two = sids_socket_two;
  cfg.fmopl_sid = fmopl_sid;

  /* `numsids` is not applied. How many chips the player emulates is the
   * tune's business and nothing else's: it decides which addresses the
   * emulation decodes, and every one of them ends up as a register write on
   * the same bus whatever the board is carrying. Clamping it to the board's
   * count only threw away the second chip's writes of a two SID tune. */
  (void)numsids;
}

/**
 * @brief The machine itself, for a frontend that needs to reach past this API.
 *
 * C++ only and deliberately not part of the C surface. The software audio
 * backend has to be attached to the same machine the player is stepping, and it
 * is a C++ object with a C++ constructor, so there is nothing to be gained by
 * pretending otherwise. `attach_once()` first, so a caller cannot get a machine
 * that has not been set up.
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

  /* A CIA source is armed when the interrupt mask allows it and, for the two
   * timers, when the timer is actually started. A mask bit on its own says
   * nothing: plenty of init routines set the mask and never start the timer,
   * and reporting that as "this tune runs on Timer B" would be wrong. Control
   * register bit 0 is the start bit for both timers. */
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

  /* The VIC's own interrupt enable, bit 0 being the raster compare. */
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
  /* A BASIC RSID is loaded as a program, so the two are told apart by whether
   * a tune was parsed at all. */
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
 * @brief Frames per second of the video model the tune asked for.
 *
 * PAL is 50.125, not 50: a frame is 19656 cycles of a 985248 Hz clock. A host
 * pacing playback against its own clock has to use this rather than a round
 * number, or it drifts by a frame every eight seconds.
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
uint32_t usplayer_sid_writes(void) { return g_machine.sid().writes(); }
uint64_t usplayer_cycles_waited(void) { return g_backend.cycles_waited(); }
uint64_t usplayer_cycles_paced(void) { return g_backend.cycles_paced(); }

uint32_t usplayer_benchmark(uint32_t cycles)
{
  attach_once();

  if (us_time_us_64 == nullptr || cycles == 0) return 0;

  /* Nothing should reach the SIDs while this runs, and whatever was playing
   * should find its backend where it left it. */
  static NullSidBackend measuring;
  SidBackend & previous = g_machine.sid().backend();
  g_machine.set_sid_backend(measuring);

  const uint64_t started = us_time_us_64();
  g_machine.run(cycles);
  const uint64_t elapsed = us_time_us_64() - started;

  g_machine.set_sid_backend(previous);
  g_machine.sid().resync();

  if (elapsed == 0) return 0;
  /* cycles per microsecond is cycles per second in thousands */
  return static_cast<uint32_t>((static_cast<uint64_t>(cycles) * 1000ull) /
                               elapsed);
}

uint32_t usplayer_static_footprint(void)
{
  return static_cast<uint32_t>(sizeof(g_machine) + sizeof(g_player) +
                               sizeof(g_backend) + sizeof(g_tune_bytes));
}
