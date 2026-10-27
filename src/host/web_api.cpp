/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * web_api.cpp
 * The flat C ABI the browser calls
 * Only file in the web build that knows it is a web build
 *
 * Forwards to usplayer.h, the same API the firmware uses.
 * Adds bytes into the heap, the ring the SID writes land
 * in and the numbers a wall clock pump needs
 *
 * Call sequence as used by usplayer-web.js:
 *
 *   const p = usp_alloc(size); HEAPU8.set(bytes, p);
 *   usp_load_sidtune(p, size, subtune);   // or usp_load_prg(p, size)
 *   usp_init_sidplayer();                 // tunes only, programs self start
 *   usp_start();
 *   // then once per frame of wall clock time:
 *   usp_step();                           // one C64 frame
 *   // ... drain the ring, send it, flush at the frame boundary ...
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

#include <cstdint>
#include <cstdlib>

#include "sid_web.h"
#include "sidfile.h"
#include "songlengths.h"
#include "usplayer.h"
#include "sid_residfp.h"
/* For usp_sid_register(): the SID and its register mirror */
#include "machine.h"
#include "mos6581_8580.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

using usbsid::web_backend;

namespace {

/* Clock last asked for, USBSID-Pico index: 0 default, 1 PAL, 2 NTSC, 3 DREAN, 4 NTSC2
 * The page sends it with SET_CLOCK */
int g_clock_id = 1;

} /* namespace */

extern "C" {

/**
 * @brief Record a board clock change request for the page to pass on
 *
 * No board in this process, defined because an undefined weak is not reliably null under wasm
 */
static void web_apply_clockrate(int n_clock, bool suspend_sids)
{
  (void)suspend_sids;
  g_clock_id = n_clock;
}

/**
 * @brief Microseconds since the page loaded
 *
 * emscripten_get_now() is performance.now() (ms)
 * Bound below, Mach-O refuses to link an undefined weak time_us_64
 */
static uint64_t web_time_us_64(void)
{
#ifdef __EMSCRIPTEN__
  return static_cast<uint64_t>(emscripten_get_now() * 1000.0);
#else
  return 0;
#endif
}

/* Bound once, a namespace scope constructor runs at load before any export is callable */
namespace {
struct BindWebClock {
  BindWebClock(void)
  {
    us_time_us_64 = &web_time_us_64;
    us_apply_clockrate = &web_apply_clockrate;
  }
};
const BindWebClock g_bind_web_clock;
} /* namespace */

/* Getting bytes in */

/* Plain malloc for the page to stage a file */
void * usp_alloc(int size) { return malloc(static_cast<size_t>(size)); }
void usp_free(void * p) { free(p); }

/**
 * @brief Load a PSID or RSID from the heap, return 0 if it is not one
 *
 * Subtune counts like the firmware: 0 is the file's default song, 1 is the second
 */
int usp_load_sidtune(uint8_t * buf, int size, int subtune)
{
  load_sidtune(buf, size, static_cast<char>(subtune));
  return usplayer_loaded() ? 1 : 0;
}

/**
 * @brief Load a PSID or RSID from the heap at an explicit song, return 0 if it is not one
 *
 * Song counts from one, 0 is the file's default song. Song 1 has no spelling in
 * usp_load_sidtune(), where 0 means the default
 */
int usp_load_sidtune_song(uint8_t * buf, int size, int song)
{
  usplayer_upload_start();
  if (buf != nullptr && size > 0) {
    usplayer_upload_feed(buf, static_cast<size_t>(size));
  }
  usplayer_upload_finish_song(song);
  return usplayer_loaded() ? 1 : 0;
}

/**
 * @brief Load a program from the heap, return 0 if it is not one
 *
 * No separate init step, boots a machine, loads it and types RUN
 * Slow, about two seconds of emulated time on the calling thread
 */
int usp_load_prg(uint8_t * buf, int size)
{
  load_prg(buf, static_cast<size_t>(size), false);
  return usplayer_loaded() ? 1 : 0;
}

/* Playing */

/** @brief Tunes only: boot, relocate the driver and enter it */
void usp_init_sidplayer(void) { init_sidplayer(); }

/** @brief Start, the page drives the frames */
void usp_start(void) { start_sidplayer(false); }

/** @brief One C64 video frame, also the drain and flush unit for the page */
void usp_step(void) { loop_sidplayer(); }

void usp_stop(void) { stop_sidplayer(); }
void usp_next_subtune(void) { next_subtune(); }
void usp_prev_subtune(void) { previous_subtune(); }
void usp_pause(int pause) { emu_pause_playing(pause != 0); }
void usp_force_socket_two(void) { force_socktwo(); }

/**
 * @brief Tell the player what the board carries
 *
 * Call explicitly (a browser has no driver to ask), without it $df40/$df50
 * reach nothing and an FM/OPL tune plays its SID voices only
 * numsids is ignored. fmopl is 1-based, -1 for a board with no FM/OPL
 */
void usp_set_sid_config(int numsids, int socket_one, int socket_two, int fmopl)
{
  usplayer_set_sid_config(static_cast<uint8_t>(numsids),
                          static_cast<uint8_t>(socket_one),
                          static_cast<uint8_t>(socket_two),
                          static_cast<int8_t>(fmopl));
}

/** @brief RUN/STOP on the keyboard matrix, stops a program */
int usp_key_runstop(void) { return usplayer_key_runstop() ? 1 : 0; }
/** @brief Type a line at the prompt, takes a few frames per character */
int usp_type(const char * text) { return usplayer_type(text) ? 1 : 0; }

/* What the page needs to know */

int usp_is_playing(void) { return usplayer_playing() ? 1 : 0; }
int usp_is_paused(void) { return usplayer_paused() ? 1 : 0; }
int usp_is_prg(void) { return usplayer_is_prg() ? 1 : 0; }
int usp_is_pal(void) { return usplayer_is_pal() ? 1 : 0; }
int usp_clock_id(void) { return g_clock_id; }
uint32_t usp_clock_hz(void) { return usplayer_clock_hz(); }
double usp_refresh_hz(void) { return usplayer_refresh_hz(); }
int usp_song(void) { return usplayer_song(); }
int usp_songs(void) { return usplayer_songs(); }
uint32_t usp_frames(void) { return usplayer_frames(); }
uint32_t usp_sid_writes(void) { return usplayer_sid_writes(); }
/* The tune's strings as UTF-8 for UTF8ToString, the file stores Windows-1252 */
static char g_utf8_name[usbsid::kUtf8FieldSize];
static char g_utf8_author[usbsid::kUtf8FieldSize];
static char g_utf8_released[usbsid::kUtf8FieldSize];

const char * usp_tune_name(void)
{
  return usbsid::win1252_to_utf8(usplayer_tune_name(), g_utf8_name, sizeof(g_utf8_name));
}
const char * usp_tune_author(void)
{
  return usbsid::win1252_to_utf8(usplayer_tune_author(), g_utf8_author, sizeof(g_utf8_author));
}
const char * usp_tune_released(void)
{
  return usbsid::win1252_to_utf8(usplayer_tune_released(), g_utf8_released,
                                 sizeof(g_utf8_released));
}
uint32_t usp_benchmark(uint32_t cycles) { return usplayer_benchmark(cycles); }

/* v5: multi-SID, panning, FM/OPL, embedded song lengths
 * Informational only: usplayer_sid_count() may exceed the 4 chips wired up
 * Panning hint is not rendered (one channel audio), FM/OPL is a flag to show
 * Read after usp_load_sidtune()/usp_init_sidplayer() */
int usp_sid_count(void) { return usplayer_sid_count(); }
int usp_sid_addr(int chip) { return usplayer_sid_addr(static_cast<uint8_t>(chip)); }
/** @brief 0 left, 1 center, 2 right */
int usp_sid_pan(int chip) { return usplayer_sid_pan(static_cast<uint8_t>(chip)); }
/** @brief 0 standard, 1 L/C/R, 2 center first, 3 fully centered */
int usp_pan_layout(void) { return usplayer_pan_layout(); }
/** @brief 0 direct, 1 reverse, 2 group, 3 spread */
int usp_pan_mode(void) { return usplayer_pan_mode(); }
int usp_has_fm_opl(void) { return usplayer_has_fm_opl() ? 1 : 0; }
int usp_has_embedded_songlengths(void) { return usplayer_has_embedded_songlengths() ? 1 : 0; }
int usp_embedded_songlength_ms(int song)
{
  return static_cast<int>(usplayer_embedded_songlength_ms(static_cast<uint16_t>(song)));
}

/** @brief Interrupt sources the tune has armed, see USP_IRQ_* */
uint32_t usp_irq_sources(void) { return usplayer_irq_sources(); }
/** @brief How what is loaded was started, see USP_START_* */
int usp_start_mode(void) { return usplayer_start_mode(); }
/** @brief Where the PSID driver was relocated to, or 0 */
int usp_driver_address(void) { return static_cast<int>(usplayer_driver_address()); }

/* The ring
 * The page reads ring_ptr out of HEAPU8, takes everything between tail and head and writes the new tail back
 * Four bytes an entry: [reg, value, cycles_hi, cycles_lo], the payload of a CYCLED_WRITE */

uint8_t * usbsid_web_ring_ptr(void)
{
  return const_cast<uint8_t *>(web_backend().ring_ptr());
}
uint32_t usbsid_web_ring_entries(void) { return web_backend().ring_entries(); }
uint32_t usbsid_web_ring_head(void) { return web_backend().head(); }
uint32_t usbsid_web_ring_tail(void) { return web_backend().tail(); }
void usbsid_web_ring_set_tail(uint32_t tail) { web_backend().set_tail(tail); }

/** @brief Frame boundaries crossed, the page flushes when this moves */
uint32_t usbsid_web_flush_count(void) { return web_backend().flushes(); }
/** @brief Writes lost to a full ring, non zero means a stall */
uint32_t usbsid_web_drop_count(void) { return web_backend().drops(); }
uint32_t usbsid_web_write_count(void) { return web_backend().writes(); }
/** @brief Silence requests, the page resets the device when this moves */
uint32_t usbsid_web_reset_count(void) { return web_backend().resets(); }
uint32_t usbsid_web_get_clockrate(void) { return web_backend().clock_hz(); }

/**
 * @brief Mute one voice of one SID while the tune keeps playing
 *
 * Gate masking happens in the emulation upstream of the ring: WebUSB, Web Serial and ASID all get it
 *
 * @param chip   1 to 4
 * @param voice  1 to 3
 */
void usp_set_voice_mute(int chip, int voice, int muted)
{
  usplayer_set_voice_mute(static_cast<uint8_t>(chip), static_cast<uint8_t>(voice),
                          muted != 0);
}

/** @brief The mute bits for one chip, bits 0 to 2, chip from 1 */
int usp_voice_mute(int chip)
{
  return usplayer_voice_mute(static_cast<uint8_t>(chip));
}

/**
 * @brief Mute a whole chip, dropping its writes
 *
 * A voice mute only masks gate/sustain, digi playback via $18 (volume) keeps sounding
 *
 * @param chip  1 to 4
 * @param muted non zero to silence
 */
void usp_set_chip_mute(int chip, int muted)
{
  usplayer_set_chip_mute(static_cast<uint8_t>(chip), muted != 0);
}

/** @brief The muted chips, bit 0 for chip one */
int usp_chip_mute(void)
{
  return usplayer_chip_mute();
}

/**
 * @brief Last value written to a SID register, from the emulation's mirror
 *
 * reSIDfp advance() consumes writes internally, nothing reaches the page
 * The mirror (also replayed by ResidFpSidBackend::attach()) is a side effect free array read
 * Real SID registers are write-only except $1b/$1c
 *
 * @param chip 1 to 4
 * @param reg  0 to 31, $d400 relative
 */
int usp_sid_register(int chip, int reg)
{
  const int c = (chip < 1) ? 0 : ((chip - 1) & 0x03);
  const usbsid::data_t physical =
    static_cast<usbsid::data_t>((c << 5) | (reg & 0x1f));
  return usbsid::usplayer_machine().sid().peek(physical);
}

/**
 * @brief One byte of the emulated C64's RAM
 *
 * Plain RAM read, no banking. Reading through the PLA (emu_read_byte()) could
 * acknowledge a CIA interrupt. An I/O address answers with the RAM beneath it
 *
 * @param address 0 to 65535, masked
 */
int usp_read_memory(int address)
{
  return emu_dma_read_ram(static_cast<uint16_t>(address & 0xffff));
}

/**
 * @brief Copy a range of the emulated C64's RAM into a heap buffer, return bytes copied
 *
 * Side effect free like usp_read_memory(), wraps at $ffff
 *
 * @param out     destination in the wasm heap, from usp_alloc
 * @param address first address, masked to 0 to 65535
 * @param len     bytes to copy
 */
int usp_read_memory_block(uint8_t * out, int address, int len)
{
  if (out == nullptr || len <= 0) return 0;
  for (int i = 0; i < len; i++) {
    out[i] = emu_dma_read_ram(static_cast<uint16_t>((address + i) & 0xffff));
  }
  return len;
}

/**
 * @brief A CIA timer's latch, the value it reloads from
 *
 * Estimates a CIA driven tune's call rate: PAL cycles/frame divided by this (~19654 = once a frame)
 * The live counter changes on every read, the latch does not
 *
 * @param cia    1 or 2
 * @param timer  0 for A, 1 for B
 */
int usp_cia_latch(int cia, int timer)
{
  return usplayer_cia_latch(static_cast<uint8_t>(cia), static_cast<uint8_t>(timer));
}

/** @brief How long the current song has been playing, milliseconds, per song */
int usp_playtime_ms(void) { return static_cast<int>(usplayer_playtime_ms()); }

/* Songlengths, for a page that has the database
 * The database is about five megabytes, the page owns the text and hands it over per call
 * No MD5 in the browser (WebCrypto omits it), the hash comes from this code */

/**
 * @brief The database key for a .sid file: the MD5 of the whole file
 *
 * @param out  at least 33 bytes, 32 hex characters and a terminator
 */
void usp_song_md5(const uint8_t * file_bytes, int len, char * out)
{
  if (file_bytes == nullptr || len <= 0 || out == nullptr) return;
  usbsid::songlengths_key(file_bytes, static_cast<size_t>(len), out);
}

/**
 * @brief One song's length in milliseconds, 0 when not in the database
 *
 * @param db    the database text, as the page loaded it
 * @param song  counting from 1
 */
int usp_songlength_ms(const char * db, int db_len, const char * key, int song)
{
  if (db == nullptr || db_len <= 0 || key == nullptr) return 0;
  const usbsid::SongLengths sl =
    usbsid::songlengths_lookup(db, static_cast<size_t>(db_len), key);
  if (!sl.valid) return 0;
  return static_cast<int>(sl.for_song(static_cast<uint16_t>(song)));
}

/** @brief How many songs the database lists for a key, 0 when absent */
int usp_songlength_count(const char * db, int db_len, const char * key)
{
  if (db == nullptr || db_len <= 0 || key == nullptr) return 0;
  const usbsid::SongLengths sl =
    usbsid::songlengths_lookup(db, static_cast<size_t>(db_len), key);
  return sl.valid ? static_cast<int>(sl.count) : 0;
}

/* Software audio: the ResidFpSidBackend of the CLI player compiled to wasm
 * Samples are pulled with usp_audio_take, an AudioWorklet cannot call into this module */

static usbsid::ResidFpSidBackend g_soft;
static bool g_soft_on = false;
/* Stereo mix from the following usp_audio_configure(), see usp_audio_set_stereo() */
static bool g_soft_stereo = false;

/* Panning asked for with usp_audio_set_panning(), reapplied per configure */
static int g_pan_stereo = 0;
static int g_pan_layout = -1;
static int g_pan_mode = -1;
static int g_pan_single = 1;

/**
 * @brief Put every chip where the panning settings say
 *
 * Mono: all Center. One chip: g_pan_single. Several: the v5 tables for the
 * chosen layout and mode, the tune's own where -1
 */
static void apply_panning(void)
{
  const uint8_t n = g_soft.chips();
  usbsid::SidPan pans[usbsid::kMaxSids];
  for (uint8_t c = 0; c < usbsid::kMaxSids; c++) pans[c] = usbsid::SidPan::Center;
  if (g_pan_stereo && n == 1) {
    pans[0] = (g_pan_single == 0) ? usbsid::SidPan::Left
            : (g_pan_single == 2) ? usbsid::SidPan::Right : usbsid::SidPan::Center;
  } else if (g_pan_stereo && n > 1) {
    const int layout = (g_pan_layout < 0) ? usplayer_pan_layout() : g_pan_layout;
    const int mode = (g_pan_mode < 0) ? usplayer_pan_mode() : g_pan_mode;
    usbsid::compute_panning(static_cast<usbsid::SidPanLayout>(layout & 3),
                            static_cast<usbsid::SidPanMode>(mode & 3), n, pans);
  }
  for (uint8_t c = 0; c < n; c++) g_soft.set_pan(static_cast<uint8_t>(c + 1), pans[c]);
}

/**
 * @brief Build the software SID and route the emulation into it, return 1 on success
 *
 * @param chips    1 to 4, normally the tune's own count
 * @param rate     the AudioContext's sampleRate
 * @param quality  0 fast (linear), 1 good (sinc)
 * @param model    0 for 6581, 1 for 8580
 */
int usp_audio_configure(int chips, int rate, int quality, int model)
{
  const uint32_t clock_hz = usplayer_clock_hz();
  if (!g_soft.configure(static_cast<uint8_t>(chips),
                        static_cast<double>(clock_hz),
                        static_cast<unsigned>(rate),
                        quality ? usbsid::SoftSidQuality::Good
                                : usbsid::SoftSidQuality::Fast,
                        model ? usbsid::SoftSidModel::Csg8580
                              : usbsid::SoftSidModel::Mos6581,
                        g_soft_stereo)) {
    g_soft_on = false;
    return 0;
  }
  apply_panning();
  /* attach() sets access_overhead to 0, see sid_residfp.h */
  g_soft.attach(usbsid::usplayer_machine());
  /* ReSIDfp audio needs real reads for OSC3 and ENV3, this overrides SidBackendType */
  usbsid::usplayer_machine().sid().config().real_reads = true;
  g_soft_on = true;
  return 1;
}

/**
 * @brief Hand the machine back to the web backend after usp_audio_configure()
 *
 * Restores what the configure changed (backend, access_overhead, render_idle,
 * real_reads) to the defaults of a hardware web transport (ASID, WebUSB)
 * Safe to call when audio was never configured
 */
void usp_audio_release(void)
{
  if (!g_soft_on) return;
  g_soft.discard();
  usbsid::Machine & machine = usbsid::usplayer_machine();
  machine.set_sid_backend(usbsid::web_backend());
  usbsid::SidConfig & cfg = machine.sid().config();
  cfg.access_overhead = 1;
  cfg.render_idle = false;
  cfg.real_reads = false;
  g_soft_on = false;
}

/** @brief Is the software SID receiving writes */
int usp_audio_enabled(void) { return g_soft_on ? 1 : 0; }

/** @brief Rendered samples waiting to be taken */
int usp_audio_available(void)
{
  return g_soft_on ? static_cast<int>(g_soft.available()) : 0;
}

/**
 * @brief Take up to max rendered frames into a heap buffer, return frames written
 *
 * @param out  an int16 buffer in the wasm heap, from usp_alloc, room for
 *             max * usp_audio_channels() samples, L/R interleaved in stereo
 */
int usp_audio_take(int16_t * out, int max)
{
  if (!g_soft_on || out == nullptr || max <= 0) return 0;
  return static_cast<int>(g_soft.take(out, static_cast<size_t>(max)));
}

/** @brief Drop everything rendered but not taken, on a stop or a seek */
void usp_audio_discard(void) { if (g_soft_on) g_soft.discard(); }

/**
 * @brief Mix to two channels from the following usp_audio_configure() on
 *
 * Off by default, stereo frames are L/R pairs (see usp_audio_take())
 *
 * @param on 1 for stereo, 0 for mono
 */
void usp_audio_set_stereo(int on) { g_soft_stereo = (on != 0); }

/** @brief Samples per frame of usp_audio_take(): 1 mono, 2 stereo */
int usp_audio_channels(void) { return g_soft_on ? static_cast<int>(g_soft.channels()) : 1; }

/**
 * @brief Place the chips in the stereo mix, at once and after each configure
 *
 * Needs a stereo configure (usp_audio_set_stereo())
 *
 * @param stereo  0 puts every chip Center (mono), 1 pans
 * @param layout  v5 panning layout 0-3 for several chips, -1 for the tune's own
 * @param mode    v5 panning mode 0-3 for several chips, -1 for the tune's own
 * @param single  one chip tunes: 0 left, 1 center, 2 right
 */
void usp_audio_set_panning(int stereo, int layout, int mode, int single)
{
  g_pan_stereo = stereo;
  g_pan_layout = layout;
  g_pan_mode = mode;
  g_pan_single = single;
  if (g_soft_on) apply_panning();
}

/** @brief Where a chip sits: 0 left, 1 center, 2 right, chip from 1 */
int usp_audio_pan(int chip)
{
  return static_cast<int>(g_soft.pan(static_cast<uint8_t>(chip)));
}

/**
 * @brief Set the reSIDfp filter of every chip, kept across configures
 *
 * @param enabled    0 bypasses the filter
 * @param curve6581  6581 filter curve, 0 dark to 1 bright, default 0.5
 * @param range6581  6581 filter range, 0 to 1, default 19/39
 * @param curve8580  8580 filter curve, 0 dark to 1 bright, default 0.5
 * @param waveforms  combined waveforms: 0 average, 1 weak, 2 strong
 */
void usp_audio_set_filter(int enabled, double curve6581, double range6581,
                          double curve8580, int waveforms)
{
  usbsid::SoftSidFilter f;
  f.enabled = (enabled != 0);
  f.curve_6581 = curve6581;
  f.range_6581 = range6581;
  f.curve_8580 = curve8580;
  f.waveforms = (waveforms == 1) ? usbsid::SoftSidWaveforms::Weak
              : (waveforms == 2) ? usbsid::SoftSidWaveforms::Strong
                                 : usbsid::SoftSidWaveforms::Average;
  g_soft.set_filter(f);
}

/**
 * @brief Record each voice's output for an oscilloscope, or stop
 *
 * Kept across usp_audio_configure(), see ResidFpSidBackend::set_scope()
 *
 * @param on 1 to record, 0 to stop
 */
void usp_audio_scope(int on) { g_soft.set_scope(on != 0); }

/** @brief Values per scope frame: three per chip, 0 when not configured */
int usp_audio_scope_voices(void)
{
  return g_soft_on ? static_cast<int>(g_soft.scope_voices()) : 0;
}

/**
 * @brief Take scope frames in step with usp_audio_take(), return frames written
 *
 * Take as many frames as usp_audio_take() returned, right after it
 *
 * @param out     an int16 buffer in the wasm heap, max * voices long
 * @param max     the most frames to take
 * @param voices  values per frame to keep, the first ones (chip 1 voices
 *                1-3 first)
 */
int usp_audio_scope_take(int16_t * out, int max, int voices)
{
  if (!g_soft_on || out == nullptr || max <= 0 || voices <= 0) return 0;
  return static_cast<int>(g_soft.take_scope(out, static_cast<size_t>(max),
                                            static_cast<unsigned>(voices)));
}

/**
 * @brief Run the emulation without synthesising anything
 *
 * Runs through a silent lead-in at ~1/10th the cost per frame
 * Writes still land, chips are not clocked, state is current when this goes back on
 *
 * @param on 1 to synthesise, 0 to run silently
 */
void usp_audio_render(int on) { g_soft.set_render(on != 0); }

/** @brief Is the synthesis running */
int usp_audio_rendering(void) { return g_soft.rendering() ? 1 : 0; }

/**
 * @brief Play the software audio faster or slower, pitch following, return the multiplier in effect
 *
 * Changes the rate reSIDfp and the FM/OPL resample to: fewer samples per
 * emulated frame above 1, more below, chip state is kept
 * Callable before usp_audio_configure(), which keeps it
 *
 * @param mult speed multiplier, 1 is normal, clamped to 0.1 to 8 and the
 *             resampler's rate range
 */
double usp_audio_set_speed(double mult) { return g_soft.set_speed(mult); }

/** @brief The software audio speed multiplier in effect. */
double usp_audio_speed(void) { return g_soft.speed(); }


/**
 * @brief Samples that came out past full scale and were clamped
 *
 * Chips sum with no headroom, N chips can reach N times full scale and clip as a ripple/buzz
 */
int usp_audio_clipped(void)
{
  return g_soft_on ? static_cast<int>(g_soft.clipped()) : 0;
}

/** @brief Writes to $df40/$df50 reSIDfp cannot voice, for a page to report */
int usp_audio_fm_writes(void)
{
  return g_soft_on ? static_cast<int>(g_soft.fm_writes()) : 0;
}

/**
 * @brief How loud the reSIDfp (SID) side of the mix is, independent of FM
 *
 * Multiplier applied before the two chips are summed, a post mix GainNode
 * cannot balance one against the other
 * Callable any time, the backend remembers it (see ResidFpSidBackend::set_sid_gain(), CLI -rv/--resid-volume)
 *
 * @param percent 100 is unity, 0 silences the SID side, 300 is the CLI's
 *                suggested ceiling (not enforced)
 */
void usp_audio_set_sid_volume(int percent)
{
  g_soft.set_sid_gain(static_cast<float>(percent) / 100.0f);
}

/** @brief The SID side's current multiplier, 100 = unity */
int usp_audio_sid_volume(void)
{
  return static_cast<int>(g_soft.sid_gain() * 100.0f);
}

/**
 * @brief How loud the FM/OPL side of the mix is, independent of the SID side
 *
 * Like usp_audio_set_sid_volume(), default 50 as the OPL is the louder chip (see OplChip::set_gain())
 *
 * @param percent 100 is unity, 0 silences the FM side
 */
void usp_audio_set_fm_volume(int percent)
{
  g_soft.set_fm_gain(static_cast<float>(percent) / 100.0f);
}

/** @brief The FM side's current multiplier, 100 = unity */
int usp_audio_fm_volume(void)
{
  return static_cast<int>(g_soft.fm_gain() * 100.0f);
}

} /* extern "C" */
