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
#include "sidfile.h"
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

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    data_t read(addr_t reg, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override;
    void reset(void) override;

    void mute(bool muted);

    /** @brief Cycles of silence that were not sent. Diagnostic. */
    uint64_t cycles_waited(void) const { return waited_; }

    /**
     * @brief Pick which of the tune's SIDs land on which board socket.
     *
     * `tune_sids[0]` becomes the board's first SID, `tune_sids[1]` its second,
     * and so on; entries past the fourth are ignored, the board has only four
     * physical sockets to give them to (see write()'s own comment). A tune SID
     * number outside 1..kMaxSids is ignored too. Any tune SID not named here
     * is silently dropped rather than played, which is the point: this is how
     * a tune with more SIDs than the board has sockets, or fewer sockets than
     * the tune expects, gets to choose which of the tune's chips it actually
     * hears.
     *
     * Passing `count == 0` restores the default: the tune's first four SIDs go
     * to the board's first four sockets, in order.
     *
     * @param tune_sids  tune SID numbers, 1 based, in board output order
     * @param count      how many entries `tune_sids` holds
     */
    void set_sid_select(const uint8_t * tune_sids, uint8_t count);

  private:
    /**
     * @brief Turn a tune-side register into a board-side one, or say no.
     *
     * With no selection made, this is the old fixed behaviour: the tune's
     * first four SIDs pass straight through, chip 5 and up are dropped.
     *
     * With a selection made, `reg`'s chip is looked up in `chip_to_slot_`;
     * unselected chips are dropped, selected ones are moved onto the board
     * socket `set_sid_select()` put them at.
     *
     * @param reg  in/out: the tune-side register in, the board-side one out
     * @returns false when `reg` belongs to nothing the board should see
     */
    bool remap(addr_t & reg) const;

    USBSID_NS::USBSID_Class * device_ = nullptr;
    bool open_ = false;
    int num_sids_ = 0;
    int sids_one_ = 0;
    int sids_two_ = 0;
    int fmopl_sid_ = -1;
    int pcb_version_ = -1;
    uint64_t waited_ = 0;

    /* Which board socket (0 based) each tune SID (1 based, so index chip - 1)
     * lands on, or -1 for "not selected, drop it". Indexed up to kMaxSids
     * (15) because a selection can name any of the tune's chips, not just the
     * four the board can actually play. */
    bool select_active_ = false;
    int8_t chip_to_slot_[kMaxSids];
};

} /* namespace usbsid */

#endif /* _US_SID_SID_USBSID_H_ */
