/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_residfp.h
 * A SidBackend that synthesises audio with reSIDfp
 *
 * Receives the same writes as the USBSID-Pico backend would, in another shape:
 *
 * 1. The access cycle is not deducted
 *    Hardware backends get cycles with one already taken off (SidConfig::access_overhead)
 *    A software SID has no bus, attach() sets access_overhead to 0
 *
 * 2. reSIDfp wants the delta before the write
 *    The seam hands over a write with the gap since the last event
 *    reSIDfp wants clock(cycles) and then write(reg, value)
 *
 * 3. The chip is folded into the register
 *    reg is physical: $000-$01f chip 1, $020-$03f chip 2, up to $1c0-$1df for chip kMaxSoftSids (15)
 *    reg >> 5 picks the instance, reg & 0x1f the register
 *    Every chip is clocked by the same delta, they share phi2
 *    reg is 16 bit (addr_t), 15 chips do not fit the top 3 bits of a byte
 *
 * FM/OPL
 *    Writes to $df40/$df50 reach the backend as reg >= kFmOplRange (kFmOplParkBase, mos6581_8580.cpp)
 *    reSIDfp has no FM, they go to the OplChip and are counted in fm_writes()
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
#ifndef _US_SID_RESIDFP_H_
#define _US_SID_RESIDFP_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "opl_chip.h"
#include "sid_backend.h"
#include "sidfile.h"  /* kMaxSids, SidPan */
#include "types.h"

namespace reSIDfp { class residfp; }

namespace usbsid {

class Machine;

/** @brief Max chips synthesised at once, same as kMaxSids (sidfile.h) */
constexpr uint8_t kMaxSoftSids = kMaxSids;

/** @brief Resampler, mapped to the reSIDfp enum */
enum class SoftSidQuality : uint8_t {
  Fast,      /* linear interpolation, DECIMATE */
  Good,      /* sinc, RESAMPLE. The expensive one and the right one */
};

/** @brief Chip to model, taken from the tune header when it says */
enum class SoftSidModel : uint8_t { Mos6581, Csg8580 };

/** @brief reSIDfp combined waveforms strength */
enum class SoftSidWaveforms : uint8_t { Average, Weak, Strong };

/**
 * @brief reSIDfp filter settings for every chip
 *
 * Defaults are the reSIDfp ones, the 6581 range is shared by all chips (reSIDfp limit)
 */
struct SoftSidFilter {
  bool enabled = true;
  double curve_6581 = 0.5;            /* 0 dark to 1 bright */
  double range_6581 = 19.0 / 39.0;    /* 0 to 1, reSIDfp's uCox of 20e-6 */
  double curve_8580 = 0.5;            /* 0 dark to 1 bright */
  SoftSidWaveforms waveforms = SoftSidWaveforms::Average;
};

/** @brief Synthesise what the emulation writes */
class ResidFpSidBackend final : public SidBackend
{
  public:
    ResidFpSidBackend(void);
    ~ResidFpSidBackend(void) override;

    ResidFpSidBackend(const ResidFpSidBackend &) = delete;
    ResidFpSidBackend & operator=(const ResidFpSidBackend &) = delete;

    /**
     * @brief Build the chips and set the sample rate, return false if reSIDfp rejects the parameters
     *
     * @param chips        1 to kMaxSoftSids
     * @param clock_hz     the tune C64 clock, 985248 PAL or 1022727 NTSC
     * @param sample_rate  device rate (fixed on a phone)
     * @param quality      Fast or Good, see SoftSidQuality
     * @param model        chip to model
     * @param stereo       stereo mix, see set_pan(). Off by default, all chips start Center
     */
    bool configure(uint8_t chips, double clock_hz, unsigned sample_rate,
                   SoftSidQuality quality, SoftSidModel model,
                   bool stereo = false);

    /** @brief Is it configured and safe to attach? */
    bool ready(void) const { return ready_; }

    /** @brief Mono or stereo mix */
    bool stereo(void) const { return stereo_; }

    /** @brief Samples per frame, 1 mono or 2 stereo */
    unsigned channels(void) const { return stereo_ ? 2u : 1u; }

    /**
     * @brief Set the stereo position of a chip, chip counts from 1
     *
     * Needs stereo at configure(), chips start Center
     * Hard panning: Left goes to the left accumulator, Right to the right, Center to both
     * Headroom per channel (recompute_pan_headroom() in the .cpp) is the count of chips reaching it, Center counts in both
     */
    void set_pan(uint8_t chip, SidPan pan);

    /** @brief Stereo position of a chip from 1, Center if there is no such chip */
    SidPan pan(uint8_t chip) const
    {
      return (chip >= 1 && chip <= chips_) ? pan_[chip - 1] : SidPan::Center;
    }

    /**
     * @brief Set the filter of every chip
     *
     * Only changed settings are applied, a 6581 curve or range change rebuilds reSIDfp tables
     *
     * @param filter the new settings
     */
    void set_filter(const SoftSidFilter & filter);

    /** @brief Filter settings in effect */
    const SoftSidFilter & filter(void) const { return filter_; }

    /**
     * @brief Set the SID mix multiplier
     *
     * Applied after the headroom divide and before the clamp to int16, see advance()
     * 1.0 is unity, 0.0 silences the SID side. Scaled apart from set_fm_gain()
     * Not reset by configure()
     *
     * @param gain multiplier, clamped to >= 0
     */
    void set_sid_gain(float gain) { sid_gain_ = (gain < 0.0f) ? 0.0f : gain; }
    /** @brief SID mix multiplier */
    float sid_gain(void) const { return sid_gain_; }

    /**
     * @brief Set the FM/OPL multiplier, forwarded to OplChip::set_gain()
     *
     * Kept by the OplChip until the chip exists, see fm()
     *
     * @param gain multiplier, clamped to >= 0
     */
    void set_fm_gain(float gain) { fm_.set_gain(gain); }
    /** @brief FM/OPL multiplier */
    float fm_gain(void) const { return fm_.gain(); }

    /**
     * @brief Change speed via the output rate and return the multiplier in effect
     *
     * Resamples to sample_rate / mult, pitch follows speed. Chip and FM/OPL state is kept
     * Callable before configure(), kept over it
     *
     * @param mult speed multiplier, 1 is normal. Clamped to 0.1 to 8 and to an output rate of 4 kHz to 192 kHz (limit of the reSIDfp two pass sinc resampler)
     */
    double set_speed(double mult);
    /** @brief Speed multiplier in effect, the request until configure() and the clamped value after */
    double speed(void) const
    {
      return (ready_ && out_rate_ != 0)
        ? static_cast<double>(sample_rate_) / static_cast<double>(out_rate_)
        : speed_;
    }

    /**
     * @brief Attach to a machine and set access_overhead to 0
     */
    void attach(Machine & machine);

    /* ---- the seam ------------------------------------------------------- */

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    data_t read(addr_t reg, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override {}
    void reset(void) override;

    /* ---- what the audio path takes ------------------------------------- */

    /**
     * @brief Take up to frames rendered frames and return the number written
     *
     * Fewer than asked when the emulation has not run far enough, keep it ahead of the device
     * out needs room for frames * channels() int16_t, interleaved L, R, L, R... when stereo
     */
    size_t take(int16_t * out, size_t frames);

    /** @brief Rendered frames waiting */
    size_t available(void) const { return out_.size() / channels() - taken_; }

    /* ---- the oscilloscope tap ------------------------------------------ */

    /**
     * @brief Record every voice output beside the mix, or stop
     *
     * One scope frame per rendered frame, scope_voices() int16 values: chip 1 voices 1-3, chip 2 voices 1-3 etc
     * Read with take_scope() alongside take(). Off by default, kept over configure()
     *
     * @param on true to record, false to stop and drop the recording
     */
    void set_scope(bool on);

    /** @brief Is the scope tap recording? */
    bool scope(void) const { return scope_on_; }

    /** @brief Values per scope frame, three per chip */
    unsigned scope_voices(void) const { return 3u * chips_; }

    /**
     * @brief Take up to frames scope frames, oldest first, return frames written
     *
     * @param out        room for frames * voices int16_t
     * @param frames     max frames to take
     * @param voices     values to keep per frame (the first ones), clamped to scope_voices()
     */
    size_t take_scope(int16_t * out, size_t frames, unsigned voices);

    /** @brief Drop everything rendered but not taken. */
    void discard(void);

    /**
     * @brief Clock forward with no write, flushes the tail of a render
     *
     * The seam only delivers a gap attached to a write, the stretch after the last SID event is never handed over
     * A render ends up to two frames short
     * Measured on Coma_Light: 1630 samples missing after 250 frames, 1586 after 500, 1504 after 1000 (constant)
     * Compare cycles_clocked() with the cycles the emulation ran and drain the difference
     */
    void drain(uint32_t cycles) { advance(cycles); }

    /** @brief Total cycles handed to reSIDfp since the last reset. */
    uint64_t cycles_clocked(void) const { return cycles_clocked_; }

    /**
     * @brief Take register writes but synthesise nothing
     *
     * Skips a silent stretch (loader lead-in) fast. Writes reach the chips, oscillators/envelopes/filters do not clock
     * Do not use over a sounding passage. About 10x faster (3.81ms/frame vs 0.39ms), cycles_clocked() keeps counting
     */
    void set_render(bool on) { render_ = on; }

    /** @brief Is the synthesis running? */
    bool rendering(void) const { return render_; }

    /* ---- what a frontend needs to report ------------------------------- */

    /** @brief Writes seen to $df40/$df50 */
    uint64_t fm_writes(void) const { return fm_writes_; }

    /**
     * @brief The FM/OPL chip, built on the first $df40/$df50 write
     *
     * reSIDfp has no FM, those writes go to the OplChip and are summed into the samples
     */
    const OplChip & fm(void) const { return fm_; }

    /** @brief Frames synthesised since the last reset */
    uint64_t samples(void) const { return produced_; }
    /** @brief Clamped samples (one count per channel) */
    uint64_t clipped(void) const { return clipped_; }

    unsigned sample_rate(void) const { return sample_rate_; }
    uint8_t chips(void) const { return chips_; }

  private:
    /** @brief Clock every chip forward and keep the output */
    void advance(uint32_t cycles);
    /** @brief Recount chips reaching each channel, Center counts in both */
    void recompute_pan_headroom(void);

    /** @brief Output rate for speed_ at sample_rate_, clamped */
    unsigned speed_rate_(void) const;

    /**
     * @brief Apply filter settings to one chip
     *
     * @param chip  0-based chip
     * @param all   true for every setting, false for those differing from was
     * @param was   settings the chip has
     */
    void apply_filter_(uint8_t chip, bool all, const SoftSidFilter & was);

    /**
     * @brief Copy the tapped voices of a chip into the scope frames
     *
     * @param chip  0-based chip, slot in each frame
     * @param count samples the chip produced this step
     * @param base  index in scope_ of the first frame of this step
     */
    void store_scope_(uint8_t chip, int count, size_t base);

    reSIDfp::residfp * sid_[kMaxSoftSids] = {};
    uint8_t chips_ = 0;
    unsigned sample_rate_ = 0;
    /* Resample rate of chips and FM/OPL, sample_rate_ / speed_ */
    unsigned out_rate_ = 0;
    double clock_hz_ = 0.0;
    SoftSidQuality quality_ = SoftSidQuality::Good;
    /** Requested multiplier, see set_speed() */
    double speed_ = 1.0;
    bool ready_ = false;
    /* False while skipping, see set_render() */
    bool render_ = true;

    /* Stereo mix, set at configure() */
    bool stereo_ = false;
    SidPan pan_[kMaxSoftSids] = {};
    /* Chips reaching the left/right channel, Center counts in both
     * At least 1, unused when !stereo_ */
    uint8_t pan_count_l_ = 1;
    uint8_t pan_count_r_ = 1;

    /** See set_sid_gain() */
    float sid_gain_ = 1.0f;

    /** See set_filter() */
    SoftSidFilter filter_;

    /* Rendered frames, oldest first: one int16_t each if mono, an L/R pair each if stereo */
    std::vector<int16_t> out_;
    size_t taken_ = 0;
    uint64_t produced_ = 0;
    uint64_t clipped_ = 0;
    uint64_t fm_writes_ = 0;
    /* Built on the first $df40/$df50 write */
    OplChip fm_;

    uint64_t cycles_clocked_ = 0;

    /* Samples of one chip, reSIDfp returns at most one per cycle and the seam delta is 16 bit */
    std::vector<int16_t> scratch_;
    /* Mono accumulator, or the left one when stereo. mix_r_ is the right one, sized like scratch_ */
    std::vector<int32_t> mix_;
    std::vector<int32_t> mix_r_;
    /* Stereo scratch for OplChip::mix_into(), stateful, runs once per chunk
     * FM is rendered once into this (zeroed first) and added to both channels, see advance()
     * Empty when !stereo_ */
    std::vector<int16_t> fm_scratch_;

    /* Scope tap, see set_scope(). tap_ gets 3 voices of one chip per sample
     * scope_ holds frames not yet taken, scope_taken_ counts frames taken from its front */
    bool scope_on_ = false;
    std::vector<int16_t> tap_;
    std::vector<int16_t> scope_;
    size_t scope_taken_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_RESIDFP_H_ */
