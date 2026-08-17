/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * usplayer.h
 * The C API the firmware calls.
 *
 * The first block is a drop-in replacement for old player ~ src/usplayer.h:
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

/* The firmware's clock switch, as a pointer rather than a weak function. See the
 * note in usplayer.cpp: an undefined weak symbol resolves to null only on ELF,
 * and the desktop suite builds on Mach-O and PE too.
 *
 * The firmware does not need this and is unaffected: under EMBEDDED the real
 * `apply_clockrate` is taken by address. It is declared here for the web build
 * and the tests, which bind it to their own. Null means the clock is left alone.
 */
extern void (*us_apply_clockrate)(int n_clock, bool suspend_sids);

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

/**
 * @brief Start one song, by number, from its beginning.
 *
 * @param song  1 to `usplayer_songs()`. Out of range returns false and changes
 *              nothing. It does **not** wrap, on purpose: wrapping made a caller
 *              that had lost track of the song count look as though it had
 *              worked, since `usplayer_restart_song(1231)` on a thirteen song
 *              tune quietly played song 1.
 *
 * **For a next or previous button, call `next_subtune()` or
 * `previous_subtune()` instead.** They wrap, they re-initialise, and they need no
 * number from the caller, which matters because the player is the only thing that
 * knows which song it is on and how many there are: the firmware has neither
 * until it asks, and arithmetic done in three frontends is arithmetic one of them
 * gets wrong.
 *
 * Both go through a full re-initialise. That used to be the expensive option and
 * is not any more: about 19 000 cycles since the boot image, fourteen
 * milliseconds on the RP2350, against a driver side jump that jams the CPU on
 * `psid/Last_Ninja_2.sid` from song 4 onward.
 */
extern bool usplayer_restart_song(uint16_t song);

/**
 * @brief How long the current song has been playing, in milliseconds.
 *
 * Per **song**, not per session: every load and every init resets the frame
 * counter, so a subtune change starts it again. The CLI learned that the hard
 * way, where a session counter showed the wrong time after pressing next.
 *
 * Not the song's *length*, which needs HVSC's Songlengths database, five
 * megabytes that have no business on the device.
 */
extern uint32_t usplayer_playtime_ms(void);

/**
 * @brief Hold one voice of one SID silent while the tune keeps playing.
 *
 * @param chip   1 to 4
 * @param voice  1 to 3
 *
 * The voice's **gate bit** is forced to 0 on the way to the hardware and every
 * other bit passes through, so a muted voice still follows the tune's waveform,
 * sync and filter changes and returns in the right state. Muting clears the gate
 * at once rather than waiting for the tune to write that register again, which
 * for a long sustain could be seconds.
 *
 * The emulation is untouched: `$d41b` and `$d41c` still answer as the tune
 * expects, so a tune polling voice three as a timer keeps working.
 */
extern void usplayer_set_voice_mute(uint8_t chip, uint8_t voice, bool muted);

/** @brief The mute bits for one chip, bits 0 to 2. Chip counts from 1. */
extern uint8_t usplayer_voice_mute(uint8_t chip);

/**
 * @brief Hold a whole chip silent, dropping its writes.
 *
 * @param chip  1 to 4
 * @param muted true to silence
 *
 * Not the same as muting its three voices. A voice mute masks the gate and the
 * sustain on the way out and lets everything else through, which is right for a
 * voice a tune keeps playing. A chip mute drops the chip's writes entirely, so it
 * also silences anything going through the volume register: a tune playing samples
 * on $d418 carries on regardless of any voice mute, which is what made this
 * necessary.
 *
 * Unlike the board's own mute this is per chip, so one SID can be silenced while
 * the others play. `$d41b` and `$d41c` keep answering either way, because the
 * mirror and voice three see every write before the drop.
 */
extern void usplayer_set_chip_mute(uint8_t chip, bool muted);

/** @brief The muted chips, bit 0 for chip one. */
extern uint8_t usplayer_chip_mute(void);

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

/**
 * @brief Which interrupt sources the tune has actually armed, as a bitmask.
 *
 * What drives a tune's play routine is one of the few things about it that is
 * interesting and that the file never states: it is whatever the init routine
 * programmed. A CIA timer is the usual answer, a raster interrupt is what a
 * tune synchronised to the screen uses, and a TOD alarm is rare enough to be
 * worth seeing when it happens.
 *
 * Read from the chips rather than guessed: a CIA source counts when its mask
 * bit is set **and** the timer that feeds it is running, and the VIC's raster
 * source counts when its enable bit is set. So this answers for the tune as it
 * is playing now, and changes if a later subtune programs something else.
 *
 * CIA2 raises NMI rather than IRQ on a real machine; it is reported here
 * because "what is driving this" is the question being asked, not "which pin".
 */
#define USP_IRQ_CIA1_TA   0x01u
#define USP_IRQ_CIA1_TB   0x02u
#define USP_IRQ_CIA1_TOD  0x04u
#define USP_IRQ_CIA2_TA   0x08u
#define USP_IRQ_CIA2_TB   0x10u
#define USP_IRQ_CIA2_TOD  0x20u
#define USP_IRQ_VIC_RASTER 0x40u
extern uint32_t usplayer_irq_sources(void);

/**
 * @brief A CIA timer's latch, the value it reloads from.
 *
 * How fast a CIA driven tune calls its play routine: divide a frame's cycles by
 * this, so a latch of about 19654 is once a frame on PAL and half of it is
 * twice. `usplayer_irq_sources()` says *which* timer is driving; this says how
 * often.
 *
 * The latch and not the counter, because the counter is wherever the timer has
 * got to at the moment of asking and is a different number every time. Reading
 * the latch has no side effects, which reading the chip's own registers does.
 *
 * @param cia    1 or 2, anything else is treated as 1
 * @param timer  0 for timer A, 1 for timer B
 */
extern uint16_t usplayer_cia_latch(uint8_t cia, uint8_t timer);

/**
 * @brief How what is loaded was started.
 *
 * `USP_START_DRIVER` is the normal path for a tune: the PSID driver is
 * installed and calls init and play. `USP_START_BASIC` is an RSID with the
 * BASIC flag and no init address, which is a program the machine boots and RUNs
 * with no driver at all. `USP_START_PRG` is a .prg or .p00.
 */
#define USP_START_DRIVER 0
#define USP_START_BASIC  1
#define USP_START_PRG    2
extern int usplayer_start_mode(void);

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

#if defined(__cplusplus)
} /* the C surface ends here */

namespace usbsid {
class Machine;
/** @brief The machine the player is stepping. C++ only, see the definition. */
Machine & usplayer_machine(void);
}

extern "C" {
#endif

#ifdef __cplusplus
  }
#endif

#endif /* _USPLAYER_H_ */
