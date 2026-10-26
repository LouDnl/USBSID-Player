/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * keyboard.h
 * Keyboard as the machine sees it: 8 rows of 8 switches across the CIA1 ports.
 * A key is held or not, the KERNAL scan finds it by driving a row of port A low
 * and reading the columns on port B. RUN/STOP and programs reading the matrix
 * or $cb work. Typing is a queue with a frame counter (a real key is held longer than one scan)
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
#ifndef _US_IO_KEYBOARD_H_
#define _US_IO_KEYBOARD_H_

#include "mos6526.h"
#include "types.h"

namespace usbsid {

/** @brief One key as a place in the matrix, row is the port A bit driven low, column the port B bit read low (both from zero) */
struct KeyPos {
  uint8_t row;
  uint8_t col;
  bool shift;   /* needs the left shift held with it */
};

/* Keys that are not characters */
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

/** @brief Where a character sits in the matrix, false if it has no key */
bool key_for_char(char c, KeyPos & out);

/**
 * @brief Hold keys down on a CIA and type lines of them
 *
 * Call tick_frame() once a frame. A key is held for two frames and released for two
 */
class Keyboard
{
  public:
    explicit Keyboard(Mos6526 & cia1) : cia_(cia1) {}

    /** @brief Hold a key down or let it up immediately */
    void set(KeyPos key, bool pressed);

    /** @brief Let everything up and clear the queue */
    void reset(void);

    /** @brief Queue a key press and release */
    bool tap(KeyPos key);

    /** @brief Queue a line of text (and a return if it ends with one), false if the queue is full */
    bool type(const char * text);

    /** @brief Advance the queue, call once a frame */
    void tick_frame(void);

    /** @brief True while anything is waiting to be typed */
    bool busy(void) const { return pending_ != 0 || held_frames_ != 0; }

    size_t queued(void) const { return pending_; }

  private:
    static constexpr size_t kQueueSize = 32;
    /* Key down and gap length, the KERNAL scans once per interrupt */
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
