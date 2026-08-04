/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * keyboard.h
 * The keyboard, as the machine sees it: eight rows of eight switches wired
 * across CIA1's two ports.
 *
 * Nothing here talks to the KERNAL. A key is held down or it is not, and the
 * KERNAL's own interrupt scan finds it the same way it finds a real one:
 * driving one row of port A low and reading which columns came back low on
 * port B. That is the difference between this and the shortcut step 2.10 uses,
 * where characters are put straight into the keyboard buffer at $0277: this
 * path goes through the scan, the debounce, the shift handling and the buffer,
 * so RUN/STOP works, so does a program reading the matrix itself, and so does
 * anything that watches $cb for the key currently held.
 *
 * Because a real key is held for longer than one scan, typing is a queue with
 * a frame counter rather than something that happens at once.
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
#ifndef _US_IO_KEYBOARD_H_
#define _US_IO_KEYBOARD_H_

#include "mos6526.h"
#include "types.h"

namespace usbsid {

/**
 * @brief One key, as a place in the matrix.
 *
 * Row is the port A bit the KERNAL drives low, column is the port B bit that
 * comes back low when the key is held. Both count from zero.
 */
struct KeyPos {
  uint8_t row;
  uint8_t col;
  bool shift;   /* needs the left shift held with it */
};

/* The keys that are not characters, named because their positions are not
 * something to look up twice. */
constexpr KeyPos kKeyReturn   = { 0, 1, false };
constexpr KeyPos kKeyDelete   = { 0, 0, false };
constexpr KeyPos kKeySpace    = { 7, 4, false };
constexpr KeyPos kKeyRunStop  = { 7, 7, false };
constexpr KeyPos kKeyCommodore= { 7, 5, false };
constexpr KeyPos kKeyCtrl     = { 7, 2, false };
constexpr KeyPos kKeyLShift   = { 1, 7, false };
constexpr KeyPos kKeyRShift   = { 6, 4, false };
constexpr KeyPos kKeyHome     = { 6, 3, false };
constexpr KeyPos kKeyCrsrDown = { 0, 7, false };
constexpr KeyPos kKeyCrsrRight= { 0, 2, false };
constexpr KeyPos kKeyF1       = { 0, 4, false };
constexpr KeyPos kKeyF3       = { 0, 5, false };
constexpr KeyPos kKeyF5       = { 0, 6, false };
constexpr KeyPos kKeyF7       = { 0, 3, false };
constexpr KeyPos kKeyPlus     = { 5, 0, false };
constexpr KeyPos kKeyMinus    = { 5, 3, false };

/** @brief Where a character sits in the matrix. False when it has no key. */
bool key_for_char(char c, KeyPos & out);

/**
 * @brief Holds keys down on a CIA and types lines of them.
 *
 * `tick_frame()` has to be called once a frame for the queue to advance. A key
 * is held for two frames and released for two, which is longer than the
 * KERNAL's scan and its repeat delay, so every keystroke registers exactly
 * once.
 */
class Keyboard
{
  public:
    explicit Keyboard(Mos6526 & cia1) : cia_(cia1) {}

    /** @brief Hold a key down, or let it up, right now. */
    void set(KeyPos key, bool pressed);

    /** @brief Let everything up and forget anything queued. */
    void reset(void);

    /** @brief Queue a key press and release. */
    bool tap(KeyPos key);

    /**
     * @brief Queue a line of text, and a return if the text ends with one.
     *
     * Returns false when the queue is too full to take all of it.
     */
    bool type(const char * text);

    /** @brief Advance the queue. Call once a frame. */
    void tick_frame(void);

    /** @brief Whether anything is still waiting to be typed. */
    bool busy(void) const { return pending_ != 0 || held_frames_ != 0; }

    size_t queued(void) const { return pending_; }

  private:
    static constexpr size_t kQueueSize = 32;
    /* How long a key stays down, and how long the gap is. The KERNAL scans
     * once per interrupt, so anything shorter than a frame can be missed and
     * anything without a gap looks like the key was never let up. */
    static constexpr uint8_t kHoldFrames = 2;
    static constexpr uint8_t kGapFrames = 2;

    void press_current(bool pressed);

    Mos6526 & cia_;
    KeyPos queue_[kQueueSize] = {};
    size_t pending_ = 0;
    size_t head_ = 0;
    uint8_t held_frames_ = 0;
    uint8_t gap_frames_ = 0;
    bool holding_ = false;
};

} /* namespace usbsid */

#endif /* _US_IO_KEYBOARD_H_ */
