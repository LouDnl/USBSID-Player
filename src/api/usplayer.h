/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * usplayer.h
 * The C API for the firmware calls
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

/* Firmware clock switch as a pointer, not a weak function (see usplayer.cpp)
 * Firmware takes the real apply_clockrate by address under EMBEDDED
 * Web build and tests bind a replacement, null leaves the clock alone
 */
extern void (*us_apply_clockrate)(int n_clock, bool suspend_sids);

/* ------------------------------------------------------------------------ *
 * The old API, unchanged
 *
 *   load_sidtune(buffer, size, subtune);   // buffer may be freed after this
 *   init_sidplayer();
 *   start_sidplayer(false);
 *   while (playing) loop_sidplayer();      // one frame per call
 *   stop_sidplayer();
 * ------------------------------------------------------------------------ */

/**
 * @brief Load a program and start it (PRG or P00), returns ready for loop_sidplayer()
 *
 * Boots a machine, puts the program at its load address and starts it with RUN or SYS.
 * The loop argument is ignored
 */
extern void load_prg(uint8_t * binary_, size_t binsize_, bool loop);

/**
 * @brief Copy and parse a SID file
 *
 * @param subt  subtune, 0 is the file default, otherwise 0 based (1 is the second song)
 */
extern void load_sidtune(uint8_t * sidfile, int sidfilesize, char subt);

/* ------------------------------------------------------------------------ *
 * Streaming upload
 *
 * Packets land directly in the player tune buffer as they arrive
 *
 *   usplayer_upload_start();
 *   while (more packets) usplayer_upload_feed(packet, n);
 *   usplayer_upload_finish_prg(false);      // or:
 *   usplayer_upload_finish_tune(subtune);
 *
 * load_sidtune()/load_prg() are built on these three
 * ------------------------------------------------------------------------ */

/** @brief Reset the upload buffer, call before the first packet */
extern void usplayer_upload_start(void);

/** @brief Append bytes to the upload buffer, returns false when full (extra bytes are dropped) */
extern bool usplayer_upload_feed(const uint8_t * buf, size_t len);

/** @brief Finish an upload as PRG */
extern void usplayer_upload_finish_prg(bool loop);

/** @brief Finish an upload as SID tune */
extern void usplayer_upload_finish_tune(char subt);

/** @brief Boot the machine, relocate the driver and enter it (slow) */
extern void init_sidplayer(void);

/** @brief Begin playing what init_sidplayer() prepared */
extern void start_sidplayer(bool loop);

/** @brief Run one frame */
extern void loop_sidplayer(void);

/** @brief Stop, silence the SIDs and return the stopped state */
extern bool stop_sidplayer(void);

extern void next_subtune(void);
extern void previous_subtune(void);

/**
 * @brief Start one song by number from its beginning, returns false when out of range
 *
 * @param song  1 to usplayer_songs(), out of range changes nothing and does not wrap
 *
 * Use next_subtune() or previous_subtune() for a forward/previous button, those wrap.
 * Full re-initialise, about 14ms on the RP2350 from the boot image
 */
extern bool usplayer_restart_song(uint16_t song);

/**
 * @brief Milliseconds the current song has been playing
 *
 * Per song, every load and init resets it, not the song length (needs the HVSC
 * Songlengths database, 5MB)
 */
extern uint32_t usplayer_playtime_ms(void);

/**
 * @brief Mute one voice of one SID while the tune keeps playing
 *
 * @param chip   1 to 4
 * @param voice  1 to 3
 *
 * Forces the voice gate bit to 0 on the way to the hardware, all other bits pass.
 * Waveform, sync and filter changes still follow, the gate clears at once.
 * Emulation is untouched, $d41b and $d41c still answer as the tune expects
 */
extern void usplayer_set_voice_mute(uint8_t chip, uint8_t voice, bool muted);

/** @brief Mute bits for one chip, bits 0 to 2, chip counts from 1 */
extern uint8_t usplayer_voice_mute(uint8_t chip);

/**
 * @brief Mute a whole chip by dropping its writes
 *
 * @param chip  1 to 4
 * @param muted true to silence
 *
 * Also silences the volume register ($d418 samples), a voice mute does not.
 * Per chip, other SIDs keep playing. $d41b and $d41c keep answering
 */
extern void usplayer_set_chip_mute(uint8_t chip, bool muted);

/** @brief Muted chips, bit 0 is chip one */
extern uint8_t usplayer_chip_mute(void);

/** @brief Send everything to socket two */
extern void force_socktwo(void);

/* ------------------------------------------------------------------------ *
 * Control functions from emulation.cpp
 * ------------------------------------------------------------------------ */

/** @brief Pause playback, other calls still work */
extern void emu_pause_playing(bool pause);

/**
 * @brief Play without pre delays and pacing, SID writes still go out
 *
 * Applied at a frame boundary, off restarts the pacer from the current time
 * Ended by stop_sidplayer(), init_sidplayer() and a new upload
 * Not implemented in the web build
 */
extern void emu_ffwd(bool enable);

/* Straight at the emulated RAM, no banking, no side effects */
extern uint8_t emu_dma_read_ram(uint16_t address);
extern void emu_dma_write_ram(uint16_t address, uint8_t data);

/* Through the PLA, as the CPU sees it */
extern uint8_t emu_read_byte(uint16_t address);
extern void emu_write_byte(uint16_t address, uint8_t data);

/* ------------------------------------------------------------------------ *
 * Configuration
 *
 * The player reads the tune, decides chip count and clock and tells the board.
 * No call is needed for a tune to play
 * ------------------------------------------------------------------------ */

/**
 * @brief Override the socket layout the player assumes
 *
 * Optional, sets where socket two starts (force_socktwo()) and which chip answers FM/OPL
 * The chip count comes from the tune, writes to $d420 and $d440 go on the bus either way
 *
 * @param numsids           ignored
 * @param sids_socket_one   chips in socket one
 * @param sids_socket_two   chips in socket two
 * @param fmopl_sid         chip answering $df40/$df50, 1 based, -1 for none
 */
extern void usplayer_set_sid_config(uint8_t numsids, uint8_t sids_socket_one,
                                    uint8_t sids_socket_two, int8_t fmopl_sid);

/**
 * @brief Apply the tune clock rate to the board, on by default
 *
 * A PAL tune asks for a PAL clock through apply_clockrate() at load time.
 * The SIDs are suspended briefly, off plays tunes of the other standard at the wrong pitch
 */
extern void usplayer_set_clock_follows_tune(bool enable);

/* ------------------------------------------------------------------------ *
 * The keyboard
 *
 * Goes through the CIA1 matrix, the KERNAL scan sees a real key.
 * Works on a running program that never reads the keyboard buffer
 * ------------------------------------------------------------------------ */

/**
 * @brief Type a line as keystrokes, returns false when the queue is full
 *
 * Queued (a key is held longer than one scan), only advances while frames run
 */
extern bool usplayer_type(const char * text);

/** @brief Press and release RUN/STOP */
extern bool usplayer_key_runstop(void);

/**
 * @brief Hold a key down or let it up
 *
 * Row and column count from zero, row 7 column 7 is RUN/STOP, row 7 column 4 is space
 */
extern void usplayer_key_set(uint8_t row, uint8_t col, bool pressed);

/** @brief Let all keys up and drop the queue */
extern void usplayer_keys_clear(void);

/** @brief Typed text is still on its way in */
extern bool usplayer_typing(void);

/* State, for a display or status report */
extern bool usplayer_playing(void);
extern bool usplayer_paused(void);

/** @brief Last load_sidtune() or load_prg() parsed */
extern bool usplayer_loaded(void);
/** @brief Loaded file is a program, not a tune */
extern bool usplayer_is_prg(void);

/**
 * @brief Interrupt sources the tune armed, as a bitmask
 *
 * Read from the chips, a CIA source counts when its mask bit is set and its timer runs,
 * the raster source when its enable bit is set. Follows later subtunes
 * CIA2 is reported as a source although it raises NMI
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
 * @brief CIA timer latch (reload value)
 *
 * Divide the cycles of a frame by it for the play rate, about 19654 is once a frame on PAL.
 * usplayer_irq_sources() tells which timer drives, reading the latch has no side effects
 *
 * @param cia    1 or 2, anything else is 1
 * @param timer  0 for timer A, 1 for timer B
 */
extern uint16_t usplayer_cia_latch(uint8_t cia, uint8_t timer);

/**
 * @brief How the loaded file was started
 *
 * USP_START_DRIVER ~ tune with the PSID driver
 * USP_START_BASIC ~ RSID with the BASIC flag and no init address, booted and RUN without driver
 * USP_START_PRG ~ .prg or .p00
 */
#define USP_START_DRIVER 0
#define USP_START_BASIC  1
#define USP_START_PRG    2
extern int usplayer_start_mode(void);

/** @brief SID clock the tune was written for, in Hz */
extern uint32_t usplayer_clock_hz(void);
/** @brief Clock is PAL */
extern bool usplayer_is_pal(void);

/**
 * @brief Frames per second of the tune video model
 *
 * PAL is 50.125 and NTSC 59.83, rounding drifts a frame every 8 seconds
 */
extern double usplayer_refresh_hz(void);

extern uint16_t usplayer_song(void);
extern uint16_t usplayer_songs(void);
extern uint32_t usplayer_frames(void);
extern uint16_t usplayer_driver_address(void);
/* Header strings as stored in the file, Windows-1252 (see win1252_to_utf8()) */
extern const char * usplayer_tune_name(void);
extern const char * usplayer_tune_author(void);
extern const char * usplayer_tune_released(void);

/**
 * @brief Multi-SID and v5 metadata from the tune, informational
 *
 * usplayer_sid_count() is the raw header count, a v5 tune can exceed the 4 chips
 * the player plays
 */
extern uint8_t usplayer_sid_count(void);
/** @brief Address of chip, 0 based, 0 when out of range */
extern uint16_t usplayer_sid_addr(uint8_t chip);
/** @brief Pan hint of chip: 0 left, 1 center, 2 right */
extern uint8_t usplayer_sid_pan(uint8_t chip);
/** @brief v5 SID Panning Layout, flags bits 6-7: 0 standard, 1 L/C/R, 2 center first, 3 fully centered */
extern uint8_t usplayer_pan_layout(void);
/** @brief v5 SID Panning Mode, flags bits 8-9: 0 direct, 1 reverse, 2 group, 3 spread */
extern uint8_t usplayer_pan_mode(void);
/** @brief v5 flags bit 11: tune uses an FM/OPL chip alongside its SIDs */
extern bool usplayer_has_fm_opl(void);
/** @brief v5 flags bit 10: song lengths are appended to the file */
extern bool usplayer_has_embedded_songlengths(void);
/** @brief Song length from that embedded table in ms, 0 if none */
extern uint32_t usplayer_embedded_songlength_ms(uint16_t song);

/** @brief SID writes since the tune started */
extern uint32_t usplayer_sid_writes(void);

/**
 * @brief Cycles spent waiting out gaps too long to carry with a write
 *
 * Diagnostic, a large count on a playing tune points at a timing problem
 */
extern uint64_t usplayer_cycles_waited(void);

/**
 * @brief Cycles the player sat out, not sent to the hardware
 *
 * Diagnostic, a gap after the bus queue drained is waited out against the board clock
 * (the emulation spent part of it). Play routine plus idle loop paces nearly all, a digi tune none
 */
extern uint64_t usplayer_cycles_paced(void);

/** @brief Total statically allocated bytes */
extern uint32_t usplayer_static_footprint(void);

/**
 * @brief Emulation speed of this board in thousands of cycles per second
 *
 * Runs the machine for the given emulated PHI2 cycles with SID output disconnected and
 * times it against the board clock. PAL needs 985 kcycles/s, NTSC 1023
 * Advances the machine, call from a stopped player (1 million cycles is a good size)
 * Returns 0 without a clock to time against
 */
extern uint32_t usplayer_benchmark(uint32_t cycles);

/** @brief Loaded file needs real BASIC and KERNAL ROMs (BASIC program without SYS line, BASIC RSID) */
extern bool usplayer_needs_roms(void);

/** @brief A running program jammed for lack of ROMs */
extern bool usplayer_stuck_without_roms(void);

#if !defined(EMBEDDED) || !EMBEDDED
/**
 * @brief Replace the ROM images, active at the following init
 *
 * Images are copied, NULL marks one absent ~ BASIC/chargen read the RAM below,
 * no KERNAL uses the stub KERNAL
 *
 * @param basic    8192 bytes or NULL
 * @param kernal   8192 bytes or NULL
 * @param chargen  4096 bytes or NULL
 */
extern void usplayer_set_roms(const uint8_t * basic, const uint8_t * kernal,
                              const uint8_t * chargen);
/** @brief Back to the default ROM set of the build (stock images or stub) */
extern void usplayer_default_roms(void);

/** @brief Real images in use: bit 0 BASIC, bit 1 KERNAL, bit 2 chargen */
extern uint8_t usplayer_roms_present(void);
#endif

#if defined(__cplusplus)
} /* End of the C surface */

namespace usbsid {
class Machine;
/** @brief The machine the player is stepping, C++ only */
Machine & usplayer_machine(void);
}

extern "C" {
#endif

#ifdef __cplusplus
  }
#endif

#endif /* _USPLAYER_H_ */
