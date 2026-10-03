/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session_board.cpp
 * See session_board.h.
 *
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
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

#include "session_board.h"

#include "mos6581_8580.h"

namespace usbsid {

BoardSessionSink::BoardSessionSink(std::vector<std::string> board_order, uint8_t overhead)
  : board_order_(std::move(board_order)), overhead_(overhead)
{
}

BoardSessionSink::~BoardSessionSink(void)
{
  usb_.close();
}

bool BoardSessionSink::attach(Machine & machine)
{
  if (!usb_.is_open() && !usb_.open(board_order_)) return false;

  /* What the hardware is and what an access costs, before the tune's init
   * writes go out, the same as the command line player sets it up */
  SidConfig & config = machine.sid().config();
  config.access_overhead = overhead_;
  config.sids_socket_one = usb_.sids_socket_one();
  config.sids_socket_two = usb_.sids_socket_two();
  config.fmopl_sid = usb_.fmopl_sid();
  machine.set_sid_backend(usb_);
  if (held_) {
    usb_.mute(false);
    held_ = false;
  }
  return true;
}

bool BoardSessionSink::prepare(Machine & machine, const SidFile * tune)
{
  (void)tune;
  /* The tune's clock before its init writes go out */
  usb_.set_clock_rate(machine.vic().timing().clock_hz);
  return usb_.is_open();
}

bool BoardSessionSink::frame_done(const std::atomic<bool> & stopping)
{
  (void)stopping;
  return usb_.is_open();
}

void BoardSessionSink::silence(Machine & machine)
{
  if (!usb_.is_open() || held_) return;
  held_ = true;
  /* Zeroed registers keep the silence through an unmute, the board's MUTE
   * makes it immediate */
  Mos6581_8580 & sid = machine.sid();
  const uint8_t chips = (sid.config().count == 0) ? 1 : sid.config().count;
  for (uint8_t c = 0; c < chips; c++) {
    for (data_t r = 0; r <= 0x18; r++) {
      const addr_t reg = sid.physical_reg(static_cast<addr_t>(c * 0x20 + r));
      if (reg != kSidNotMapped) usb_.write(reg, 0x00, 4);
    }
  }
  usb_.flush();
  usb_.mute(true);
}

void BoardSessionSink::restore(Machine & machine)
{
  if (!usb_.is_open()) return;
  if (held_) {
    usb_.mute(false);
    held_ = false;
  }
  Mos6581_8580 & sid = machine.sid();
  const uint8_t chips = (sid.config().count == 0) ? 1 : sid.config().count;
  for (uint8_t c = 0; c < chips; c++) {
    for (data_t r = 0; r <= 0x18; r++) {
      const addr_t logical = static_cast<addr_t>(c * 0x20 + r);
      const addr_t reg = sid.physical_reg(logical);
      if (reg != kSidNotMapped) usb_.write(reg, sid.peek(logical), 4);
    }
  }
  usb_.flush();
}

void BoardSessionSink::detach(Machine & machine)
{
  silence(machine);
}

} /* namespace usbsid */
