/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * player.h
 * The playback state machine: load a tune, initialise it, run it, move
 * between subtunes, stop.
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

#pragma once
#ifndef _US_PLAYER_PLAYER_H_
#define _US_PLAYER_PLAYER_H_

#include "keyboard.h"
#include "machine.h"
#include "prgfile.h"
#include "psiddrv_install.h"
#include "sidfile.h"
#include "types.h"

namespace usbsid {

class Player
{
  public:
    explicit Player(Machine & machine) : machine_(machine) {}

    /**
     * @brief Parse a SID file and install it, without starting it.
     *
     * The bytes must outlive the player: nothing is copied.
     */
    bool load_sid(const data_t * bytes, size_t len, uint16_t song = 0);

    /**
     * @brief Parse a PRG or a P00, without starting it.
     *
     * The bytes must outlive the player, the same as for a SID file.
     */
    bool load_prg(const data_t * bytes, size_t len);

    /**
     * @brief Boot a machine, load the program into it and start it.
     *
     * Started the way a person would: the program goes where its first two
     * bytes say, BASIC is told how far it reaches, and then RUN or SYS is
     * typed at the prompt. No shortcut into the entry point, because a
     * program that a shortcut can start is a program that does not need this
     * code path.
     */
    bool init_prg(void);

    /** @brief Start the installed tune, or restart it on another subtune. */
    bool init_tune(uint16_t song);

    void start(void);
    void stop(void);
    void pause(bool paused);
    bool playing(void) const { return playing_; }
    bool paused(void) const { return paused_; }

    /* ---- the keyboard -------------------------------------------------- *
     * These go through the matrix, so the KERNAL's own scan finds them. That
     * is what makes RUN/STOP work on a program: nothing here reaches into the
     * keyboard buffer. */

    /** @brief Type a line. It is queued: a key takes a few frames. */
    bool type(const char * text);
    /** @brief Hold a key down and let it up again, over a few frames. */
    bool tap_key(KeyPos key);
    /** @brief RUN/STOP, which is how a running program is interrupted. */
    bool run_stop(void);
    /** @brief Whether anything typed is still on its way in. */
    bool typing(void) const { return machine_.keyboard().busy(); }

    /** @brief Switch subtune in place, the way the existing player does. */
    /**
     * @brief The driver's "load another song" entry. **Nothing uses this.**
     *
     * Kept because it is the mechanism the working player uses and it is cheaper
     * than a re-initialise, and left unused because it **jams the CPU on some
     * tunes**: `psid/Last_Ninja_2.sid` dies at song 4 and stays dead. If it is
     * ever wanted again, `temp/tools/dbg_subtune.cpp` walks a tune both ways and
     * reports jams.
     */
    void select_subtune(uint16_t song);

    /** @brief The next or previous song, from its beginning, wrapping. */
    void next_subtune(void);
    void previous_subtune(void);

    /**
     * @brief Change song and start it from its beginning.
     *
     * `next_subtune()` and `previous_subtune()` use `select_subtune()` while a
     * tune is playing, which writes the new song number into the driver and
     * jumps to its "load another song" entry. That deliberately keeps the
     * machine and the tune's own data in place, and the consequence is that
     * most tunes carry on from wherever the music had got to rather than
     * starting the new song at its start.
     *
     * @param song  1 to songs(). **Not** wrapped: out of range returns false.
     *              Relative movement belongs to next_subtune() and
     *              previous_subtune(), which wrap and need no number, because the
     *              player is the only thing that knows which song it is on and
     *              how many there are.
     */
    bool restart_song(uint16_t song);

    /** @brief Run one frame's worth of cycles. */
    void run_frame(void);
    /** @brief Run n frames. */
    void run_frames(uint32_t frames);

    const SidFile & tune(void) const { return tune_; }
    const PrgFile & prg(void) const { return prg_; }
    /** @brief Whether what is loaded is a program rather than a tune. */
    bool is_prg(void) const { return is_prg_; }
    uint16_t song(void) const { return song_; }
    uint16_t songs(void) const { return tune_.songs; }
    addr_t driver_address(void) const { return reloc_addr_; }
    uint32_t frames_played(void) const { return frames_; }

  private:
    void boot_kernal(void);
    void apply_boot_image(void);
    void setup_for_driver(bool is_pal);
    /** @brief Tell BASIC where the program it just "loaded" ends. */
    void set_basic_pointers(addr_t end_addr);

    Machine & machine_;
    SidFile tune_;
    PrgFile prg_;
    bool is_prg_ = false;
    uint16_t song_ = 1;
    addr_t reloc_addr_ = 0;
    bool loaded_ = false;
    bool playing_ = false;
    bool paused_ = false;
    uint32_t frames_ = 0;
};

} /* namespace usbsid */

#endif /* _US_PLAYER_PLAYER_H_ */
