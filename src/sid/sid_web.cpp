/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_web.cpp
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

#include "sid_web.h"

namespace usbsid {

void WebSidBackend::write(data_t reg, data_t value, uint16_t cycles)
{
  ++writes_;

  if (pending() >= kRingEntries) { /* the page has stopped draining */
    ++drops_;
    return;
  }

  /* The gap goes on the ring as it arrives. The cycle the access itself costs
   * was already taken off upstream by SidConfig::access_overhead, and the page
   * sends these as CYCLED_WRITE, the same firmware command libusb sends, so
   * what is on the ring is exactly the pre-delay the board sits out. This used
   * to subtract a second cycle, which put every write a cycle early. */
  const uint16_t delay = cycles;
  uint8_t * entry = &ring_[(head_ & (kRingEntries - 1)) * kEntryBytes];
  entry[0] = reg;
  entry[1] = value;
  entry[2] = static_cast<uint8_t>(delay >> 8);
  entry[3] = static_cast<uint8_t>(delay & 0xff);
  ++head_;
}

/**
 * @brief A register read.
 *
 * There is nothing to return. A read is a round trip, and the ring only goes
 * one way; making it two way would mean blocking the emulation until the page
 * has been round the event loop, which is the one thing a frame stepped player
 * cannot do. So this answers open bus and is counted.
 *
 * It costs nothing in practice: reads only reach a backend when `real_reads`
 * is on, and it is off. `$d41b` and `$d41c`, the registers tunes actually
 * poll, are answered by the emulated voice three instead.
 */
data_t WebSidBackend::read(data_t reg, uint16_t cycles)
{
  (void)reg;
  (void)cycles;
  ++reads_;
  return 0xff;
}

/**
 * @brief A gap too long to carry with a write.
 *
 * Counted and dropped, the same as the desktop backend does, and for the same
 * reason: the page is what keeps playback in real time, so sitting the gap out
 * here as well would serve it twice and a silent passage would take twice as
 * long as it should.
 */
void WebSidBackend::wait(uint16_t cycles)
{
  waited_ += cycles;
}

void WebSidBackend::reset(void)
{
  writes_ = 0;
  reads_ = 0;
  waited_ = 0;
  drops_ = 0;
  tail_ = head_; /* whatever is queued belongs to the tune that just ended */
}

/**
 * @brief Ask for the chips to be silenced.
 *
 * The page does the silencing, because only it can talk to the device. What
 * happens here is that anything still queued is thrown away, so a reset is not
 * stuck behind a burst of writes from a tune that has already stopped, and the
 * counter the page watches is advanced.
 */
void WebSidBackend::reset_hardware(void)
{
  tail_ = head_;
  ++resets_;
}

void WebSidBackend::entry(uint32_t index, data_t & reg, data_t & value,
                          uint16_t & cycles) const
{
  const uint8_t * e = &ring_[((tail_ + index) & (kRingEntries - 1)) * kEntryBytes];
  reg = e[0];
  value = e[1];
  cycles = static_cast<uint16_t>((e[2] << 8) | e[3]);
}

WebSidBackend & web_backend(void)
{
  static WebSidBackend backend;
  return backend;
}

} /* namespace usbsid */
