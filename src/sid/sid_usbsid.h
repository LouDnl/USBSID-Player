/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_usbsid.h
 * The backend that talks to the hardware
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

/** @brief set_sid_select() entry for the FM/OPL (no tune SID) */
inline constexpr uint8_t kSelectFmOpl = 0xff;

/**
 * @brief Send register writes to one or more USBSID-Pico boards
 *
 * Opening the device is separate from constructing. Without a device the
 * backend does nothing, the player runs without hardware.
 *
 * All SIDs of all open boards form one flat list of logical slots, board 1 first
 * A tune's chips go to those slots in order
 */
class UsbSidBackend final : public SidBackend
{
  public:
    UsbSidBackend(void);
    ~UsbSidBackend(void) override;

    /**
     * @brief Open USBSID-Pico boards, false when none opened
     *
     * @param board_order  empty opens one board, the driver default (first in USB bus/port order)
     *                     otherwise opens exactly these serial numbers in this order
     *                     and skips any that fail to open
     */
    bool open(const std::vector<std::string> & board_order = {});
    void close(void);
    bool is_open(void) const { return open_; }

    /** @brief Tell every open board the tune clock */
    void set_clock_rate(uint32_t hz);

    /* Device configuration once open, socket counts and pcb version are of board 1 */
    int num_boards(void) const { return manager_.BoardCount(); }
    int num_sids(void) const { return num_sids_; }
    /** @brief Slots set_sid_select() can fill, 4 sockets on one board or the logical SID count of several */
    int num_slots(void) const { return single_ ? 4 : manager_.TotalSIDs(); }
    int sids_socket_one(void) const { return sids_one_; }
    int sids_socket_two(void) const { return sids_two_; }
    /** @brief Slot the FM/OPL answers on, 1 based, -1 for none
     * One board: its FM/OPL SID number. Several: logical slot of the first FM/OPL in board order */
    int fmopl_sid(void) const { return fmopl_sid_; }
    /** @brief Board (1 based, open order) fmopl_sid() is on, -1 for none */
    int fmopl_board(void) const { return fmopl_board_; }
    int pcb_version(void) const { return pcb_version_; }
    const USBSID_Manager & manager(void) const { return manager_; }

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    data_t read(addr_t reg, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override;
    void reset(void) override;

    void mute(bool muted);

    /** @brief Cycles of silence that were not sent, diagnostic */
    uint64_t cycles_waited(void) const { return waited_; }

    /**
     * @brief Pick which tune SIDs land on which logical slot, return false when a kSelectFmOpl entry named a slot that is no FM/OPL
     *
     * tune_sids[0] is logical slot 1, tune_sids[1] slot 2 etc
     * Entries past num_slots() and tune SID numbers outside 1..kMaxSids are ignored
     * Tune SIDs not in the list are dropped
     *
     * A kSelectFmOpl entry sends the tune FM/OPL writes to that slot, only
     * when the slot is a board FM/OPL (see slot_is_fmopl())
     * With a selection active the FM/OPL plays nowhere else, set
     * SidConfig::fmopl_sid to -1 to park its writes, an unnamed or refused FM/OPL is dropped
     *
     * count == 0 restores the default, chips go to the logical slots in order
     *
     * @param tune_sids  tune SID numbers (1 based) or kSelectFmOpl, in logical slot order
     * @param count      number of entries in tune_sids
     */
    bool set_sid_select(const uint8_t * tune_sids, uint8_t count);

    /**
     * @brief Check if a logical slot (0 based) is a board FM/OPL
     *
     * True only when the firmware reports that SID number as FM/OPL (READ_FMOPLSID)
     * One board: slot is SID number - 1. Several: local slot on its board - 1
     */
    bool slot_is_fmopl(int slot) const;

  private:
    /**
     * @brief Turn a tune register into a logical slot and board register, return false when no open board should see reg
     *
     * @param reg          tune register, chip in the high bits
     * @param logical_sid  out, index into manager_.LogicalMap()
     * @param board_reg    out, register for that board with its socket slot in the high bits
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

    /* Logical slot (0 based) per tune SID (1 based, index chip - 1), -1 drops it
     * Sized kMaxSids, a selection can name any chip of the tune */
    bool select_active_ = false;
    int fm_slot_ = -1;          /* logical slot an fm entry won, 0 based */
    int16_t chip_to_logical_[kMaxSids];
};

} /* namespace usbsid */

#endif /* _US_SID_SID_USBSID_H_ */
