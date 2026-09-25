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

#include <string>
#include <vector>

#include "sid_backend.h"
#include "sidfile.h"
#include "types.h"

#include "USBSID_Manager.h"

namespace usbsid {

/** @brief A set_sid_select() entry that means "the FM/OPL", not a tune SID. */
inline constexpr uint8_t kSelectFmOpl = 0xff;

/**
 * @brief Sends register writes to one or more USBSID-Pico boards.
 *
 * Opening the device is a separate step from constructing this, so a player
 * can be built, a tune loaded and everything checked before any hardware is
 * touched. With no device present the backend simply does nothing, which is
 * what makes the whole player runnable without hardware.
 *
 * Every open board's SIDs form one flat list of logical slots, board 1's
 * first. A tune's chips go to those slots in order.
 */
class UsbSidBackend final : public SidBackend
{
  public:
    UsbSidBackend(void);
    ~UsbSidBackend(void) override;

    /**
     * @brief Open USBSID-Pico boards. False when none opened.
     *
     * @param board_order  empty: open one board, the one the driver picks
     *                     by default (first in USB bus/port order). Non
     *                     empty: open exactly these serial numbers, in this
     *                     order, skipping any that fail to open.
     */
    bool open(const std::vector<std::string> & board_order = {});
    void close(void);
    bool is_open(void) const { return open_; }

    /** @brief Tell every open board which clock the tune runs at. */
    void set_clock_rate(uint32_t hz);

    /* How the device is configured, once it is open. Socket counts and the
     * pcb version describe board 1. */
    int num_boards(void) const { return manager_.BoardCount(); }
    int num_sids(void) const { return num_sids_; }
    /** @brief Slots set_sid_select() can fill: 4 sockets on one board, the
     * logical SID count across several. */
    int num_slots(void) const { return single_ ? 4 : manager_.TotalSIDs(); }
    int sids_socket_one(void) const { return sids_one_; }
    int sids_socket_two(void) const { return sids_two_; }
    /** @brief Slot the FM/OPL answers on, 1 based, -1 for none. One board:
     * its own FM/OPL SID number. Several: the logical slot of the first
     * FM/OPL found in board order. */
    int fmopl_sid(void) const { return fmopl_sid_; }
    /** @brief Board, 1 based in open order, that fmopl_sid() is on, -1 for none. */
    int fmopl_board(void) const { return fmopl_board_; }
    int pcb_version(void) const { return pcb_version_; }
    const USBSID_Manager & manager(void) const { return manager_; }

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    data_t read(addr_t reg, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override;
    void reset(void) override;

    void mute(bool muted);

    /** @brief Cycles of silence that were not sent. Diagnostic. */
    uint64_t cycles_waited(void) const { return waited_; }

    /**
     * @brief Pick which of the tune's SIDs land on which logical slot.
     *
     * `tune_sids[0]` becomes logical slot 1, `tune_sids[1]` slot 2, and so
     * on; entries past num_slots() are ignored, as is a tune SID number
     * outside 1..kMaxSids. Any tune SID not named here is dropped rather
     * than played, which is how a tune with more SIDs than the boards have
     * gets to choose which of its chips are heard.
     *
     * A `kSelectFmOpl` entry sends the tune's FM/OPL writes to that slot,
     * but only when the slot is its board's own FM/OPL (see
     * slot_is_fmopl()). With a selection active the FM/OPL is not played
     * anywhere else: the caller sets SidConfig::fmopl_sid to -1 for its
     * writes to arrive parked, and an unnamed or refused FM/OPL is dropped.
     *
     * Passing `count == 0` restores the default: the tune's chips go to the
     * logical slots in order.
     *
     * @param tune_sids  tune SID numbers, 1 based, or kSelectFmOpl, in
     *                   logical slot order
     * @param count      how many entries `tune_sids` holds
     * @returns false when a kSelectFmOpl entry named a slot that is not an
     *          FM/OPL, true otherwise
     */
    bool set_sid_select(const uint8_t * tune_sids, uint8_t count);

    /**
     * @brief Whether a logical slot, 0 based, is its board's FM/OPL.
     *
     * True only when the board's firmware reports that SID number as its
     * FM/OPL (READ_FMOPLSID). One board: the slot is the SID number minus
     * one. Several: the slot's local slot on its board, minus one.
     */
    bool slot_is_fmopl(int slot) const;

  private:
    /**
     * @brief Turn a tune-side register into a logical slot and board register.
     *
     * @param reg          tune-side register, chip folded into the high bits
     * @param logical_sid  out: index into manager_.LogicalMap()
     * @param board_reg    out: register to hand that board, its own socket
     *                     slot folded into the high bits
     * @returns false when `reg` belongs to nothing an open board should see
     */
    bool remap(addr_t reg, int & logical_sid, uint8_t & board_reg) const;

    void find_first_fmopl(void);

    USBSID_Manager manager_;
    bool open_ = false;
    bool single_ = true;        /* one board open, see remap() */
    int num_sids_ = 0;
    int sids_one_ = 0;
    int sids_two_ = 0;
    int fmopl_sid_ = -1;
    int fmopl_board_ = -1;
    int pcb_version_ = -1;
    uint64_t waited_ = 0;

    /* Logical slot (0 based) each tune SID (1 based, index chip - 1) lands
     * on, or -1 to drop it. Sized kMaxSids: a selection can name any of the
     * tune's chips, not only the ones the boards can play. */
    bool select_active_ = false;
    int fm_slot_ = -1;          /* logical slot an `fm` entry won, 0 based */
    int16_t chip_to_logical_[kMaxSids];
};

} /* namespace usbsid */

#endif /* _US_SID_SID_USBSID_H_ */
