/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_residfp.h
 * A SidBackend that synthesises audio with reSIDfp instead of driving a board.
 *
 * This is the seam working as intended: the emulation does not know or care
 * that nothing is being written to hardware. What arrives here is exactly what
 * arrives at the USBSID-Pico backend, and the only reason this file is more
 * than twenty lines is that reSIDfp wants the same information in a different
 * shape.
 *
 * Three things about that shape, each of which is somewhere an off by one would
 * hide:
 *
 * 1. **The access cycle must not be deducted.** Every hardware backend is
 *    handed `cycles` with one already taken off, because performing the access
 *    on USBSID-Pico costs a cycle of bus time (`SidConfig::access_overhead`).
 *    A software SID has no bus and no such cycle. So a player using this
 *    backend must set `access_overhead = 0`, and `attach()` does it rather than
 *    leaving it to the caller to remember. Getting this wrong is inaudible on
 *    one write and is the tempo across a frame of a digi tune.
 *
 * 2. **reSIDfp wants the delta before the write, separately.** The seam hands
 *    over a gap *with* a write, meaning "this write happens `cycles` after the
 *    last event". reSIDfp wants `clock(cycles)` and then `write(reg, value)`.
 *    Same information, opposite order, and the ordering is what makes a write
 *    land on the right cycle.
 *
 * 3. **The chip is folded into the register.** `reg` is physical: `$000-$01f` is
 *    chip 1, `$020-$03f` chip 2, up to `$1c0-$1df` for chip kMaxSoftSids (15).
 *    `reg >> 5` picks the instance and `reg & 0x1f` the register. Every
 *    configured chip is clocked by the same delta whether or not it was
 *    written to, because they all run off the same phi2 whatever the tune is
 *    doing. `reg` is 16 bit (addr_t), not 8, because 15 chips no longer fit
 *    the top 3 bits of a byte the way the 4 this backend used to be capped at
 *    did - see SidBackend::write()'s own comment.
 *
 * What this cannot do: **FM/OPL**. `kFmOplParkBase`'s two addresses
 * (mos6581_8580.cpp) reach a backend when a tune writes `$df40`/`$df50` and
 * no SID claims them - `reg >= kFmOplRange` below, chosen to sit past even
 * the widest real chip range so it can never be mistaken for one. reSIDfp has
 * no FM, so those writes are counted and dropped, and `fm_writes()` is non
 * zero afterwards so a frontend can say so once rather than being
 * mysteriously quiet.
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

/** @brief How many chips this backend will synthesise at once. Matches
 * kMaxSids (sidfile.h), the most a v5 tune's own multiSidConfig can ask
 * for: this is the one backend "playing SIDs" is actually meant to expand
 * to more than 4 for (CLI and Web audio) - a real board (UsbSidBackend,
 * EmbeddedSidBackend, WebSidBackend) stays at its own physical 4 sockets. */
constexpr uint8_t kMaxSoftSids = kMaxSids;

/** @brief Which resampler, mapped onto reSIDfp's own enum by the .cpp. */
enum class SoftSidQuality : uint8_t {
  Fast,      /* linear interpolation, DECIMATE */
  Good,      /* sinc, RESAMPLE. The expensive one and the right one */
};

/** @brief Which chip to model. Taken from the tune's header when it says. */
enum class SoftSidModel : uint8_t { Mos6581, Csg8580 };

/**
 * @brief Synthesises what the emulation writes, instead of sending it anywhere.
 */
class ResidFpSidBackend final : public SidBackend
{
  public:
    ResidFpSidBackend(void);
    ~ResidFpSidBackend(void) override;

    ResidFpSidBackend(const ResidFpSidBackend &) = delete;
    ResidFpSidBackend & operator=(const ResidFpSidBackend &) = delete;

    /**
     * @brief Build the chips and set the sample rate.
     *
     * @param chips        1 to kMaxSoftSids
     * @param clock_hz     the tune's C64 clock, 985248 PAL or 1022727 NTSC
     * @param sample_rate  the device's rate, not a wish: on a phone it is fixed
     * @param quality      Fast or Good, see SoftSidQuality
     * @param model        which chip to model
     * @param stereo       opt in to a stereo mix, see set_pan(). Off by
     *                     default: every existing caller of this backend
     *                     (WAV, the live audio device, the web ring) assumed
     *                     one channel, and changing that under them without
     *                     asking would silently double their buffer's real
     *                     meaning. Every chip starts out Center either way,
     *                     which is an ordinary mono mix panned to both
     *                     channels, so turning this on with no set_pan()
     *                     calls is harmless on its own.
     * @returns false if reSIDfp would not accept the parameters, in which case
     *          nothing is configured and this backend must not be used
     */
    bool configure(uint8_t chips, double clock_hz, unsigned sample_rate,
                   SoftSidQuality quality, SoftSidModel model,
                   bool stereo = false);

    /** @brief Is it configured and safe to attach? */
    bool ready(void) const { return ready_; }

    /** @brief Whether this instance is mixing to one channel or two. */
    bool stereo(void) const { return stereo_; }

    /** @brief Samples per frame: 1 mono, 2 stereo. What take()'s caller must
     * scale its own buffer and frame count by. */
    unsigned channels(void) const { return stereo_ ? 2u : 1u; }

    /**
     * @brief Where one chip sits in the stereo mix. Chip counts from 1.
     *
     * No effect at all unless `stereo` was true at configure() - see its own
     * comment. Every chip starts Center (the v5 SID file format's own
     * default hint, sidfile.h's SidPan, when a tune says nothing) until this
     * is called, so a stereo-configured backend nobody calls this on still
     * plays as an ordinary center mix on both channels.
     *
     * Hard panning, not a gain law: Left goes only to the left accumulator,
     * Right only to the right, Center to both - the same three positions a
     * v5 tune's own multiSidConfig panning hint can ask for (see
     * compute_panning(), sidfile.cpp), so there is no finer position to
     * represent. Per channel headroom (recompute_pan_headroom() in the .cpp)
     * is the count of chips actually reaching that channel, Center counted
     * in both, not chips_ itself - an L/R/L/R spread should not get quieter
     * on either side just because a third, Center chip exists.
     */
    void set_pan(uint8_t chip, SidPan pan);

    /**
     * @brief Attach to a machine, setting what this backend needs of it.
     *
     * Sets `access_overhead` to 0, which is the one part of the contract that
     * is not the same as a hardware backend's. See the header comment.
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
     * @brief Take up to `frames` rendered frames.
     *
     * Whatever has been synthesised so far and not yet taken. Returns how many
     * frames were written, which is fewer than asked for when the emulation
     * has not run far enough yet: it is the caller's job to keep the
     * emulation ahead of the device, exactly as it is with a board.
     *
     * `out` must have room for `frames * channels()` int16_t: one sample per
     * frame mono, interleaved L, R, L, R... stereo. Unchanged shape from
     * before stereo existed as long as the caller was already sizing by
     * channels() (1 then, same as now for a mono-configured instance).
     */
    size_t take(int16_t * out, size_t frames);

    /** @brief How many rendered frames are waiting. */
    size_t available(void) const { return produced_ - taken_; }

    /** @brief Drop everything rendered but not taken. */
    void discard(void);

    /**
     * @brief Clock forward with no write, to flush the tail of a render.
     *
     * The seam only ever delivers a gap attached to a write, so the stretch
     * between a tune's last SID event and wherever the emulation stopped is
     * never handed over: there is no write to hang it on. That is correct for
     * live playback, where the stream simply ends, and it leaves a render up to
     * two frames short of the time it was asked for.
     *
     * Measured on Coma_Light: 1630 samples missing after 250 frames, 1586 after
     * 500, 1504 after 1000. **Constant, not scaling**, which is what says it is
     * the tail and not a per write cycle leak. A leak would have been the
     * `access_overhead` mistake this backend's header warns about.
     *
     * A frontend that cares compares `cycles_clocked()` against the cycles it
     * knows the emulation ran and drains the difference.
     */
    void drain(uint32_t cycles) { advance(cycles); }

    /** @brief Total cycles handed to reSIDfp since the last reset. */
    uint64_t cycles_clocked(void) const { return cycles_clocked_; }

    /**
     * @brief Take the register writes but do not synthesise anything.
     *
     * For skipping a silent stretch (e.g. a loader's lead-in) fast: writes
     * still reach the chips, but oscillators/envelopes/filters don't clock,
     * so no state is lost. Do not use this over a passage that is sounding.
     * ~10x faster than synthesising (3.81ms/frame vs 0.39ms, measured).
     * `cycles_clocked()` keeps counting regardless.
     */
    void set_render(bool on) { render_ = on; }

    /** @brief Is the synthesis running? */
    bool rendering(void) const { return render_; }

    /* ---- what a frontend needs to report ------------------------------- */

    /** @brief Writes to $df40/$df50 that reSIDfp cannot make a sound for. */
    uint64_t fm_writes(void) const { return fm_writes_; }

    /**
     * @brief The FM/OPL chip, built the first time a tune writes to one.
     *
     * reSIDfp has no FM; writes to $df40/$df50 are routed to an owned OplChip
     * and summed into this backend's own samples, so every frontend gets FM
     * without knowing about it.
     */
    const OplChip & fm(void) const { return fm_; }

    /** @brief Frames synthesised since the last reset (see take()'s own
     * frame/channel distinction). */
    uint64_t samples(void) const { return produced_; }
    /** @brief Samples (not frames - one count per channel) that had to be
     * clamped, so clipping is not silent. */
    uint64_t clipped(void) const { return clipped_; }

    unsigned sample_rate(void) const { return sample_rate_; }
    uint8_t chips(void) const { return chips_; }

  private:
    /** @brief Clock every chip forward and keep what came out. */
    void advance(uint32_t cycles);
    /** @brief Recount how many configured chips reach each channel, after
     * configure() or set_pan() changes what pan_[] or chips_ says. Center
     * counts toward both - see set_pan()'s own comment on why. */
    void recompute_pan_headroom(void);

    reSIDfp::residfp * sid_[kMaxSoftSids] = {};
    uint8_t chips_ = 0;
    unsigned sample_rate_ = 0;
    bool ready_ = false;
    /* False while a stretch is being run through with nothing synthesised,
     * see set_render(). */
    bool render_ = true;

    /* Opt-in stereo mix, set once at configure() time - see its own comment
     * on why this is not a runtime toggle. */
    bool stereo_ = false;
    SidPan pan_[kMaxSoftSids] = {};
    /* How many configured chips reach the left/right channel respectively,
     * Center counted in both - the per-channel headroom divisor advance()
     * uses. Always at least 1. Meaningless, and unused, when !stereo_. */
    uint8_t pan_count_l_ = 1;
    uint8_t pan_count_r_ = 1;

    /* Rendered frames, oldest first: one int16_t each if mono, an
     * interleaved L/R pair each if stereo (see channels()). A vector and not
     * a fixed ring: the CLI drains this from a device callback at whatever
     * size the device asks for, and a tune stepped a frame at a time
     * produces about 800 frames a frame, so the sizes involved are small and
     * predictable. */
    std::vector<int16_t> out_;
    size_t taken_ = 0;
    uint64_t produced_ = 0;
    uint64_t clipped_ = 0;
    uint64_t fm_writes_ = 0;
    /* Built on the first $df40/$df50 write and not before: most tunes have no
     * FM side and should pay nothing for one. */
    OplChip fm_;

    uint64_t cycles_clocked_ = 0;

    /* One chip's worth of samples, reused, so no allocation happens on the
     * write path. reSIDfp returns at most one sample per cycle it is clocked
     * and the seam's delta is sixteen bits, so this is the worst case. */
    std::vector<int16_t> scratch_;
    /* Mono accumulator (!stereo_) or, stereo, the left one; mix_r_ is the
     * right one and is only ever touched when stereo_. Same kMaxDelta sizing
     * as scratch_ above. */
    std::vector<int32_t> mix_;
    std::vector<int32_t> mix_r_;
    /* Stereo-only scratch for OplChip::mix_into(), which mixes into a
     * contiguous mono buffer in place and is stateful - it generates new OPL
     * samples on every call, so it must run exactly once per chunk, never
     * once per channel. FM is rendered once, into this (zeroed first), then
     * that one mono result is added into both already-mixed SID channels by
     * hand - see advance(). Unused, and left empty, when !stereo_, where
     * mix_into() runs directly on out_ exactly as it always did. */
    std::vector<int16_t> fm_scratch_;
};

} /* namespace usbsid */

#endif /* _US_SID_RESIDFP_H_ */
