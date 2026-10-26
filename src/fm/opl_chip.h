/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * opl_chip.h
 * FM/OPL half of a SID+FM tune for playing without a board
 *
 * Tunes drive a YM3526 or YM3812 alongside the SID (SFX Sound Expander, FM-YAM)
 * by writing $df40 to select a register and $df50 to write it.
 * USBSID-Pico forwards those to a real chip when the board has one.
 *
 * Wraps Nuked-OPL3-fast (lib/nukedopl), a perf fork of Nuked OPL3 (YMF262).
 * An OPL3 in default mode is register compatible with OPL1 and OPL2.
 * Renders at the SID synthesis rate and is summed in ResidFpSidBackend::advance().
 *
 * A CLI build with FM_YMFM=1 (./build.sh cli-ymfm) uses ymfm YM3812 (lib/ymfm) instead
 * Not built for the embedded target (FM writes go to the board there)
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
 */

#pragma once
#ifndef _US_OPL_CHIP_H_
#define _US_OPL_CHIP_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(US_FM_YMFM) && US_FM_YMFM
#include "ymfm_opl.h"
#else
extern "C" {
#include "opl3.h"
}
#endif

namespace usbsid {

/** @brief The two bus addresses the SID backend passes FM writes on as */
enum : uint8_t {
  kFmAddressReg = 0x80,   /**< $df40: select an OPL register */
  kFmDataReg    = 0x90,   /**< $df50: write the selected register */
};

/** @brief One OPL fed from a tune's $df40/$df50 writes, built when a tune writes one */
class OplChip
{
  public:
    OplChip(void) = default;

    OplChip(const OplChip &) = delete;
    OplChip & operator=(const OplChip &) = delete;

    /**
     * @brief Build the chip for an output rate (Nuked resamples from 49716 Hz)
     *
     * @param sample_rate Rate the SID synthesis is configured for
     */
    void configure(unsigned sample_rate);

    /**
     * @brief Change the output rate without resetting the chip
     *
     * Registers, voices and pending writes are kept (before configure() the rate is remembered)
     *
     * @param sample_rate New output rate, ignored when 0
     */
    void set_rate(unsigned sample_rate);

    /** @brief True if there is a chip to write to */
    bool ready(void) const { return ready_; }

    /**
     * @brief One write as the tune made it
     *
     * $df40 latches the register, $df50 writes it, anything else is ignored.
     * Buffered, a real chip takes a couple of samples to apply a write
     *
     * @param reg kFmAddressReg or kFmDataReg
     * @param value Value to write
     */
    void bus_write(uint8_t reg, uint8_t value);

    /**
     * @brief Add count mono OPL samples onto the SID samples in out
     *
     * Scaled by gain() (0.5 default) before summing
     *
     * @param out Buffer holding the SID samples
     * @param count Number of samples to add
     */
    void mix_into(int16_t * out, size_t count);

    /**
     * @brief Set the multiplier the OPL is scaled by before summing
     *
     * 1.0 is unity, 0.5 (default) halves, 0.0 silences, negative is clamped to 0
     *
     * @param gain New multiplier
     */
    void set_gain(float gain) { gain_ = (gain < 0.0f) ? 0.0f : gain; }
    /** @brief The multiplier mix_into() scales the OPL signal by */
    float gain(void) const { return gain_; }

    /** @brief Writes the chip has taken */
    uint64_t writes(void) const { return writes_; }
    /** @brief Sums that were clamped, should be rare */
    uint64_t clipped(void) const { return clipped_; }

    /** @brief Silence the chip between tunes */
    void reset(void);

  private:
#if defined(US_FM_YMFM) && US_FM_YMFM
    ymfm::ymfm_interface intf_;
    ymfm::ym3812 chip_{intf_};
    double native_step_ = 1.0;   /**< native samples consumed per output sample */
    double native_pos_ = 0.0;    /**< position between prev_ and next_, 0..1 */
    int32_t prev_ = 0;           /**< native sample before the position */
    int32_t next_ = 0;           /**< native sample after the position */
#else
    opl3_chip chip_ = {};
#endif
    bool ready_ = false;
    unsigned sample_rate_ = 0;
    uint8_t address_ = 0;      /**< register last selected by $df40 */
    uint64_t writes_ = 0;
    uint64_t clipped_ = 0;

    /* FM-YAM digi mode, raw 8-bit PCM through the F-Number ports.
     * Mr.Mouse's Digi Organizer arms it by writing $04 to OPL register 0x01 then streams
     * PCM through F-Number low ($a0, or $a1 for a second channel). SIDKick-pico fmopl.c
     * does the same. Otherwise every PCM byte becomes a frequency update */
    bool digi_armed_ = false;
    /* Sample and hold value between register writes, index 0 is $a0 and 1 is $a1, already centered and scaled */
    int16_t digi_value_[2] = { 0, 0 };
    /* See set_gain(), survives configure() and reset() */
    float gain_ = 0.5f;
    /* Reset the engine for sample_rate_ */
    void chip_reset_(void);
    /* Write one register on the engine */
    void chip_write_(uint8_t reg, uint8_t value);
#if defined(US_FM_YMFM) && US_FM_YMFM
    /* Render one mono sample at the output rate */
    int32_t chip_sample_(void);
#endif

    /* Nuked renders stereo pairs, they land in this buffer before the mix (unused by ymfm) */
    std::vector<int16_t> scratch_;
};

} /* namespace usbsid */

#endif /* _US_OPL_CHIP_H_ */
