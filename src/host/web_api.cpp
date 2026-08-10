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
#include "usplayer.h"

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
void apply_clockrate(int n_clock, bool suspend_sids)
{
  (void)suspend_sids;
  g_clock_id = n_clock;
}

/**
 * @brief Microseconds since the page loaded.
 *
 * Same reasoning as apply_clockrate: the C API's benchmark calls this through
 * a weak pointer, so it needs a real definition here. `emscripten_get_now()`
 * is `performance.now()`, milliseconds as a double, which is sub microsecond
 * on any browser that matters.
 */
uint64_t time_us_64(void)
{
#ifdef __EMSCRIPTEN__
  return static_cast<uint64_t>(emscripten_get_now() * 1000.0);
#else
  return 0;
#endif
}

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
const char * usp_tune_name(void) { return usplayer_tune_name(); }
const char * usp_tune_author(void) { return usplayer_tune_author(); }
const char * usp_tune_released(void) { return usplayer_tune_released(); }
uint32_t usp_benchmark(uint32_t cycles) { return usplayer_benchmark(cycles); }

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

} /* extern "C" */
