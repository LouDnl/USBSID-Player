/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session_net.cpp
 * See session_net.h.
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

#include "session_net.h"

namespace usbsid {

NetSessionSink::NetSessionSink(std::string host, uint16_t port, uint8_t sids)
  : host_(std::move(host)), port_(port), sids_(sids)
{
}

NetSessionSink::~NetSessionSink(void)
{
  net_.disconnect();
}

bool NetSessionSink::attach(Machine & machine)
{
  if (!net_.is_connected() && !net_.connect(host_.c_str(), port_)) return false;
  machine.set_sid_backend(net_);
  return true;
}

bool NetSessionSink::prepare(Machine & machine, const SidFile * tune)
{
  (void)machine;
  /* A program says nothing about its chips: two, as the command line does */
  const uint8_t chips = (sids_ > 0) ? sids_
                      : static_cast<uint8_t>((tune != nullptr) ? tune->sid_count : 2);
  net_.set_sid_count(chips);
  return net_.is_connected();
}

bool NetSessionSink::frame_done(const std::atomic<bool> & stopping)
{
  (void)stopping;
  return net_.is_connected();
}

} /* namespace usbsid */
