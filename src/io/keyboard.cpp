/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * keyboard.cpp
 *
 * The matrix layout is the C64's, row by row, as printed in every reference:
 * row 0 holds the delete key and the function keys, row 7 holds RUN/STOP, the
 * space bar and the Commodore key.
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

#include "keyboard.h"

namespace usbsid {

namespace {

struct CharKey {
  char c;
  uint8_t row;
  uint8_t col;
  bool shift;
};

/* The unshifted keys, in matrix order. Letters are given in lower case
 * because that is what a caller writes; on the machine they are the same key
 * either way, and unshifted is what the editor prints as an upper case
 * character in its default character set. */
const CharKey kCharKeys[] = {
  /* row 0 */                    { '\r', 0, 1, false },
  /* row 1 */
  { '3', 1, 0, false }, { 'w', 1, 1, false }, { 'a', 1, 2, false },
  { '4', 1, 3, false }, { 'z', 1, 4, false }, { 's', 1, 5, false },
  { 'e', 1, 6, false },
  /* row 2 */
  { '5', 2, 0, false }, { 'r', 2, 1, false }, { 'd', 2, 2, false },
  { '6', 2, 3, false }, { 'c', 2, 4, false }, { 'f', 2, 5, false },
  { 't', 2, 6, false }, { 'x', 2, 7, false },
  /* row 3 */
  { '7', 3, 0, false }, { 'y', 3, 1, false }, { 'g', 3, 2, false },
  { '8', 3, 3, false }, { 'b', 3, 4, false }, { 'h', 3, 5, false },
  { 'u', 3, 6, false }, { 'v', 3, 7, false },
  /* row 4 */
  { '9', 4, 0, false }, { 'i', 4, 1, false }, { 'j', 4, 2, false },
  { '0', 4, 3, false }, { 'm', 4, 4, false }, { 'k', 4, 5, false },
  { 'o', 4, 6, false }, { 'n', 4, 7, false },
  /* row 5 */
  { '+', 5, 0, false }, { 'p', 5, 1, false }, { 'l', 5, 2, false },
  { '-', 5, 3, false }, { '.', 5, 4, false }, { ':', 5, 5, false },
  { '@', 5, 6, false }, { ',', 5, 7, false },
  /* row 6 */
  { '*', 6, 1, false }, { ';', 6, 2, false }, { '=', 6, 5, false },
  { '^', 6, 6, false }, { '/', 6, 7, false },
  /* row 7 */
  { '1', 7, 0, false }, { '2', 7, 3, false }, { ' ', 7, 4, false },
  { 'q', 7, 6, false },

  /* the shifted ones worth having: what a typed line actually needs */
  { '!', 7, 0, true }, { '"', 7, 3, true }, { '#', 1, 0, true },
  { '$', 1, 3, true }, { '%', 2, 0, true }, { '&', 2, 3, true },
  { '\'', 3, 0, true }, { '(', 3, 3, true }, { ')', 4, 0, true },
  { '<', 5, 7, true }, { '>', 5, 4, true }, { '?', 6, 7, true },
  { '[', 5, 5, true }, { ']', 6, 2, true },
};

} /* namespace */

bool key_for_char(char c, KeyPos & out)
{
  if (c == '\n') c = '\r';
  if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');

  for (const CharKey & k : kCharKeys) {
    if (k.c != c) continue;
    out.row = k.row;
    out.col = k.col;
    out.shift = k.shift;
    return true;
  }
  return false;
}

void Keyboard::set(KeyPos key, bool pressed)
{
  cia_.set_key(key.row, key.col, pressed);
  if (key.shift) {
    cia_.set_key(kKeyLShift.row, kKeyLShift.col, pressed);
  }
}

void Keyboard::reset(void)
{
  cia_.clear_keys();
  pending_ = 0;
  head_ = 0;
  held_frames_ = 0;
  gap_frames_ = 0;
  holding_ = false;
}

bool Keyboard::tap(KeyPos key)
{
  if (pending_ >= kQueueSize) return false;
  queue_[(head_ + pending_) % kQueueSize] = key;
  ++pending_;
  return true;
}

bool Keyboard::type(const char * text)
{
  if (text == nullptr) return false;

  for (const char * p = text; *p != '\0'; p++) {
    KeyPos key;
    if (!key_for_char(*p, key)) continue; /* nothing on this machine types it */
    if (!tap(key)) return false;
  }
  return true;
}

void Keyboard::press_current(bool pressed)
{
  if (pending_ == 0) return;
  set(queue_[head_], pressed);
}

void Keyboard::tick_frame(void)
{
  if (holding_) {
    if (--held_frames_ != 0) return;
    press_current(false);
    holding_ = false;
    head_ = (head_ + 1) % kQueueSize;
    --pending_;
    gap_frames_ = kGapFrames;
    return;
  }

  if (gap_frames_ != 0) { --gap_frames_; return; }
  if (pending_ == 0) return;

  press_current(true);
  holding_ = true;
  held_frames_ = kHoldFrames;
}

} /* namespace usbsid */
