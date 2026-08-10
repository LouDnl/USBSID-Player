/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6581_8580.h
 * The SID as the machine sees it, and the bookkeeping that turns a register
 * write into a timestamped event for USBSID-Pico.
 *
 * The address translation (one to four SIDs, socket forcing, the FM/OPL
 * address, forced addresses) is carried over from
 * old player ~ src/c64/mos6581_8580_sid.cpp, which is known good and was not
 * worth redesigning.
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
#ifndef _US_SID_MOS6581_8580_H_
#define _US_SID_MOS6581_8580_H_

#include "core/bus.h"
#include "io_device.h"
#include "mos6569.h"
#include "sid_backend.h"
#include "sid_voice3.h"
#include "types.h"

namespace usbsid {

/* The address translation returns this when an address is not a SID at all */
constexpr data_t kSidNotMapped = 0xfe;

/**
 * @brief How many SIDs there are and where they live.
 */
struct SidConfig {
  uint8_t count = 1;            /* 1 to 4 */
  addr_t base[4] = { 0xd400, 0x0000, 0x0000, 0x0000 };

  /* USBSID-Pico socket layout, mirrored from the device config */
  uint8_t sids_socket_one = 1;
  uint8_t sids_socket_two = 0;
  int8_t  fmopl_sid = -1;       /* which chip answers $df40/$df50, 1 based */

  bool force_socket_two = false;
  bool force_address = false;
  data_t forced_address = 0;    /* the physical base to force writes to */

  bool real_reads = false;      /* read back from the hardware, not the mirror */

  /* What performing one access costs the hardware, in cycles. It is taken off
   * every delta before it is sent, because the access itself is time. One is
   * the measured figure for USBSID-Pico; it is here rather than a constant so
   * it can be checked against a board without a rebuild. With a tune writing
   * twenty five registers a frame, being a cycle out is inaudible. With a digi
   * writing six hundred, it is a percent of the frame. */
  uint8_t access_overhead = 1;
};

class Mos6581_8580 final : public IoDevice, public VicFrameObserver
{
  public:
    Mos6581_8580(Bus & bus, SidBackend & backend);
    ~Mos6581_8580(void) override = default;

    void reset(void);

    data_t io_read(addr_t addr) override US_RAM_ATTR;
    void io_write(addr_t addr, data_t value) override US_RAM_ATTR;

    /* the VIC calls this at the end of every frame, which is where the
     * existing player flushes the USB buffer */
    void vic_frame_ended(void) override;

    void set_backend(SidBackend & backend) { backend_ = &backend; }
    SidBackend & backend(void) { return *backend_; }

    SidConfig & config(void) { return config_; }
    const SidConfig & config(void) const { return config_; }

    /**
     * @brief Map a C64 address to a physical USBSID register.
     *
     * Returns kSidNotMapped when the address belongs to no configured chip.
     */
    data_t translate(addr_t addr, uint8_t & chip) const US_RAM_ATTR;
    /**
     * @brief Start measuring cycle deltas from now.
     *
     * The delta carried by the first write after a long stretch of nothing is
     * measured from the last access, and after a machine boot that is millions
     * of cycles ago: enough to be chopped into dozens of maximum length waits
     * for a gap that never happened as far as the tune is concerned. The
     * player calls this when a tune actually starts.
     */
    void resync(void)
    {
      last_event_ = bus_.cycles();
      for (uint8_t i = 0; i < 4; i++) voice3_[i].resync(last_event_);
    }

    /** @brief Voice three of a chip, 1 based, for tests. */
    SidVoice3 & voice3(uint8_t chip) { return voice3_[(chip - 1) & 0x03]; }

    /* the register mirror, one 32 byte block per chip */
    data_t peek(data_t physical_reg) const { return regs_[physical_reg & 0x7f]; }

    uint32_t writes(void) const { return writes_; }
    uint32_t reads(void) const { return reads_; }

  private:
    uint16_t cycles_since_last_event(void) US_RAM_ATTR;
    bool custom_address(addr_t addr) const US_RAM_ATTR;
    Bus & bus_;
    SidBackend * backend_;
    SidConfig config_;

    /* $00-$1f first chip, $20-$3f second, and so on. $80 and up is the
     * "nowhere" block the FM/OPL translation uses when no chip claims it. */
    data_t regs_[0x80] = { 0 };

    /* $d41b and $d41c are the only readable registers a SID has, and tunes
     * poll them for timing and for random numbers, so voice three of every
     * configured chip is emulated far enough to answer them. */
    SidVoice3 voice3_[4];

    cycle_t last_event_ = 0;
    uint32_t writes_ = 0;
    uint32_t reads_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_MOS6581_8580_H_ */
