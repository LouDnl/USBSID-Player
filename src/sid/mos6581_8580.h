/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6581_8580.h
 * The SID as the machine sees it, and the bookkeeping that turns a register
 * write into a timestamped event for USBSID-Pico.
 *
 * The address translation (one to kMaxSids SIDs, socket forcing, the FM/OPL
 * address, forced addresses) is carried over from
 * old player ~ src/c64/mos6581_8580_sid.cpp, which is known good and was not
 * worth redesigning; only how many chips it counts to has changed since.
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
#include "sidfile.h"
#include "types.h"

namespace usbsid {

/* The address translation returns this when an address is not a SID at all.
 * Out of range of every real register (0x0000-0x01df, kMaxSids chips at
 * 0x20 each) and of the FM/OPL "parked, unclaimed" block (0x01e0-0x01ff, see
 * kFmOplParkBase in mos6581_8580.cpp), so it can never collide with a
 * legitimate translate() answer now that reg is a 16 bit address rather than
 * an 8 bit one. */
constexpr addr_t kSidNotMapped = 0xffff;

/* Register mirror size (regs_[] below): kMaxSids chips at 0x20 registers
 * each = 0x1e0 bytes, rounded to a power of two so indexing stays a mask,
 * with the spare 0x20 at the top parking the FM/OPL "unclaimed" case (see
 * kFmOplParkBase, mos6581_8580.cpp). */
constexpr size_t kRegsSize = 0x200;

/**
 * @brief How many SIDs there are and where they live.
 */
struct SidConfig {
  /**
   * @brief Hand the backend the time that passes when the tune writes nothing.
   *
   * Off for hardware, on for a software SID (set by its attach()). A
   * software SID only renders when told time has passed, so a silent
   * stretch with no register writes would otherwise produce no audio and
   * let pacing race ahead once sound resumes. This pushes elapsed time to
   * the backend at the end of each video frame regardless.
   */
  bool render_idle = false;
  uint8_t count = 1;            /* 1 to kMaxSids (15), see sidfile.h */
  addr_t base[kMaxSids] = { 0xd400 };

  /**
   * @brief Which voices are held silent, one byte per chip, bits 0 to 2.
   *
   * Forces the gate bit and sustain nibble to 0 on the way to hardware
   * (gate alone would leave up to 24s of release audible). Everything else
   * passes through untouched, and only on the way out: `regs_[]` keeps
   * what the tune wrote so `$d41b`/`$d41c` polling still behaves the same
   * muted or not.
   */
  uint8_t voice_mute[kMaxSids] = { 0 };

  /* Whole chips held silent, bit 0 for chip one. Unlike voice_mute above,
   * writes are dropped entirely (matches the board's own mute), used for
   * an FM/OPL chip or a second SID a tune is fighting over. uint16_t since
   * kMaxSids is 15, one bit short of an 8 bit mask. */
  uint16_t chip_mute = 0;

  /**
   * @brief Hard solo: which voices are kept, one byte per chip, bits 0 to 2.
   *
   * Unlike voice_mute, a voice missing from here has its writes dropped
   * entirely rather than merely gate/sustain-silenced, and a chip with no
   * bit set at all also drops its shared filter/volume registers - so only
   * --solo's own SPEC ever reaches the backend. Set once from the CLI
   * before playback starts (see solo_active below); no runtime toggle or
   * unmute-replay concern like chip_mute has.
   */
  uint8_t voice_solo[kMaxSids] = { 0 };

  /* Whether --solo is in effect at all. voice_solo[] full of zero bits
   * means "nothing solo'd yet", which without this flag would be
   * indistinguishable from "solo not requested" - and the former must
   * still drop every chip. */
  bool solo_active = false;

  /* USBSID-Pico socket layout, mirrored from the device config */
  uint8_t sids_socket_one = 1;
  uint8_t sids_socket_two = 0;
  int8_t  fmopl_sid = -1;       /* which chip answers $df40/$df50, 1 based */

  bool force_socket_two = false;
  bool force_address = false;
  data_t forced_address = 0;    /* the physical base to force writes to */

  bool real_reads = false;      /* read back from the hardware, not the mirror */

  /* Cost of one hardware access, in cycles, subtracted from every delta
   * before it's sent. Measured figure for USBSID-Pico (1); a field rather
   * than a constant so it can be checked against a board without rebuild. */
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
     * Returns a 16 bit address, not an 8 bit register byte, since kMaxSids
     * (15) no longer fits 3 top bits; a real-board backend still gets a
     * value under 0x80 for the 4 chips it can use (see UsbSidBackend::write()).
     */
    addr_t translate(addr_t addr, uint8_t & chip) const US_RAM_ATTR;

    /** @brief What the hardware should see. Out of line deliberately: gcc
     * already inlines it into io_write() at -O3, a hand-rolled inline
     * fast path measured slower. */
    data_t mask_for_output(addr_t reg, data_t value) const US_RAM_ATTR;
    /**
     * @brief Start measuring cycle deltas from now. Called when a tune
     * actually starts, so the first write's delta isn't measured from
     * boot (which would chop into many max-length waits).
     */
    void resync(void)
    {
      last_event_ = bus_.cycles();
      for (uint8_t i = 0; i < kMaxSids; i++) voice3_[i].resync(last_event_);
    }

    /** @brief Voice three of a chip, 1 based, for tests. */
    SidVoice3 & voice3(uint8_t chip)
    {
      return voice3_[(chip >= 1 && chip <= kMaxSids) ? (chip - 1) : 0];
    }

    /* the register mirror, one 32 byte block per chip. Masked to 0x1ff
     * (kRegsSize - 1, a power of two one past kMaxSids * 0x20) rather than
     * kRegsSize itself, so this stays a cheap AND regardless of what the
     * caller hands in. */
    data_t peek(addr_t physical_reg) const { return regs_[physical_reg & (kRegsSize - 1)]; }

    /**
     * @brief Hold one voice silent, or let it go again.
     *
     * @param chip   1 to kMaxSids
     * @param voice  1 to 3
     *
     * Setting the mask alone isn't enough: the chip holds gate high until
     * next written, which may be seconds away. Muting/unmuting both push
     * sustain then control immediately, in that order, so unmute restores
     * level and restarts the note if gated.
     */
    void set_voice_mute(uint8_t chip, uint8_t voice, bool muted);

    /**
     * @brief Hard-solo one voice: only voices given to this (across however
     * many calls) ever have their writes reach the backend at all.
     *
     * @param chip   1 to kMaxSids
     * @param voice  1 to 3
     *
     * One-shot for a CLI run, not a toggle - there is no way to un-solo a
     * voice once set. First call flips solo_active, after which every chip
     * with nothing solo'd drops its shared filter/volume registers too.
     */
    void set_voice_solo(uint8_t chip, uint8_t voice);

    /**
     * @brief Hold a whole chip silent, dropping its writes. Chip counts
     * from 1. On unmute, replays what the tune wrote while muted so it
     * resumes in the right state.
     */
    void set_chip_mute(uint8_t chip, bool muted);

    /** @brief The muted chips, bit 0 for chip one. */
    uint16_t chip_mute(void) const { return config_.chip_mute; }

    /** @brief The mute bits for one chip, bits 0 to 2. Chip counts from 1. */
    uint8_t voice_mute(uint8_t chip) const
    {
      return (chip >= 1 && chip <= kMaxSids) ? config_.voice_mute[chip - 1] : 0;
    }

    uint32_t writes(void) const { return writes_; }
    uint32_t reads(void) const { return reads_; }

  private:
    uint16_t cycles_since_last_event(void) US_RAM_ATTR;
    bool custom_address(addr_t addr) const US_RAM_ATTR;
    Bus & bus_;
    SidBackend * backend_;
    SidConfig config_;

    /* $00-$1f first chip, $20-$3f second, and so on, up to kMaxSids chips.
     * $1e0-$1ff is the "nowhere" block the FM/OPL translation uses when no
     * chip claims it - see kFmOplParkBase, mos6581_8580.cpp. Sized to
     * kRegsSize (a power of two) rather than kMaxSids * 0x20 exactly, so
     * peek() and every reg-indexed access here can mask instead of branch. */
    data_t regs_[kRegsSize] = { 0 };

    /* $d41b and $d41c are the only readable registers a SID has, and tunes
     * poll them for timing and for random numbers, so voice three of every
     * configured chip is emulated far enough to answer them. */
    SidVoice3 voice3_[kMaxSids];

    cycle_t last_event_ = 0;

    /* Chips whose mute state changed and still need their registers pushed,
     * bit 0 for chip one. set_chip_mute() runs on core 0 (config handler)
     * and only sets a bit here; the actual push happens on the emulating
     * core (core 1) in io_write(), since cycle accounting must stay on
     * that core. uint16_t since kMaxSids is 15, one bit short of an 8 bit
     * mask. */
    volatile uint16_t chip_mute_pending_ = 0;

    void apply_chip_mute_pending(void);
    uint32_t writes_ = 0;
    uint32_t reads_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_MOS6581_8580_H_ */
