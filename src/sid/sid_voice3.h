/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_voice3.h
 * The two registers a SID can be read from: $d41b and $d41c
 *
 * The player drives real chips and emulates no SID, except voice three
 * $d41b is the oscillator output and $d41c the envelope of voice three, the
 * only way a C64 program reads anything back from a SID
 * Tunes use them as random source, timer and "wait until the oscillator has moved"
 * A tune polling $d41b that gets the same value every time waits forever
 *
 * Emulated are the 24 bit oscillator, the noise shift register and the envelope
 * generator, the reSID model by Dag Lem
 *
 * Clocked lazily, nothing influences it between two accesses, catching up at
 * a read gives the answer of stepping every cycle at none of the cost
 * A write first catches up to its own cycle
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

    /** @brief A register write, anything not voice three is ignored */
    void write(reg_t reg, data_t value, cycle_t now) US_RAM_ATTR;
    /** @brief A register read, everything except 1b/1c is ignored */
    data_t read(reg_t reg, cycle_t now) US_RAM_ATTR;
    /** @brief $d41b, the waveform output of voice three */
    data_t osc3(cycle_t now) US_RAM_ATTR;
    /** @brief $d41c, the envelope of voice three */
    data_t env3(cycle_t now) US_RAM_ATTR;
    /** @brief Put the clock back in step, nothing else changes */
    void resync(cycle_t now) { last_cycle_ = now; }

  private:
    struct Osc {
      uint32_t freq = 0; /* 16 bit */
      uint32_t acc = 0;  /* 24 bit */
      data_t ctrl = 0;
      bool test(void) const { return (ctrl & 0x08) != 0; }
    };

    void clock_to(cycle_t now) US_RAM_ATTR;
    void clock(uint32_t cycles) US_RAM_ATTR;
    void clock_waveforms(uint32_t cycles) US_RAM_ATTR;
    void clock_oscillators(uint32_t cycles) US_RAM_ATTR;
    void clock_noise(uint32_t cycles) US_RAM_ATTR;
    void shift_lfsr(uint32_t feedback) US_RAM_ATTR;
    void discharge_noise(void) US_RAM_ATTR;
    void write_control(data_t value) US_RAM_ATTR;
    bool sync_active(void) const US_RAM_ATTR;
    bool combined_noise_active(void) const US_RAM_ATTR;
    uint32_t noise_bits(void) const US_RAM_ATTR;
    data_t osc3_output(void) const US_RAM_ATTR;
    void clock_envelope(uint32_t cycles) US_RAM_ATTR;
    void step_envelope(void) US_RAM_ATTR;

    /* oscillators, voice one and two only for sync and ring modulation */
    Osc osc_[3];
    uint32_t shift_ = 0x7ffffc;  /* 23 bit noise LFSR */
    uint16_t pw_ = 0;            /* voice three only */
    uint32_t noise_pipeline_ = 0;
    uint32_t noise_reset_cycles_ = 0;

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
