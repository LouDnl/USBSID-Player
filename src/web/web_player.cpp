/*
 * USBSID-Player web backend.
 *
 * src/web/web_player.cpp
 *
 * Browser-facing control API for the WEB (Emscripten/WASM) build, exported as a
 * flat C ABI (see CMake EXPORTED_FUNCTIONS). It mirrors the EMBEDDED byte-buffer
 * flow from src/usplayer.cpp but is compiled for the WEB target (which otherwise
 * rides the DESKTOP code paths). The intended JS usage is the non-blocking,
 * frame-stepped model:
 *
 *   buf = usp_alloc(size); HEAPU8.set(bytes, buf);
 *   usp_load_sidtune(buf, size, subtune);   // or usp_load_prg(buf, size)
 *   usp_init_sidplayer();                    // sidtune only
 *   usp_start(0);                            // set up, do NOT block
 *   // then, once per animation frame:
 *   usp_step();                              // emulate one C64 frame
 *   // ... JS drains the SID write ring (see web_backend.cpp) ...
 *
 * File author: LouD
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
 */

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <signal.h>

#include <mos6510_cpu.h>
#include <mos6560_6561_vic.h>

/* ---- Shared player/emulator entry points (defined elsewhere) ------------ */
extern void init(void);    /* emu_init + hardwaresid_init  (usplayer.cpp) */
extern void deinit(void);  /* emu_deinit + hardwaresid_deinit             */

/* Pointer (in-memory) file loaders: EMBEDDED-signature overloads, now also
 * compiled for WEB (see psiddrv/psid.cpp and prgrunner.cpp). */
extern int  psid_load_file(uint8_t* binary_, size_t binsize_, int subtune);
extern void run_prg(uint8_t* binary_, size_t binsize_, bool loop);

extern void psid_init_driver(void);
extern void psid_init_tune(int install_driver_hook);
extern void psid_shutdown(void);
extern void start_vsid_player(bool is_pal, bool loop);
extern void emulate_c64_single(void);
extern void emu_next_subtune(void);
extern void emu_previous_subtune(void);
extern void emu_pause_playing(bool pause);

/* ---- Shared emulator state (DESKTOP types; WEB rides DESKTOP) ------------ */
extern volatile sig_atomic_t stop;
extern volatile sig_atomic_t playing;
extern volatile sig_atomic_t vsidpsid;
extern volatile bool is_pal;
extern uint8_t songno;
extern bool forcesockettwo;

/* Emulator core objects (defined in emulation.h). Needed to pace a frame. */
extern mos6510 *Cpu;
extern mos6560_6561 *Vic;

extern "C" {

/* Heap helpers so JS can stage a tune's bytes without pulling in extra runtime
 * methods. (These just forward to malloc/free.) */
void*   usp_alloc(int size) { return malloc((size_t)size); }
void    usp_free(void* p)   { free(p); }

/* Load a PSID/RSID sidtune from an in-heap buffer. subtune 0 = default. */
int usp_load_sidtune(uint8_t* buf, int size, int subtune)
{
  stop = 0;
  init();
  vsidpsid = 1;
  songno = (uint8_t)(subtune == 0 ? -1 : subtune);
  int s = ((int)(int8_t)songno != -1) ? ((int)(int8_t)songno + 1) : (int)(int8_t)songno;
  return psid_load_file(buf, (size_t)size, s);
}

/* Load a raw PRG from an in-heap buffer (sets up, does not block). */
void usp_load_prg(uint8_t* buf, int size)
{
  stop = 0;
  init();
  vsidpsid = 0;
  run_prg(buf, (size_t)size, false); /* loop=false: set up, return, then step */
}

/* Sidtune only: build + install the PSID driver. Call after usp_load_sidtune. */
void usp_init_sidplayer(void)
{
  psid_init_driver();
  psid_init_tune(1); /* 1 = install driver hook */
}

/* Prepare playback. Pass loop=0 for the frame-stepped web model (recommended);
 * loop=1 runs the blocking emulate loop (not for the browser main thread). */
void usp_start(int loop)
{
  if (vsidpsid) {
    psid_shutdown();
    start_vsid_player((bool)is_pal, (bool)loop);
  }
  playing = 1;
}

/* Advance the emulation by one C64 video frame (~19656 PAL / 17096 NTSC cycles).
 * emulate_c64_single() is a single instruction, so a frame is a bounded cycle
 * loop. The VIC raises the VSYNC SID flush at the frame boundary internally, so
 * one usp_step() == one drain-and-flush unit for the JS side. */
void usp_step(void)
{
  if (stop) return;
  CPUCLOCK start = Cpu->cycles();
  CPUCLOCK frame = (CPUCLOCK)Vic->refresh_rate;
  if (frame == 0) frame = 19656; /* PAL fallback before start configures VIC */
  while (!stop && (CPUCLOCK)(Cpu->cycles() - start) < frame) {
    emulate_c64_single();
  }
}

/* Advance by exactly one CPU instruction (fine-grained; rarely needed). */
void usp_step_instr(void)
{
  emulate_c64_single();
}

/* Tune frame rate in Hz (PAL ~50.12, NTSC ~59.83). The JS pump uses this to
 * pace usp_step() to wall-clock time instead of the display refresh rate,
 * otherwise a 120/144 Hz monitor plays the tune at 2-3x speed. Valid after
 * usp_start(). Returns 0 before the VIC is configured. */
double usp_refresh_hz(void)
{
  return (Vic != nullptr) ? Vic->refresh_frequency : 0.0;
}

/* 1 if the loaded tune runs as PAL, 0 if NTSC. Lets JS set the device clock to
 * match (SET_CLOCK: 1=PAL, 2=NTSC). Valid after usp_start(). */
int usp_is_pal(void)
{
  return (int)is_pal;
}

/* Stop playback and tear down. Idempotent: a second call is a no-op so the JS
 * side (and the config-tool app) can call stop repeatedly without crashing. */
void usp_stop(void)
{
  if (stop && !playing) return;   /* already stopped */
  stop = 1;
  playing = 0;
  if (vsidpsid) psid_shutdown();
  deinit();
}

void usp_next_subtune(void) { emu_next_subtune(); }
void usp_prev_subtune(void) { emu_previous_subtune(); }
void usp_pause(int pause)   { emu_pause_playing((bool)pause); }
int  usp_is_playing(void)   { return (int)playing; }

/* Route all tunes to socket two (dual-SID boards). */
void usp_force_socket_two(void) { forcesockettwo = true; }

} /* extern "C" */
