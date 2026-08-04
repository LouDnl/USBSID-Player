/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * usplayer.h
 * The C API the firmware calls.
 *
 * The first block is a drop-in replacement for player-repo/src/usplayer.h:
 * same names, same signatures, same call order, so USBSID-Pico's usbsid.c and
 * config.c need no changes beyond which directory they include from. The
 * second block is the handful of control functions the firmware would
 * otherwise reach into emulation.cpp for. The third is new, and is what the
 * old player got by reading the firmware's own globals: this player is told
 * instead of looking, so it stays buildable and testable off device.
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

#ifndef _USPLAYER_H_
#define _USPLAYER_H_
#pragma once

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
  extern "C" {
#endif

/* ------------------------------------------------------------------------ *
 * The old API, unchanged
 *
 * The firmware's core 1 loop uses it like this:
 *
 *   load_sidtune(buffer, size, subtune);   // buffer may be freed after this
 *   init_sidplayer();
 *   start_sidplayer(false);
 *   while (playing) loop_sidplayer();      // one frame per call
 *   stop_sidplayer();
 *
 * `load_sidtune` copies what it needs, so freeing the buffer straight after
 * the call is safe. That is what usbsid.c already does.
 * ------------------------------------------------------------------------ */

/**
 * @brief Load a program and start it: PRG, or the P00 container.
 *
 * A program has no init step. This boots a machine, puts the program where its
 * first two bytes say and starts it the way a person would, with RUN or with
 * SYS, so it returns ready to be driven by loop_sidplayer(). `loop` is
 * accepted and ignored: nothing here decides a program has finished.
 */
extern void load_prg(uint8_t * binary_, size_t binsize_, bool loop);

/**
 * @brief Copy and parse a SID file.
 *
 * @param subt  subtune, zero means the file's own default. Counted the way
 *              the firmware counts it: 0 based, so 1 is the second song.
 */
extern void load_sidtune(uint8_t * sidfile, int sidfilesize, char subt);

/** @brief Boot the machine, relocate the driver, enter it. The slow one. */
extern void init_sidplayer(void);

/** @brief Begin playing what init_sidplayer() prepared. */
extern void start_sidplayer(bool loop);

/** @brief Run one frame. Returns as soon as the frame is over. */
extern void loop_sidplayer(void);

/** @brief Stop, silence the SIDs, and report that we are stopped. */
extern bool stop_sidplayer(void);

extern void next_subtune(void);
extern void previous_subtune(void);

/** @brief Send everything to socket two instead of socket one. */
extern void force_socktwo(void);

/* ------------------------------------------------------------------------ *
 * The control functions that used to live in emulation.cpp
 * ------------------------------------------------------------------------ */

/** @brief Pause playback. A paused player still answers every other call. */
extern void emu_pause_playing(bool pause);

/** @brief Run without pacing. Only meaningful once pacing exists (2.13). */
extern void emu_ffwd(bool enable);

/* Straight at the emulated RAM, no banking, no side effects */
extern uint8_t emu_dma_read_ram(uint16_t address);
extern void emu_dma_write_ram(uint16_t address, uint8_t data);

/* Through the PLA, exactly as the CPU sees it */
extern uint8_t emu_read_byte(uint16_t address);
extern void emu_write_byte(uint16_t address, uint8_t data);

/* ------------------------------------------------------------------------ *
 * Configuration
 *
 * The firmware is not expected to configure the player, and mostly cannot:
 * it hands over a file it has not looked inside and does not know what the
 * player is about to do with it. So the direction runs the other way round.
 * The player reads the tune, decides how many chips it is emulating and what
 * clock it wants, and tells the board. Nothing here has to be called for a
 * tune to play correctly.
 * ------------------------------------------------------------------------ */

/**
 * @brief Override the socket layout the player assumes.
 *
 * Optional, and only useful to a host that knows something about the board
 * the player does not. It sets where socket two starts, for `force_socktwo()`,
 * and which chip answers the FM/OPL addresses.
 *
 * How many chips are emulated is *not* set here. That comes from the tune,
 * which is the only thing that knows, and it does not depend on the board:
 * a three SID tune's writes to $d420 and $d440 are register writes on the
 * same bus whether or not a chip is sitting there to hear them.
 *
 * @param numsids           ignored, see above
 * @param sids_socket_one   chips in socket one
 * @param sids_socket_two   chips in socket two
 * @param fmopl_sid         which chip answers $df40/$df50, 1 based, -1 for none
 */
extern void usplayer_set_sid_config(uint8_t numsids, uint8_t sids_socket_one,
                                    uint8_t sids_socket_two, int8_t fmopl_sid);

/**
 * @brief Whether the player applies the tune's clock rate to the board.
 *
 * On by default, and this is the player driving the firmware rather than the
 * other way about: a PAL tune asks the board for a PAL clock, through
 * `apply_clockrate()`, at load time. Switching the clock suspends the SIDs
 * briefly, so it can be turned off for boards where that is not wanted, at
 * the cost of every tune written for the other standard playing at the wrong
 * pitch and tempo.
 */
extern void usplayer_set_clock_follows_tune(bool enable);

/* ------------------------------------------------------------------------ *
 * The keyboard
 *
 * These go through CIA1's matrix, so the KERNAL's own scan finds them exactly
 * as it would a real key. That is what makes them work on a running program,
 * which never looks at the keyboard buffer.
 * ------------------------------------------------------------------------ */

/**
 * @brief Type a line, as keystrokes.
 *
 * Queued rather than immediate: a key has to be held for longer than one scan,
 * so a line takes a few frames per character and only advances while the
 * player is running frames. False when the queue is full.
 */
extern bool usplayer_type(const char * text);

/** @brief Press and release RUN/STOP, which interrupts a running program. */
extern bool usplayer_key_runstop(void);

/**
 * @brief Hold a key down, or let it up, right now.
 *
 * Row and column are the matrix positions, both counting from zero. Row 7
 * column 7 is RUN/STOP, row 7 column 4 is the space bar.
 */
extern void usplayer_key_set(uint8_t row, uint8_t col, bool pressed);

/** @brief Let everything up and throw away anything queued. */
extern void usplayer_keys_clear(void);

/** @brief Whether something typed is still on its way in. */
extern bool usplayer_typing(void);

/* State, for a display or a status report */
extern bool usplayer_playing(void);
extern bool usplayer_paused(void);

/** @brief Whether the last load_sidtune() or load_prg() parsed. */
extern bool usplayer_loaded(void);
/** @brief Whether what is loaded is a program rather than a tune. */
extern bool usplayer_is_prg(void);

/** @brief The SID clock the loaded tune was written for, in Hz. */
extern uint32_t usplayer_clock_hz(void);
/** @brief Whether that clock is one of the two PAL ones. */
extern bool usplayer_is_pal(void);

/**
 * @brief Frames per second of the tune's video model.
 *
 * PAL is 50.125 rather than 50, and NTSC 59.83 rather than 60. A host that
 * paces playback against its own clock needs the real number: rounding it
 * drifts by a whole frame every eight seconds.
 */
extern double usplayer_refresh_hz(void);

extern uint16_t usplayer_song(void);
extern uint16_t usplayer_songs(void);
extern uint32_t usplayer_frames(void);
extern uint16_t usplayer_driver_address(void);
extern const char * usplayer_tune_name(void);
extern const char * usplayer_tune_author(void);
extern const char * usplayer_tune_released(void);

/** @brief SID writes performed since the tune started. */
extern uint32_t usplayer_sid_writes(void);

/**
 * @brief Cycles spent waiting out gaps too long to carry with a write.
 *
 * Diagnostic. A tune with a long silent passage accumulates these; a tune that
 * shows a large count while it is clearly playing is a sign that something is
 * wrong with the timing rather than with the tune.
 */
extern uint64_t usplayer_cycles_waited(void);

/**
 * @brief Cycles the player sat out itself instead of sending to the hardware.
 *
 * Diagnostic, and the other half of the cycle accounting. A gap wide enough
 * that the bus queue has drained is waited out against the board clock rather
 * than handed over as a pre delay, because the emulation has already spent
 * part of it. A tune with a play routine and an idle loop paces nearly all of
 * its time; a digi tune paces none of it.
 */
extern uint64_t usplayer_cycles_paced(void);

/** @brief Total statically allocated bytes, so the firmware can report it. */
extern uint32_t usplayer_static_footprint(void);

/**
 * @brief How fast this board emulates, in thousands of cycles per second.
 *
 * Runs the machine for `cycles` emulated PHI2 cycles with the SID output
 * disconnected, times it against the board's own clock, and puts it back the
 * way it was. A C64 needs 985 kcycles/s for PAL and 1023 for NTSC: anything
 * below that is how much too slow playback will be, and the ratio is exact.
 *
 * Not free and not for use while playing: it advances the machine, so call it
 * from a stopped player. A million cycles is a good size. Returns 0 when
 * there is no clock to time against.
 */
extern uint32_t usplayer_benchmark(uint32_t cycles);

#ifdef __cplusplus
  }
#endif

#endif /* _USPLAYER_H_ */
