/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * mos6581_8580.h
 * The SID as the machine sees it, turns a register write into a timestamped
 * event for USBSID-Pico
 *
 * Address translation (1 to kMaxSids SIDs, socket forcing, FM/OPL address,
 * forced addresses)
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
#ifndef _US_SID_MOS6581_8580_H_
#define _US_SID_MOS6581_8580_H_

#include "core/bus.h"
#include "io_device.h"
#include "mos6569.h"
#include "sid_backend.h"
#include "sid_voice3.h"
#include "sidfile.h"
#include "types.h"

namespace usbsid {

/* Returned by the address translation when an address is not a SID
 * Outside every real register (0x0000-0x01df, kMaxSids chips at 0x20 each)
 * and the FM/OPL parked block (0x01e0-0x01ff, see kFmOplParkBase in mos6581_8580.cpp) */
constexpr addr_t kSidNotMapped = 0xffff;

/* Register mirror size (regs_[] below), kMaxSids chips at 0x20 = 0x1e0 bytes
 * Rounded to a power of two for mask indexing, the spare 0x20 on top parks
 * the unclaimed FM/OPL (see kFmOplParkBase, mos6581_8580.cpp) */
constexpr size_t kRegsSize = 0x200;

/** @brief How many SIDs there are and where they live */
struct SidConfig {
  /**
   * @brief Hand the backend the time that passes when the tune writes nothing
   *
   * Off for hardware, on for a software SID (set by its attach())
   * A software SID only renders when told time has passed, pacing would
   * race ahead after a silent stretch. Elapsed time is pushed to the backend
   * at the end of each video frame
   */
  bool render_idle = false;
  uint8_t count = 1;            /* 1 to kMaxSids (15), see sidfile.h */
  addr_t base[kMaxSids] = { 0xd400 };

  /**
   * @brief Which voices are held silent, one byte per chip, bits 0 to 2
   *
   * Forces the gate bit and sustain nibble to 0 on the way to hardware
   * (gate alone leaves up to 24s of release audible)
   * regs_[] keeps what the tune wrote, $d41b/$d41c polling behaves the same muted or not
   */
  uint8_t voice_mute[kMaxSids] = { 0 };

  /* Whole chips held silent, bit 0 for chip one. Writes are dropped entirely
   * (like the board mute), used for an FM/OPL chip or a second SID a tune
   * fights over. uint16_t, kMaxSids is 15 */
  uint16_t chip_mute = 0;

  /**
   * @brief Hard solo: which voices are kept, one byte per chip, bits 0 to 2
   *
   * Writes of a voice missing from this set are dropped entirely
   * A chip with no bit set also drops its filter/volume registers
   * Set once from the CLI before playback (see solo_active below)
   */
  uint8_t voice_solo[kMaxSids] = { 0 };

  /* Whether --solo is in effect, voice_solo[] all zero means nothing solo'd yet
   * and every chip must still drop */
  bool solo_active = false;

  /* USBSID-Pico socket layout, from the device config */
  uint8_t sids_socket_one = 1;
  uint8_t sids_socket_two = 0;
  int8_t  fmopl_sid = -1;       /* which chip answers $df40/$df50, 1 based */

  bool force_socket_two = false;
  bool force_address = false;
  data_t forced_address = 0;    /* the physical base to force writes to */

  bool real_reads = false;      /* read back from the hardware */

  /* Cost of one hardware access in cycles (1), subtracted from every delta
   * before sending. Measured for USBSID-Pico, a field to allow checking a board without rebuild */
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

    /* Called by the VIC at the end of every frame, the USB buffer is flushed */
    void vic_frame_ended(void) override;

    void set_backend(SidBackend & backend) { backend_ = &backend; }
    SidBackend & backend(void) { return *backend_; }

    SidConfig & config(void) { return config_; }
    const SidConfig & config(void) const { return config_; }

    /**
     * @brief Map a C64 address to a physical USBSID register
     *
     * Return kSidNotMapped when no configured chip owns the address
     * 16 bit address, kMaxSids (15) does not fit 3 top bits
     * A real-board backend gets a value under 0x80 for its 4 chips (see UsbSidBackend::write())
     */
    addr_t translate(addr_t addr, uint8_t & chip) const US_RAM_ATTR;

    /** @brief Board register the first chip of the tune is forced to (-f/-fa), or 0 */
    addr_t socket_offset(void) const US_RAM_ATTR;

    /**
     * @brief Shift a tune chip register by socket_offset()
     *
     * @param logical  (chip - 1) * 0x20 + register
     * Return the board register, kSidNotMapped past the last real chip
     */
    addr_t physical_reg(addr_t logical) const US_RAM_ATTR;

    /** @brief What the hardware should see, out of line (gcc inlines it into
     * io_write() at -O3, a hand-rolled inline measured slower) */
    data_t mask_for_output(addr_t reg, data_t value) const US_RAM_ATTR;
    /**
     * @brief Start measuring cycle deltas from the current time, called when a tune starts
     * The first write delta is not measured from boot (many max-length waits)
     */
    void resync(void)
    {
      last_event_ = bus_.cycles();
      play_start_ = last_event_;
      for (uint8_t i = 0; i < kMaxSids; i++) voice3_[i].resync(last_event_);
    }

    /** @brief Voice three of a chip, 1 based, for tests */
    SidVoice3 & voice3(uint8_t chip)
    {
      return voice3_[(chip >= 1 && chip <= kMaxSids) ? (chip - 1) : 0];
    }

    /* Register mirror, one 32 byte block per chip in tune chip order, unshifted by -f/-fa
     * Masked to 0x1ff (kRegsSize - 1) to stay a cheap AND */
    data_t peek(addr_t reg) const { return regs_[reg & (kRegsSize - 1)]; }

    /**
     * @brief Hold one voice silent or release it
     *
     * @param chip   1 to kMaxSids
     * @param voice  1 to 3
     *
     * The chip keeps gate high until written again, mute and unmute push
     * sustain then control immediately, in that order
     * Unmute restores level and restarts the note if gated
     */
    void set_voice_mute(uint8_t chip, uint8_t voice, bool muted);

    /**
     * @brief Hard solo one voice, only voices given to this (over any number of calls) reach the backend
     *
     * @param chip   1 to kMaxSids
     * @param voice  1 to 3
     *
     * One-shot for a CLI run, no un-solo
     * The first call sets solo_active, after that every chip with nothing
     * solo'd drops its filter/volume registers too
     */
    void set_voice_solo(uint8_t chip, uint8_t voice);

    /**
     * @brief Hold a whole chip silent, dropping its writes, chip counts from 1
     * On unmute the writes of the muted period are replayed
     */
    void set_chip_mute(uint8_t chip, bool muted);

    /** @brief The muted chips, bit 0 for chip one */
    uint16_t chip_mute(void) const { return config_.chip_mute; }

    /** @brief The mute bits for one chip, bits 0 to 2, chip counts from 1 */
    uint8_t voice_mute(uint8_t chip) const
    {
      return (chip >= 1 && chip <= kMaxSids) ? config_.voice_mute[chip - 1] : 0;
    }

    uint32_t writes(void) const { return writes_; }
    uint32_t reads(void) const { return reads_; }

    /** @brief C64 address of the access in flight, 0 outside io_read()/io_write() */
    addr_t io_addr(void) const { return io_addr_; }

    /** @brief Chip (1 based) of the access in flight, 0 outside io_read()/io_write() */
    uint8_t io_chip(void) const { return io_chip_; }

    /** @brief Bus cycles since the last resync(), the tune play time */
    cycle_t play_cycles(void) const { return bus_.cycles() - play_start_; }

  private:
    uint16_t cycles_since_last_event(void) US_RAM_ATTR;
    bool custom_address(addr_t addr) const US_RAM_ATTR;
    Bus & bus_;
    SidBackend * backend_;
    SidConfig config_;

    /* $00-$1f first chip, $20-$3f second etc, up to kMaxSids chips
     * $1e0-$1ff is the FM/OPL translation block when no chip claims it
     * (see kFmOplParkBase, mos6581_8580.cpp)
     * Sized kRegsSize (power of two), peek() and reg-indexed access can mask */
    data_t regs_[kRegsSize] = { 0 };

    /* $d41b and $d41c are the only readable SID registers, tunes poll them
     * for timing and random numbers. Voice three of every chip is emulated for them */
    SidVoice3 voice3_[kMaxSids];

    cycle_t last_event_ = 0;
    cycle_t play_start_ = 0;

    /* Access in flight, for a chained TraceSidBackend to label events */
    addr_t io_addr_ = 0;
    uint8_t io_chip_ = 0;

    /* Chips with a changed mute state that still need their registers pushed, bit 0 for chip one
     * set_chip_mute() runs on core 0 (config handler) and only sets a bit in this mask
     * The push happens on the emulating core (core 1) in io_write(),
     * cycle accounting must stay there. uint16_t, kMaxSids is 15 */
    volatile uint16_t chip_mute_pending_ = 0;

    void apply_chip_mute_pending(void);
    uint32_t writes_ = 0;
    uint32_t reads_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_MOS6581_8580_H_ */
