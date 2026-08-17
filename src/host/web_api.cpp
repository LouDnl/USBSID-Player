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
 * On the device this is the firmware's own function and it really does switch
 * the clock. Here there is no board in this process, so the request is
 * recorded for the page to pass on. It is defined rather than left weak on
 * purpose: an undefined weak function under wasm is not reliably null, and the
 * null check in usplayer.cpp is what decides whether it is called.
 */
static void web_apply_clockrate(int n_clock, bool suspend_sids)
{
  (void)suspend_sids;
  g_clock_id = n_clock;
}

/**
 * @brief Microseconds since the page loaded.
 *
 * The C API's benchmark reads the clock through `us_time_us_64`, so this build
 * has to point that at something. `emscripten_get_now()` is `performance.now()`,
 * milliseconds as a double, which is sub microsecond on any browser that
 * matters.
 *
 * Bound below rather than being a weak definition of `time_us_64`, which is what
 * this was and what stopped `test_web` linking on macOS: an undefined weak is an
 * ELF idea and Mach-O refuses it outright.
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
 * The command line player reads this off the device at connect and hands it
 * over the same way (see main_cli.cpp). The page has to do it explicitly
 * because a browser has no equivalent of "the driver already asked": the
 * transport reads the socket config over WebUSB and passes it in here.
 *
 * Without it `$df40`/`$df50` reach nothing, so an FM/OPL tune plays its SID
 * voices and none of its OPL, which is the symptom this exists to fix.
 *
 * `numsids` is accepted and ignored, as it is everywhere else: how many chips
 * the emulation decodes is the tune's business. `fmopl` is 1 based, -1 for a
 * board that has no FM/OPL.
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
/*
 * The tune's own strings, re-encoded as UTF-8.
 *
 * A PSID header's name, author and release fields are ISO 8859-1, and the page
 * reads them with `UTF8ToString`. A byte such as 0xFC, the u umlaut in
 * "Hans Jurgen", is not valid UTF-8 on its own, so the decoder produced a
 * replacement character and the page showed a question mark in the transport
 * line and in every log entry that named the tune.
 *
 * Unicode's first 256 code points are ISO 8859-1, so the conversion is the
 * textbook two byte encoding and nothing needs a table. Doing it here rather
 * than in the parser leaves the command line player writing the file's own
 * bytes to the terminal, which is what it has always done.
 *
 * The buffers are static because the ABI hands out a pointer the caller reads
 * before the next call, which is how the rest of this file already works. Two
 * bytes per input byte plus a terminator is the worst case, so 33 in and 96 out
 * cannot overflow.
 */
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

static char g_utf8_name[96];
static char g_utf8_author[96];
static char g_utf8_released[96];

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
 * Not three voice mutes. A voice mute masks the gate and the sustain on the way
 * out and lets every other write through, which is right for a voice the tune
 * keeps playing. A chip mute drops the chip's writes, and that is the only one of
 * the two that reaches $18: a tune playing samples through the volume register
 * carries on regardless of any number of voice mutes.
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
 * A page showing a register grid, a piano or an oscilloscope needs to know what
 * the tune has put in the chip. Watching the writes go past works only while
 * they leave the wasm: in software audio the reSIDfp backend takes them inside
 * `advance()` and nothing reaches the page at all, so a host counting writes
 * would show a chip that never changes while the tune plays.
 *
 * The emulation keeps its own mirror of every write for exactly this reason, and
 * that mirror is the same one `ResidFpSidBackend::attach()` replays into a
 * freshly built chip. Reading it costs an array index and has no side effects,
 * which a register read of the real chip would not: SID registers are write only
 * on hardware, and $1b/$1c (oscillator three and its envelope) are the only ones
 * that read back at all. So this answers what was written, which for a display
 * is what "the state of the chip" means.
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
 * Straight at the RAM, through no banking and with no side effects, which is
 * what a page showing a memory dump wants and is the only kind of read that is
 * safe to do from outside the emulation. Reading through the PLA instead
 * (`emu_read_byte()`) would hand an address in $d000-$dfff to the chip that
 * lives there, and reading a CIA's interrupt register acknowledges its pending
 * interrupts: a page redrawing a memory view would quietly break the tune it is
 * displaying. So an address under I/O answers with the RAM beneath it.
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
 * For a page that wants to say how fast a CIA driven tune is being called: the
 * usual figure is the PAL cycles in a frame divided by this, so a latch of about
 * 19654 is one call a frame and half of that is two.
 *
 * The latch and not the counter: the counter is wherever the timer happens to
 * have got to, which is a different number every time it is asked and no use for
 * a display. Latch reads are free of side effects, unlike reading the chip's
 * registers at $dc04.
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
 * software audio
 *
 * The page can make its own sound instead of, or as well as, driving a board.
 * The same `ResidFpSidBackend` the command line player uses, compiled to wasm
 * with the vendored reSIDfp.
 *
 * The samples are pulled rather than pushed, because an AudioWorklet **cannot
 * call into this module**: it runs on the audio thread and the module lives on
 * the main thread or in the worker. So the page takes samples here, posts them
 * to the worklet, and the worklet plays what it was given. That is the whole
 * reason `usp_audio_take` exists rather than a callback.
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
 * For the page running through a tune's silent lead-in, which can be a minute
 * of a loader filling memory before a note is played. A frame costs about a
 * tenth as much with this off, because the synthesis is nearly all of it, and
 * the audio was going to be thrown away regardless.
 *
 * Register writes still land, so the chips are current the moment it goes back
 * on; they are simply not clocked meanwhile. Only for stretches that are known
 * to be silent: see UsPlayerAudio._skipSilence(), which turns the synthesis
 * back on every so often precisely to find out whether that is still true.
 *
 * @param on 1 to synthesise, 0 to run silently
 */
void usp_audio_render(int on) { g_soft.set_render(on != 0); }

/** @brief Is the synthesis running, as opposed to being run through? */
int usp_audio_rendering(void) { return g_soft.rendering() ? 1 : 0; }


/**
 * @brief Samples that came out past full scale and were clamped.
 *
 * Chips are summed with no headroom, so N chips can reach N times full scale
 * and a loud multi SID tune distorts where the same tune on one chip does not.
 * It is heard as a ripple or a buzz on the loud parts rather than as an
 * obvious fault, which is exactly why it needs a number rather than an ear.
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

} /* extern "C" */
