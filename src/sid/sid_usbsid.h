/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_usbsid.h
 * The backend that talks to the hardware.
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
#ifndef _US_SID_SID_USBSID_H_
#define _US_SID_SID_USBSID_H_

#include "sid_backend.h"
#include "types.h"

namespace USBSID_NS { class USBSID_Class; }

namespace usbsid {

/**
 * @brief Sends register writes to a USBSID-Pico.
 *
 * Opening the device is a separate step from constructing this, so a player
 * can be built, a tune loaded and everything checked before any hardware is
 * touched. With no device present the backend simply does nothing, which is
 * what makes the whole player runnable without hardware.
 */
class UsbSidBackend final : public SidBackend
{
  public:
    UsbSidBackend(void);
    ~UsbSidBackend(void) override;

    /** @brief Find and open a USBSID-Pico. False when there is none. */
    bool open(void);
    void close(void);
    bool is_open(void) const { return open_; }

    /** @brief Tell the device which clock the tune runs at. */
    void set_clock_rate(uint32_t hz);

    /* How the device is configured, once it is open */
    int num_sids(void) const { return num_sids_; }
    int sids_socket_one(void) const { return sids_one_; }
    int sids_socket_two(void) const { return sids_two_; }
    int fmopl_sid(void) const { return fmopl_sid_; }
    int pcb_version(void) const { return pcb_version_; }

    void write(data_t reg, data_t value, uint16_t cycles) override;
    data_t read(data_t reg, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override;
    void reset(void) override;

    void mute(bool muted);

    /** @brief Cycles of silence that were not sent. Diagnostic. */
    uint64_t cycles_waited(void) const { return waited_; }

  private:
    USBSID_NS::USBSID_Class * device_ = nullptr;
    bool open_ = false;
    int num_sids_ = 0;
    int sids_one_ = 0;
    int sids_two_ = 0;
    int fmopl_sid_ = -1;
    int pcb_version_ = -1;
    uint64_t waited_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_SID_USBSID_H_ */
