/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback, for embedding on RP2350 (Pico2), and in a browser.
 *
 * src/fm/opl_chip.h
 * The FM/OPL half of a SID+FM tune, for a player with no board.
 *
 * A few tunes drive a YM3526 or YM3812 alongside the SID, through the SFX Sound
 * Expander and FM-YAM cartridges, by writing `$df40` to select a register and
 * `$df50` to write it. USBSID-Pico forwards those to a real chip when a board
 * has one; a player synthesising its own audio has to voice them itself, and
 * reSIDfp has no FM at all.
 *
 * This wraps Nuked-OPL3-fast (lib/nukedopl), a bit-exact perf fork of
 * Nuke.YKT's Nuked OPL3, which emulates a YMF262. That is an OPL3, and the two
 * chips above are the OPL1 and OPL2 it is register compatible with: an OPL3
 * left in its default mode *is* an OPL2, which is what every AdLib emulator
 * relies on.
 *
 * It renders at the same rate as the SID synthesis and is summed with it in
 * ResidFpSidBackend::advance(), so everything that takes samples from that
 * backend gets FM without knowing this file exists: --output=audio,
 * --output=wav and the browser alike.
 *
 * Not built for the embedded target. The RP2350 has no reSIDfp and no software
 * audio: there, an FM write goes to a chip on the board.
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
 */

#pragma once
#ifndef _US_OPL_CHIP_H_
#define _US_OPL_CHIP_H_

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include "opl3.h"
}

namespace usbsid {

/** @brief The two bus addresses the SID backend hands FM writes over as. */
enum : uint8_t {
  kFmAddressReg = 0x80,   /**< $df40: choose an OPL register */
  kFmDataReg    = 0x90,   /**< $df50: write the chosen register */
};

/**
 * @brief One OPL, fed from a tune's `$df40`/`$df50` writes.
 *
 * Built only when a tune actually writes one: most do not, and an OPL nobody
 * asked for is a few hundred kilobytes of tables and a per sample cost for
 * silence.
 */
class OplChip
{
  public:
    OplChip(void) = default;

    OplChip(const OplChip &) = delete;
    OplChip & operator=(const OplChip &) = delete;

    /**
     * @brief Build the chip for an output rate.
     *
     * Nuked runs at 49716 Hz internally and resamples to whatever it is given,
     * so this is the rate samples come out at and no second resampler is
     * needed.
     *
     * @param sample_rate the rate the SID synthesis is configured for
     */
    void configure(unsigned sample_rate);

    /** @brief Is there a chip to write to? */
    bool ready(void) const { return ready_; }

    /**
     * @brief One write exactly as the tune made it.
     *
     * `$df40` latches the register and `$df50` writes it, which is how the
     * cartridge is addressed and how the write pair arrives from the emulation.
     * Anything else is ignored.
     *
     * Buffered rather than immediate, which is Nuked's model of a real chip:
     * a write takes a couple of samples to land, and applying it instantly puts
     * it very slightly early.
     *
     * @param reg  kFmAddressReg or kFmDataReg
     * @param value
     */
    void bus_write(uint8_t reg, uint8_t value);

    /**
     * @brief Add `count` mono samples of OPL onto what is already in `out`.
     *
     * In place, into the buffer the SID mix has just filled - both chips are
     * summed on a machine that has both, with no second buffer to keep in
     * step. Scaled by gain() before summing (0.5 by default: the OPL is the
     * louder of the two in practice), which is deterministic attenuation
     * rather than a level-dependent limiter, same reasoning as the multi-SID
     * mix dividing by chip count.
     *
     * @param out    where the SID samples already are
     * @param count  how many to add
     */
    void mix_into(int16_t * out, size_t count);

    /**
     * @brief How much the OPL is turned down before it is summed.
     *
     * A plain multiplier, not a level-dependent limiter: 1.0 is unity, 0.5
     * (the default) halves it, 0.0 silences it. Negative is clamped to 0.
     *
     * @param gain the new multiplier
     */
    void set_gain(float gain) { gain_ = (gain < 0.0f) ? 0.0f : gain; }
    /** @brief The multiplier mix_into() scales the OPL signal by. */
    float gain(void) const { return gain_; }

    /** @brief Writes the chip has taken. */
    uint64_t writes(void) const { return writes_; }
    /** @brief Sums that had to be clamped. Should be rare, see mix_into(). */
    uint64_t clipped(void) const { return clipped_; }

    /** @brief Silence it between tunes, without rebuilding it. */
    void reset(void);

  private:
    opl3_chip chip_ = {};
    bool ready_ = false;
    unsigned sample_rate_ = 0;
    uint8_t address_ = 0;      /**< the register $df40 last selected */
    uint64_t writes_ = 0;
    uint64_t clipped_ = 0;

    /**
     * @brief FM-YAM's "digi" mode: raw 8-bit PCM through the F-Number ports.
     *
     * Real, documented FM-YAM/SFX-Sound-Expander-compatible cartridge
     * behavior (not part of the OPL2 spec, and not something Nuked knows
     * about): Mr.Mouse's Digi Organizer arms it by writing $04 to OPL
     * register 0x01, then streams 8-bit PCM through what is normally the
     * F-Number-low port ($a0, or $a1 for a second channel) instead of ever
     * touching pitch. Confirmed against a real tune built with it and
     * against SIDKick-pico's own fmopl.c, which special-cases the identical
     * register-0x01-equals-4 trigger for the same reason. Left unhandled,
     * every PCM byte lands on Nuked as a literal frequency update instead,
     * producing a rapidly warbling, aliased high-pitched tone rather than
     * the sample that was meant to play. */
    bool digi_armed_ = false;
    /** Held between writes (sample-and-hold, same as the real DAC between
     * register writes); index 0 is $a0, index 1 is $a1. Already centered and
     * scaled - see bus_write(). */
    int16_t digi_value_[2] = { 0, 0 };
    /** See set_gain(). Not reset by configure()/reset(): a host sets this
     * once and it should survive a tune change same as the SID mix's own
     * gain does. */
    float gain_ = 0.5f;
    /** Nuked renders stereo pairs; this is where they land before the mix. */
    std::vector<int16_t> scratch_;
};

} /* namespace usbsid */

#endif /* _US_OPL_CHIP_H_ */
