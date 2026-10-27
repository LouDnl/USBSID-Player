/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * player.h
 * Playback state machine: load, init, run, switch subtune, stop
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

    /** @brief Parse a SID file and install it without starting (bytes must outlive the player) */
    bool load_sid(const data_t * bytes, size_t len, uint16_t song = 0);

    /** @brief Parse a PRG or P00 without starting (bytes must outlive the player) */
    bool load_prg(const data_t * bytes, size_t len);

    /** @brief Boot a machine, load the program and start it by typing RUN or SYS at the prompt */
    bool init_prg(void);

    /** @brief True if the loaded file only starts with real ROMs (BASIC program without SYS line) */
    bool needs_roms(void) const
    {
      return loaded_ && is_prg_ && prg_.is_basic() && !prg_.has_sys_stub;
    }

    /** @brief True if a running program stopped for lack of ROMs (CPU jammed, ROM set incomplete) */
    bool stuck_without_roms(void) const
    {
      return playing_ && !machine_.mmu().roms.complete() && machine_.cpu().jammed();
    }

    /** @brief Start the installed tune or restart it on another subtune */
    bool init_tune(uint16_t song);

    void start(void);
    void stop(void);
    void pause(bool paused);
    bool playing(void) const { return playing_; }
    bool paused(void) const { return paused_; }

    /* ---- the keyboard ---- */
    /* Keys go through the matrix */

    /** @brief Type a line, queued over a few frames */
    bool type(const char * text);
    /** @brief Press and release a key over a few frames */
    bool tap_key(KeyPos key);
    /** @brief Press RUN/STOP */
    bool run_stop(void);
    /** @brief True while typed keys are still queued */
    bool typing(void) const { return machine_.keyboard().busy(); }

    /** @brief Switch to the following or previous song from its beginning, wrapping */
    void next_subtune(void);
    void previous_subtune(void);

    /**
     * @brief Change song and restart it via a full re-init
     *
     * @param song 1 to songs(), out of range returns false
     */
    bool restart_song(uint16_t song);

    /** @brief Run one frame of cycles */
    void run_frame(void);
    /** @brief Run n frames */
    void run_frames(uint32_t frames);

    const SidFile & tune(void) const { return tune_; }
    const PrgFile & prg(void) const { return prg_; }
    /** @brief True if the loaded file is a program, false for a tune */
    bool is_prg(void) const { return is_prg_; }
    /** @brief True for an RSID with the BASIC flag, a program with subtunes (song goes in $030C) */
    bool is_basic_tune(void) const { return is_prg_ && basic_tune_; }
    uint16_t song(void) const { return song_; }
    uint16_t songs(void) const { return tune_.songs; }
    addr_t driver_address(void) const { return reloc_addr_; }
    uint32_t frames_played(void) const { return frames_; }

  private:
    void boot_kernal(void);
    bool init_prg_without_roms(void);
    void apply_boot_image(void);
    void setup_for_driver(bool is_pal);
    /** @brief Set the BASIC end of program pointers */
    void set_basic_pointers(addr_t end_addr);

    Machine & machine_;
    SidFile tune_;
    PrgFile prg_;
    bool is_prg_ = false;
    bool basic_tune_ = false; /* set by load_sid for an RSID BASIC tune */
    uint16_t song_ = 1;
    addr_t reloc_addr_ = 0;
    bool loaded_ = false;
    bool playing_ = false;
    bool paused_ = false;
    uint32_t frames_ = 0;
};

} /* namespace usbsid */

#endif /* _US_PLAYER_PLAYER_H_ */
