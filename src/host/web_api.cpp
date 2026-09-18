/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * web_api.cpp
 * The flat C ABI the browser calls, and the only file in the web build that
 * knows it is a web build.
 *
 * Everything here forwards to src/api/usplayer.h, the same API the firmware
 * uses, because a browser and a Pico want the identical thing: hand over some
 * bytes, run one frame at a time, and be told what came out. What the browser
 * needs on top is a way to get bytes *into* the heap, the ring the SID writes
 * land in, and the numbers a wall clock pump needs to pace itself.
 *
 * The intended call sequence, which usplayer-web.js follows:
 *
 *   const p = usp_alloc(size); HEAPU8.set(bytes, p);
 *   usp_load_sidtune(p, size, subtune);   // or usp_load_prg(p, size)
 *   usp_init_sidplayer();                 // tunes only, programs self start
 *   usp_start();
 *   // then once per frame of wall clock time:
 *   usp_step();                           // one C64 frame
 *   // ... drain the ring, send it, flush at the frame boundary ...
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

#include <cstdint>
#include <cstdlib>

#include "sid_web.h"
#include "songlengths.h"
#include "usplayer.h"
#include "sid_residfp.h"
/* For usp_sid_register(): the machine's SID and its register mirror. */
#include "machine.h"
#include "mos6581_8580.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

using usbsid::web_backend;

namespace {

/* The clock the player last asked the board for, as USBSID-Pico's own index:
 * 0 default, 1 PAL, 2 NTSC, 3 DREAN, 4 NTSC2. The page sends it on with
 * SET_CLOCK, so the device's SID clock matches the tune whatever it was set to
 * before. */
int g_clock_id = 1;

} /* namespace */

extern "C" {

/**
 * @brief The player asking the board to change its clock.
 *
 * No board in this process, so the request is just recorded for the page to
 * pass on. Defined rather than left weak: an undefined weak under wasm is
 * not reliably null.
 */
static void web_apply_clockrate(int n_clock, bool suspend_sids)
{
  (void)suspend_sids;
  g_clock_id = n_clock;
}

/**
 * @brief Microseconds since the page loaded.
 *
 * `emscripten_get_now()` is `performance.now()` (ms, sub-microsecond
 * precision). Bound below rather than a weak `time_us_64` definition:
 * undefined weak is an ELF idea, Mach-O refuses to link it (broke
 * `test_web` on macOS).
 */
static uint64_t web_time_us_64(void)
{
#ifdef __EMSCRIPTEN__
  return static_cast<uint64_t>(emscripten_get_now() * 1000.0);
#else
  return 0;
#endif
}

/* Bound once, before anything can ask for the time. A namespace scope object's
 * constructor runs at load, which on the web is before any export is callable. */
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

/* ------------------------------------------------------------------------ *
 * getting bytes in
 * ------------------------------------------------------------------------ */

/* Plain malloc, exported so the page can stage a file without needing the
 * whole Emscripten allocator surface exposed. */
void * usp_alloc(int size) { return malloc(static_cast<size_t>(size)); }
void usp_free(void * p) { free(p); }

/**
 * @brief Load a PSID or RSID from the heap. Returns 0 if it is not one.
 *
 * The subtune is counted the way the firmware counts it and the way the file
 * dialog does: zero means the file's own default song, 1 is the second.
 */
int usp_load_sidtune(uint8_t * buf, int size, int subtune)
{
  load_sidtune(buf, size, static_cast<char>(subtune));
  return usplayer_loaded() ? 1 : 0;
}

/**
 * @brief Load a program from the heap. Returns 0 if it is not one.
 *
 * A program has no separate init step: this boots a machine, loads it and
 * types RUN, so it comes back ready to be stepped. That is the slow call,
 * about two seconds of emulated time, and it happens on the calling thread.
 */
int usp_load_prg(uint8_t * buf, int size)
{
  load_prg(buf, static_cast<size_t>(size), false);
  return usplayer_loaded() ? 1 : 0;
}

/* ------------------------------------------------------------------------ *
 * playing
 * ------------------------------------------------------------------------ */

/** @brief Tunes only: boot, relocate the driver and enter it. */
void usp_init_sidplayer(void) { init_sidplayer(); }

/** @brief Begin. The page drives the frames from here on. */
void usp_start(void) { start_sidplayer(false); }

/** @brief One C64 video frame, and one drain and flush unit for the page. */
void usp_step(void) { loop_sidplayer(); }

void usp_stop(void) { stop_sidplayer(); }
void usp_next_subtune(void) { next_subtune(); }
void usp_prev_subtune(void) { previous_subtune(); }
void usp_pause(int pause) { emu_pause_playing(pause != 0); }
void usp_force_socket_two(void) { force_socktwo(); }

/**
 * @brief Tell the player what the board is carrying.
 *
 * The page must call this explicitly (a browser has no "the driver already
 * asked" equivalent): without it, `$df40`/`$df50` reach nothing and an
 * FM/OPL tune plays its SID voices only. `numsids` is accepted and ignored,
 * how many chips the emulation decodes is the tune's own business. `fmopl`
 * is 1-based, -1 for a board with no FM/OPL.
 */
void usp_set_sid_config(int numsids, int socket_one, int socket_two, int fmopl)
{
  usplayer_set_sid_config(static_cast<uint8_t>(numsids),
                          static_cast<uint8_t>(socket_one),
                          static_cast<uint8_t>(socket_two),
                          static_cast<int8_t>(fmopl));
}

/** @brief RUN/STOP on the keyboard matrix, which is how a program is stopped. */
int usp_key_runstop(void) { return usplayer_key_runstop() ? 1 : 0; }
/** @brief Type a line at the prompt. Takes a few frames per character. */
int usp_type(const char * text) { return usplayer_type(text) ? 1 : 0; }

/* ------------------------------------------------------------------------ *
 * what the page needs to know
 * ------------------------------------------------------------------------ */

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
/* The tune's own strings, re-encoded as UTF-8. PSID header name/author/
 * release fields are ISO 8859-1; the page reads them with `UTF8ToString`,
 * so a byte like 0xFC (u-umlaut) produced a replacement character/question
 * mark without this. ISO 8859-1 maps directly to Unicode's first 256 code
 * points, so it's the textbook 2-byte encoding, no table needed. Static
 * buffers sized 192 = 2x kMetaFieldSize (96) + terminator, the worst case. */
static const char * latin1_to_utf8(const char * src, char * dst, size_t cap)
{
  size_t o = 0;
  for (const unsigned char * p = (const unsigned char *)src; *p != '\0'; ++p) {
    if (*p < 0x80) {
      if (o + 2 > cap) break;
      dst[o++] = (char)*p;
    } else {
      if (o + 3 > cap) break;
      dst[o++] = (char)(0xc0 | (*p >> 6));
      dst[o++] = (char)(0x80 | (*p & 0x3f));
    }
  }
  dst[o] = '\0';
  return dst;
}

static char g_utf8_name[192];
static char g_utf8_author[192];
static char g_utf8_released[192];

const char * usp_tune_name(void)
{
  return latin1_to_utf8(usplayer_tune_name(), g_utf8_name, sizeof(g_utf8_name));
}
const char * usp_tune_author(void)
{
  return latin1_to_utf8(usplayer_tune_author(), g_utf8_author, sizeof(g_utf8_author));
}
const char * usp_tune_released(void)
{
  return latin1_to_utf8(usplayer_tune_released(), g_utf8_released, sizeof(g_utf8_released));
}
uint32_t usp_benchmark(uint32_t cycles) { return usplayer_benchmark(cycles); }

/* ---- v5: multi-SID, panning, FM/OPL, embedded song lengths ---------------
 *
 * All informational: usplayer_sid_count() may be more than this player wires
 * up (4, everywhere), the panning hint is not rendered (the audio path stays
 * one channel, see sid_residfp.cpp), and FM/OPL is a flag to show, not a
 * request this file acts on. A page that wants to display any of this reads
 * it after usp_load_sidtune()/usp_init_sidplayer().
 */
int usp_sid_count(void) { return usplayer_sid_count(); }
int usp_sid_addr(int chip) { return usplayer_sid_addr(static_cast<uint8_t>(chip)); }
/** @brief 0 left, 1 center, 2 right. */
int usp_sid_pan(int chip) { return usplayer_sid_pan(static_cast<uint8_t>(chip)); }
/** @brief 0 standard, 1 L/C/R, 2 center first, 3 fully centered. */
int usp_pan_layout(void) { return usplayer_pan_layout(); }
/** @brief 0 direct, 1 reverse, 2 group, 3 spread. */
int usp_pan_mode(void) { return usplayer_pan_mode(); }
int usp_has_fm_opl(void) { return usplayer_has_fm_opl() ? 1 : 0; }
int usp_has_embedded_songlengths(void) { return usplayer_has_embedded_songlengths() ? 1 : 0; }
int usp_embedded_songlength_ms(int song)
{
  return static_cast<int>(usplayer_embedded_songlength_ms(static_cast<uint16_t>(song)));
}

/** @brief Which interrupt sources the tune has armed. See USP_IRQ_*. */
uint32_t usp_irq_sources(void) { return usplayer_irq_sources(); }
/** @brief How what is loaded was started. See USP_START_*. */
int usp_start_mode(void) { return usplayer_start_mode(); }
/** @brief Where the PSID driver was relocated to, or 0. */
int usp_driver_address(void) { return static_cast<int>(usplayer_driver_address()); }

/* ------------------------------------------------------------------------ *
 * the ring
 *
 * The page reads `ring_ptr` out of HEAPU8, takes everything between tail and
 * head, and writes the new tail back. Four bytes an entry,
 * [reg, value, cycles_hi, cycles_lo], which is already the payload of a
 * CYCLED_WRITE, so a run of them can go into a packet without unpacking.
 * ------------------------------------------------------------------------ */

uint8_t * usbsid_web_ring_ptr(void)
{
  return const_cast<uint8_t *>(web_backend().ring_ptr());
}
uint32_t usbsid_web_ring_entries(void) { return web_backend().ring_entries(); }
uint32_t usbsid_web_ring_head(void) { return web_backend().head(); }
uint32_t usbsid_web_ring_tail(void) { return web_backend().tail(); }
void usbsid_web_ring_set_tail(uint32_t tail) { web_backend().set_tail(tail); }

/** @brief Frame boundaries crossed. The page flushes when this moves. */
uint32_t usbsid_web_flush_count(void) { return web_backend().flushes(); }
/** @brief Writes lost to a full ring. Anything but zero means a stall. */
uint32_t usbsid_web_drop_count(void) { return web_backend().drops(); }
uint32_t usbsid_web_write_count(void) { return web_backend().writes(); }
/** @brief Silence requests. The page resets the device when this moves. */
uint32_t usbsid_web_reset_count(void) { return web_backend().resets(); }
uint32_t usbsid_web_get_clockrate(void) { return web_backend().clock_hz(); }

/**
 * @brief Hold one voice of one SID silent while the tune keeps playing.
 *
 * @param chip   1 to 4
 * @param voice  1 to 3
 *
 * One call for all three transports. The gate masking happens in the emulation,
 * upstream of the ring the page drains, so WebUSB, Web Serial and ASID all get it
 * without knowing anything about it.
 */
void usp_set_voice_mute(int chip, int voice, int muted)
{
  usplayer_set_voice_mute(static_cast<uint8_t>(chip), static_cast<uint8_t>(voice),
                          muted != 0);
}

/** @brief The mute bits for one chip, bits 0 to 2. Chip counts from 1. */
int usp_voice_mute(int chip)
{
  return usplayer_voice_mute(static_cast<uint8_t>(chip));
}

/**
 * @brief Hold a whole chip silent, dropping its writes.
 *
 * @param chip  1 to 4
 * @param muted non zero to silence
 *
 * Not three voice mutes: a voice mute only masks gate/sustain, letting
 * everything else through, so digi playback via $18 (volume) keeps sounding
 * regardless. A chip mute drops the chip's writes outright.
 */
void usp_set_chip_mute(int chip, int muted)
{
  usplayer_set_chip_mute(static_cast<uint8_t>(chip), muted != 0);
}

/** @brief The muted chips, bit 0 for chip one. */
int usp_chip_mute(void)
{
  return usplayer_chip_mute();
}

/**
 * @brief The last value written to a SID register, from the emulation's mirror.
 *
 * In software audio, reSIDfp's `advance()` consumes writes internally with
 * nothing reaching the page, so watching writes go by doesn't work there.
 * The emulation's own write mirror (also what `ResidFpSidBackend::attach()`
 * replays into a rebuilt chip) is a side-effect-free array read; real SID
 * registers are write-only except $1b/$1c.
 *
 * @param chip 1 to 4
 * @param reg  0 to 31, so $d400 relative
 */
int usp_sid_register(int chip, int reg)
{
  const int c = (chip < 1) ? 0 : ((chip - 1) & 0x03);
  const usbsid::data_t physical =
    static_cast<usbsid::data_t>((c << 5) | (reg & 0x1f));
  return usbsid::usplayer_machine().sid().peek(physical);
}

/**
 * @brief One byte of the emulated C64's RAM.
 *
 * Straight RAM read, no banking, no side effects - reading through the PLA
 * instead (`emu_read_byte()`) could hit a CIA's interrupt register and
 * acknowledge a pending interrupt, quietly breaking playback just from a
 * page redrawing a memory view. An I/O address answers with the RAM
 * beneath it instead.
 *
 * @param address 0 to 65535, masked
 */
int usp_read_memory(int address)
{
  return emu_dma_read_ram(static_cast<uint16_t>(address & 0xffff));
}

/**
 * @brief A CIA timer's latch, the value it reloads from.
 *
 * For estimating a CIA-driven tune's call rate: PAL cycles/frame divided by
 * this (~19654 = once a frame). The latch, not the live counter, which
 * changes every time it's read; latch reads are also side-effect free
 * unlike reading $dc04 directly.
 *
 * @param cia    1 or 2
 * @param timer  0 for A, 1 for B
 */
int usp_cia_latch(int cia, int timer)
{
  return usplayer_cia_latch(static_cast<uint8_t>(cia), static_cast<uint8_t>(timer));
}

/** @brief How long the current song has been playing, milliseconds, per song. */
int usp_playtime_ms(void) { return static_cast<int>(usplayer_playtime_ms()); }

/* ---- Songlengths, for a page that has the database ------------------------ *
 *
 * The database is about five megabytes, so nothing here holds a copy of it and
 * nothing embeds it: the page owns the text and hands it over for the length of
 * one call. Two calls rather than one, because the key is useful on its own and
 * because it lets a page cache keys without keeping the database in wasm memory.
 *
 * There is no MD5 in the browser to do this with. WebCrypto deliberately omits
 * it, so the hash has to come from here, which is the reason these are exported
 * at all rather than left to JavaScript.
 */

/**
 * @brief The database key for a .sid file: the MD5 of the whole file.
 *
 * @param out  at least 33 bytes; written with 32 hex characters and a
 *             terminator, so a page can read it back as a string.
 */
void usp_song_md5(const uint8_t * file_bytes, int len, char * out)
{
  if (file_bytes == nullptr || len <= 0 || out == nullptr) return;
  usbsid::songlengths_key(file_bytes, static_cast<size_t>(len), out);
}

/**
 * @brief One song's length in milliseconds, or 0 when it is not in there.
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

/** @brief How many songs the database lists for a key. Zero when absent. */
int usp_songlength_count(const char * db, int db_len, const char * key)
{
  if (db == nullptr || db_len <= 0 || key == nullptr) return 0;
  const usbsid::SongLengths sl =
    usbsid::songlengths_lookup(db, static_cast<size_t>(db_len), key);
  return sl.valid ? static_cast<int>(sl.count) : 0;
}

/* ------------------------------------------------------------------------ *
 * software audio: same `ResidFpSidBackend` the CLI player uses, compiled to
 * wasm. Samples are pulled (usp_audio_take), not pushed, because an
 * AudioWorklet cannot call into this module - it runs on the audio thread,
 * this lives on the main thread or in a worker.
 * ------------------------------------------------------------------------ */

static usbsid::ResidFpSidBackend g_soft;
static bool g_soft_on = false;

/**
 * @brief Build the software SID and route the emulation into it.
 *
 * @param chips    1 to 4, normally the tune's own count
 * @param rate     the AudioContext's sampleRate, not a wish: whatever the device
 *                 actually runs at is what this has to be
 * @param quality  0 fast (linear), 1 good (sinc)
 * @param model    0 for 6581, 1 for 8580
 * @returns 1 on success, 0 if reSIDfp would not take the parameters
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
                              : usbsid::SoftSidModel::Mos6581)) {
    g_soft_on = false;
    return 0;
  }
  /* attach() is also what sets access_overhead to 0, which a software SID needs
   * and a board does not. See sid_residfp.h. */
  g_soft.attach(usbsid::usplayer_machine());
  g_soft_on = true;
  return 1;
}

/** @brief Is the software SID the thing receiving writes? */
int usp_audio_enabled(void) { return g_soft_on ? 1 : 0; }

/** @brief Rendered samples waiting to be taken. */
int usp_audio_available(void)
{
  return g_soft_on ? static_cast<int>(g_soft.available()) : 0;
}

/**
 * @brief Take up to `max` rendered samples into a heap buffer.
 *
 * @param out  an int16 buffer in the wasm heap, from usp_alloc
 * @returns how many were written, which is fewer than asked for when the
 *          emulation has not run far enough yet
 */
int usp_audio_take(int16_t * out, int max)
{
  if (!g_soft_on || out == nullptr || max <= 0) return 0;
  return static_cast<int>(g_soft.take(out, static_cast<size_t>(max)));
}

/** @brief Drop everything rendered but not taken, on a stop or a seek. */
void usp_audio_discard(void) { if (g_soft_on) g_soft.discard(); }

/**
 * @brief Run the emulation without synthesising anything.
 *
 * For running through a tune's silent lead-in fast (~1/10th the cost per
 * frame; synthesis is nearly all of it). Writes still land, chips just
 * aren't clocked, so state is current the moment this goes back on.
 *
 * @param on 1 to synthesise, 0 to run silently
 */
void usp_audio_render(int on) { g_soft.set_render(on != 0); }

/** @brief Is the synthesis running, as opposed to being run through? */
int usp_audio_rendering(void) { return g_soft.rendering() ? 1 : 0; }


/**
 * @brief Samples that came out past full scale and were clamped.
 *
 * Chips sum with no headroom, so N chips can reach N times full scale and
 * clip as a ripple/buzz rather than an obvious fault - hence a number
 * instead of relying on an ear.
 */
int usp_audio_clipped(void)
{
  return g_soft_on ? static_cast<int>(g_soft.clipped()) : 0;
}

/** @brief Writes to $df40/$df50 reSIDfp cannot voice, so a page can say so. */
int usp_audio_fm_writes(void)
{
  return g_soft_on ? static_cast<int>(g_soft.fm_writes()) : 0;
}

/**
 * @brief How loud the reSIDfp (SID) side of the mix is, independent of FM.
 *
 * A plain multiplier applied before the two chips are summed - a post-mix
 * page volume control (a WebAudio GainNode) cannot balance one against the
 * other, only turn both down together. Callable any time, including before
 * usp_audio_configure(): the backend remembers it, exactly as the CLI's
 * -rv/--resid-volume does - see ResidFpSidBackend::set_sid_gain().
 *
 * @param percent 100 is unity, 0 silences the SID side, 300 is the CLI's
 *                own suggested ceiling (not enforced here either)
 */
void usp_audio_set_sid_volume(int percent)
{
  g_soft.set_sid_gain(static_cast<float>(percent) / 100.0f);
}

/** @brief The SID side's current multiplier, as usp_audio_set_sid_volume()
 * would take it (100 = unity). */
int usp_audio_sid_volume(void)
{
  return static_cast<int>(g_soft.sid_gain() * 100.0f);
}

/**
 * @brief How loud the FM/OPL side of the mix is, independent of the SID side.
 *
 * See usp_audio_set_sid_volume() - same idea, the other chip. Default 50: the
 * OPL is the louder of the two in practice - see OplChip::set_gain().
 *
 * @param percent 100 is unity, 0 silences the FM side
 */
void usp_audio_set_fm_volume(int percent)
{
  g_soft.set_fm_gain(static_cast<float>(percent) / 100.0f);
}

/** @brief The FM side's current multiplier, as usp_audio_set_fm_volume()
 * would take it (100 = unity). */
int usp_audio_fm_volume(void)
{
  return static_cast<int>(g_soft.fm_gain() * 100.0f);
}

} /* extern "C" */
