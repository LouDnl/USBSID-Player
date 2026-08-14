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
  /**
   * @brief Hand the backend the time that passes when the tune writes nothing.
   *
   * Off for hardware, on for a software SID, set by its attach().
   *
   * A backend normally learns how much time has passed from the gap carried by
   * the next access, so a stretch with no accesses at all costs nothing and is
   * simply carried across. A board is happy with that: it plays in real time
   * and idles when there is no work.
   *
   * A software SID is not, because it only renders when it is told time has
   * passed. A tune that touches no register for a hundred frames produces no
   * samples for a hundred frames, and anything pacing itself by "emulate until
   * there is enough audio" then races ahead at fifteen times speed until the
   * tune starts making sound again. That is exactly what the browser player did
   * on tunes with a silent introduction.
   *
   * With this set, the end of each video frame pushes whatever time has gone by
   * to the backend, so silence is rendered as silence and one frame of
   * emulation always yields one frame of audio.
   */
  bool render_idle = false;
  uint8_t count = 1;            /* 1 to 4 */
  addr_t base[4] = { 0xd400, 0x0000, 0x0000, 0x0000 };

  /**
   * @brief Which voices are held silent, one byte per chip, bits 0 to 2.
   *
   * A muted voice has two things forced on the way to the hardware: the **gate
   * bit** of its control register, and the **sustain nibble** of its
   * sustain/release register, both held at 0.
   *
   * The gate alone leaves the note's release audible, and release runs to 24
   * seconds, so a voice muted mid note would fade for as long as the tune asked
   * for. Taking the sustain floor away as well makes it quiet and keeps it quiet
   * however the tune re-gates it.
   *
   * Everything else goes through untouched: the release nibble, the waveform,
   * ring modulation and sync. So the tune can keep changing a muted voice and all
   * of it is heard when the mute is lifted. The tune's own sustain value needs no
   * separate saving, because `regs_[]` below is written before the mask is
   * applied and therefore always holds it.
   *
   * Masked on the way **out** and nowhere else. `regs_[]` keeps what the tune
   * wrote, voice three's emulation is fed the unmasked value, and a trace shows
   * the tune's own writes. That matters beyond tidiness: tunes poll `$d41b` as a
   * timer and as a random source, so a mute that changed those answers would
   * change what the tune does rather than what it sounds like.
   */
  uint8_t voice_mute[4] = { 0, 0, 0, 0 };

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
     * @brief What the hardware should see. See the definition.
     *
     * Deliberately left as an ordinary out of line declaration. It looks like it
     * belongs inline, since it sits on the per write path of all three players,
     * but it was measured: gcc at -O3 already inlines it into `io_write()`, both
     * being in the same translation unit, and hand rolling an inline fast path
     * in this header made `io_write()` five instructions longer and about ten
     * percent slower. Measurement in `_project/PROGRESS.md`, 2026-08-11.
     */
    data_t mask_for_output(data_t reg, data_t value) const US_RAM_ATTR;
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

    /**
     * @brief Hold one voice silent, or let it go again.
     *
     * @param chip   1 to 4
     * @param voice  1 to 3
     *
     * Setting the mask is only half of it. The chip holds its gate high until
     * something writes to that register, and a tune with a long sustain may not
     * write it again for seconds, so muting also sends both affected registers
     * once, sustain then control. Unmuting sends the tune's current values back in
     * the same order, which restores the sustain level and then restarts the note
     * if its gate is high: immediate and predictable, which is what a listener
     * expects from unmuting.
     *
     * Two writes per change rather than one, and the order matters in both
     * directions. The reasoning is with the code.
     */
    void set_voice_mute(uint8_t chip, uint8_t voice, bool muted);

    /** @brief The mute bits for one chip, bits 0 to 2. Chip counts from 1. */
    uint8_t voice_mute(uint8_t chip) const
    {
      return (chip >= 1 && chip <= 4) ? config_.voice_mute[chip - 1] : 0;
    }

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
