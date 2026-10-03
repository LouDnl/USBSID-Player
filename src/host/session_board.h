/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session_board.h
 * Session output to USBSID-Pico boards through the driver. Frames are paced
 * on the wall clock, the boards play every write at its cycle.
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
#ifndef _US_HOST_SESSION_BOARD_H_
#define _US_HOST_SESSION_BOARD_H_

#include <string>
#include <vector>

#include "session.h"
#include "sid_usbsid.h"

namespace usbsid {

/** @brief One or more USBSID-Pico boards. */
class BoardSessionSink final : public SessionSink
{
  public:
    /**
     * @param board_order  serial numbers to open in this order, empty for
     *                     the first board the driver finds
     * @param overhead     cycles one hardware access costs (CLI default 1)
     */
    explicit BoardSessionSink(std::vector<std::string> board_order = {},
                              uint8_t overhead = 1);
    ~BoardSessionSink(void) override;

    const char * name(void) const override { return "board"; }
    SidBackend & backend(void) override { return usb_; }
    bool attach(Machine & machine) override;
    bool prepare(Machine & machine, const SidFile * tune) override;
    bool frame_done(const std::atomic<bool> & stopping) override;
    bool wall_clock(void) const override { return true; }
    void silence(Machine & machine) override;
    void restore(Machine & machine) override;
    void detach(Machine & machine) override;

    /** @brief The opened boards, for board information. */
    const UsbSidBackend & boards(void) const { return usb_; }

  private:
    std::vector<std::string> board_order_;
    uint8_t overhead_;
    UsbSidBackend usb_;
    bool held_ = false;
};

} /* namespace usbsid */

#endif /* _US_HOST_SESSION_BOARD_H_ */
