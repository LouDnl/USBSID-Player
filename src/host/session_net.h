/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session_net.h
 * Session output to a Network SID Device server (a USBSID-Pico with
 * ENABLE_NET=1, or any NSD v4/v5 server). Paced on the wall clock.
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

#pragma once
#ifndef _US_HOST_SESSION_NET_H_
#define _US_HOST_SESSION_NET_H_

#include <string>

#include "session.h"
#include "sid_netdevice.h"

namespace usbsid {

/** @brief A Network SID Device server over TCP. */
class NetSessionSink final : public SessionSink
{
  public:
    /**
     * @param host  server name or address
     * @param port  TCP port, 6581 by default
     * @param sids  SIDs to announce, 0 for the tune's own count
     */
    NetSessionSink(std::string host, uint16_t port = 6581, uint8_t sids = 0);
    ~NetSessionSink(void) override;

    const char * name(void) const override { return "network"; }
    SidBackend & backend(void) override { return net_; }
    bool attach(Machine & machine) override;
    bool prepare(Machine & machine, const SidFile * tune) override;
    bool frame_done(const std::atomic<bool> & stopping) override;
    bool wall_clock(void) const override { return true; }

  private:
    std::string host_;
    uint16_t port_;
    uint8_t sids_;
    NetworkSidBackend net_;
};

} /* namespace usbsid */

#endif /* _US_HOST_SESSION_NET_H_ */
