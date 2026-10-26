/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_web.cpp
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

#include "sid_web.h"

#include "mos6581_8580.h" /* kMaxSids */

namespace usbsid {

void WebSidBackend::write(addr_t reg, data_t value, uint16_t cycles)
{
  ++writes_;

  /* Chip 5 and up ($80 to kFmOplRange - 1) never reach this board, it has 4 real
   * sockets over WebUSB (see the kMaxSids comment in mos6581_8580.h)
   * Dropped, a chip 8+ register truncated into entry[0] (the one byte CYCLED_WRITE
   * register field) would corrupt the stream. UsbSidBackend and EmbeddedSidBackend drop the same range
   *
   * kFmOplRange and up is the unclaimed FM/OPL park (kFmOplParkBase, mos6581_8580.cpp)
   * $df40/$df50 with no SID claiming them. The page packs these as ring regs
   * $80 (index port) and $90 (data port) into ASID 0x60 (asid-midi.js)
   * Real chip registers stay below $80 on the ring, no collision */
  constexpr addr_t kFmOplRange = static_cast<addr_t>(kMaxSids * 0x20);
  if (reg >= kFmOplRange) reg = ((reg & 0x1f) == 0x10) ? 0x90 : 0x80;
  else if (reg >= 0x80) return;

  if (pending() >= kRingEntries) { /* the page has stopped draining */
    ++drops_;
    return;
  }

  /* The gap goes on the ring as it arrives, access_overhead is already subtracted upstream */
  const uint16_t delay = cycles;
  uint8_t * entry = &ring_[(head_ & (kRingEntries - 1)) * kEntryBytes];
  entry[0] = static_cast<uint8_t>(reg);
  entry[1] = value;
  entry[2] = static_cast<uint8_t>(delay >> 8);
  entry[3] = static_cast<uint8_t>(delay & 0xff);
  ++head_;
}

/**
 * @brief Read a register, answers open bus and is counted
 *
 * Nothing to return, a read is a round trip and the ring goes one way
 * Two way would block the emulation until the page ran the event loop,
 * a frame stepped player cannot do that
 *
 * Costs nothing in practice, reads only reach a backend with real_reads
 * (off), $d41b and $d41c are answered by the emulated voice three
 */
data_t WebSidBackend::read(addr_t reg, uint16_t cycles)
{
  (void)reg;
  (void)cycles;
  ++reads_;
  return 0xff;
}

/**
 * @brief Count and drop a gap too long to carry with a write
 *
 * Same as the desktop backend, the page keeps playback in real time
 * Sitting the gap out in this backend as well would serve it twice
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
  tail_ = head_; /* queued writes belong to the ended tune */
}

/**
 * @brief Ask for the chips to be silenced
 *
 * The page does the silencing, only it can talk to the device
 * Anything still queued is thrown away, a reset is not stuck behind a burst
 * of writes from a stopped tune, and the counter the page watches is advanced
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
