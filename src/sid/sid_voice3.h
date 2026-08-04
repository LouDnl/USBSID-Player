/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_voice3.h
 * The two registers a SID can be read from: $d41b and $d41c.
 *
 * This player produces no sound of its own, it drives real chips, so there is
 * no reason to emulate a SID. There is one exception, and it is not optional:
 * $d41b is voice three's oscillator output and $d41c is its envelope, and
 * those two are the only way a C64 program can read anything back out of a
 * SID. Tunes use them constantly, as a random source, as a timer, and as a
 * "wait until the oscillator has moved" synchronisation. A tune that polls
 * $d41b and gets the same value every time waits forever, which is what three
 * of them did.
 *
 * So voice three is emulated, and only voice three: its 24 bit oscillator, its
 * noise shift register, and its envelope generator. The model is the one every
 * emulator uses, from Dag Lem's reSID.
 *
 * It is clocked lazily. Nothing here has an outside influence between one
 * access and the next, so catching up at the moment of a read gives the same
 * answer as stepping it every cycle would, at none of the cost. A write
 * catches up to the write's own cycle before it takes effect, which is what
 * keeps that true.
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
#ifndef _US_SID_SID_VOICE3_H_
#define _US_SID_SID_VOICE3_H_

#include "types.h"

namespace usbsid {

/* Voice three's registers, as offsets inside a chip's 32 byte block */
constexpr reg_t kSidRegV3FreqLo = 0x0e;
constexpr reg_t kSidRegV3FreqHi = 0x0f;
constexpr reg_t kSidRegV3PwLo   = 0x10;
constexpr reg_t kSidRegV3PwHi   = 0x11;
constexpr reg_t kSidRegV3Ctrl   = 0x12;
constexpr reg_t kSidRegV3Ad     = 0x13;
constexpr reg_t kSidRegV3Sr     = 0x14;
constexpr reg_t kSidRegOsc3     = 0x1b;
constexpr reg_t kSidRegEnv3     = 0x1c;

class SidVoice3
{
  public:
    SidVoice3(void) { reset(); }

    void reset(void);

    /** @brief A register write. Anything not voice three's is ignored. */
    void write(reg_t reg, data_t value, cycle_t now) US_RAM_ATTR;
    /** @brief $d41b, the waveform output of voice three. */
    data_t osc3(cycle_t now) US_RAM_ATTR;
    /** @brief $d41c, the envelope of voice three. */
    data_t env3(cycle_t now) US_RAM_ATTR;
    /** @brief Put the clock back in step without changing anything else. */
    void resync(cycle_t now) { last_cycle_ = now; }

  private:
    void clock_to(cycle_t now) US_RAM_ATTR;
    void clock_oscillator(uint32_t cycles) US_RAM_ATTR;
    void clock_envelope(uint32_t cycles) US_RAM_ATTR;
    void clock_noise(uint32_t shifts) US_RAM_ATTR;
    /* oscillator */
    uint32_t accumulator_ = 0;   /* 24 bit */
    uint32_t shift_ = 0x7ffff8;  /* 23 bit noise LFSR, reset value from reSID */
    uint16_t freq_ = 0;
    uint16_t pw_ = 0;
    data_t control_ = 0;

    /* envelope */
    enum class EnvState : uint8_t { Attack, DecaySustain, Release };
    EnvState env_state_ = EnvState::Release;
    uint16_t rate_counter_ = 0;
    uint16_t rate_period_ = 9;
    uint8_t envelope_ = 0;
    uint8_t exponential_counter_ = 0;
    uint8_t exponential_period_ = 1;
    bool hold_zero_ = true;
    data_t attack_decay_ = 0;
    data_t sustain_release_ = 0;

    cycle_t last_cycle_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_SID_VOICE3_H_ */
