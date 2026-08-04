/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_voice3.cpp
 *
 * The waveform generator and the envelope generator follow Dag Lem's reSID,
 * which is where every emulator's SID behaviour comes from. What is left out
 * is what a read of $d41b and $d41c cannot show: the filter, the mixer, the
 * other two voices, and voice three's sync and ring modulation inputs from
 * voice two. Those change the *sound*, and this player has real chips to make
 * the sound.
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

#include "sid_voice3.h"

namespace usbsid {

namespace {

/* Control register bits */
constexpr data_t kCtrlGate     = 0x01;
constexpr data_t kCtrlTest     = 0x08;
constexpr data_t kCtrlTriangle = 0x10;
constexpr data_t kCtrlSawtooth = 0x20;
constexpr data_t kCtrlPulse    = 0x40;
constexpr data_t kCtrlNoise    = 0x80;

/* How many cycles one step of the envelope takes, per rate nibble. From
 * reSID: these are the periods that produce the attack and decay times the
 * data sheet lists. */
constexpr uint16_t kRatePeriod[16] = {
      9,    32,    63,    95,   149,   220,   267,   313,
    392,   977,  1954,  3126,  3907, 11719, 19531, 31250
};

/**
 * @brief The decay and release curve is not linear.
 *
 * The envelope counter still steps at the rate period, but below certain
 * levels it only steps every so many of those, which is what gives decay and
 * release their exponential shape.
 */
uint8_t exponential_period_for(uint8_t envelope)
{
  switch (envelope) {
    case 0xff: return 1;
    case 0x5d: return 2;
    case 0x36: return 4;
    case 0x1a: return 8;
    case 0x0e: return 16;
    case 0x06: return 30;
    case 0x00: return 1;
    default:   return 0; /* no change at this level */
  }
}

} /* namespace */

void SidVoice3::reset(void)
{
  accumulator_ = 0;
  shift_ = 0x7ffff8;
  freq_ = 0;
  pw_ = 0;
  control_ = 0;

  env_state_ = EnvState::Release;
  rate_counter_ = 0;
  rate_period_ = kRatePeriod[0];
  envelope_ = 0;
  exponential_counter_ = 0;
  exponential_period_ = 1;
  hold_zero_ = true;
  attack_decay_ = 0;
  sustain_release_ = 0;

  last_cycle_ = 0;
}

/* ------------------------------------------------------------------------ *
 * the oscillator
 * ------------------------------------------------------------------------ */

/**
 * @brief Advance the noise shift register.
 *
 * The register is clocked once every time bit 19 of the accumulator goes from
 * zero to one, and the bit shifted in is bits 22 and 17 exclusive or'd.
 */
void SidVoice3::clock_noise(uint32_t shifts)
{
  for (uint32_t i = 0; i < shifts; i++) {
    const uint32_t bit0 = ((shift_ >> 22) ^ (shift_ >> 17)) & 0x01;
    shift_ = ((shift_ << 1) & 0x7fffff) | bit0;
  }
}

void SidVoice3::clock_oscillator(uint32_t cycles)
{
  /* The test bit holds the accumulator at zero and fills the shift register */
  if ((control_ & kCtrlTest) != 0) {
    accumulator_ = 0;
    shift_ = 0x7ffff8;
    return;
  }
  if (freq_ == 0) return;

  /* The accumulator is 24 bits and adds the frequency once a cycle, so a gap
   * of any length is one multiply. How many times bit 19 turned over in that
   * gap is the difference of the two counts, which is the number of times the
   * noise register was clocked. */
  const uint64_t before = accumulator_;
  const uint64_t after = before + static_cast<uint64_t>(freq_) * cycles;

  const uint64_t shifts = (after >> 19) - (before >> 19);
  accumulator_ = static_cast<uint32_t>(after & 0xffffff);

  if ((control_ & kCtrlNoise) != 0 && shifts != 0) {
    clock_noise(static_cast<uint32_t>(shifts));
  }
}

/* ------------------------------------------------------------------------ *
 * the envelope
 * ------------------------------------------------------------------------ */

void SidVoice3::clock_envelope(uint32_t cycles)
{
  while (cycles != 0) {
    /* Jump straight to the next step of the rate counter rather than counting
     * cycles one at a time. The result is the same and a silent stretch of a
     * whole frame costs a couple of thousand steps instead of twenty. */
    const uint32_t to_step = static_cast<uint32_t>(rate_period_ - rate_counter_);
    if (cycles < to_step) {
      rate_counter_ = static_cast<uint16_t>(rate_counter_ + cycles);
      return;
    }
    cycles -= to_step;
    rate_counter_ = 0;

    if (env_state_ == EnvState::Attack) {
      /* attack is linear and ignores the exponential counter */
      if (envelope_ < 0xff) {
        ++envelope_;
        if (envelope_ == 0xff) {
          env_state_ = EnvState::DecaySustain;
          rate_period_ = kRatePeriod[attack_decay_ & 0x0f];
        }
      }
      exponential_counter_ = 0;
      exponential_period_ = 1;
      continue;
    }

    if (hold_zero_) continue;

    ++exponential_counter_;
    if (exponential_counter_ < exponential_period_) continue;
    exponential_counter_ = 0;

    const uint8_t sustain =
      static_cast<uint8_t>((sustain_release_ >> 4) * 0x11);

    if (env_state_ == EnvState::DecaySustain && envelope_ == sustain) continue;

    if (envelope_ != 0) {
      --envelope_;
      const uint8_t period = exponential_period_for(envelope_);
      if (period != 0) exponential_period_ = period;
      if (envelope_ == 0) hold_zero_ = true;
    }
  }
}

/* ------------------------------------------------------------------------ *
 * access
 * ------------------------------------------------------------------------ */

void SidVoice3::clock_to(cycle_t now)
{
  if (now <= last_cycle_) { last_cycle_ = now; return; }

  const uint64_t delta = static_cast<uint64_t>(now - last_cycle_);
  last_cycle_ = now;

  /* A gap longer than this is a tune that has not touched voice three for
   * more than a minute. The oscillator is exact whatever the gap, and the
   * envelope has long since reached where it is going, so clamping the
   * envelope's catch up costs nothing and bounds the work. */
  constexpr uint64_t kMaxEnvelopeCatchup = 2ull * 1000ull * 1000ull;

  clock_oscillator(static_cast<uint32_t>(delta & 0xffffffff));
  if (delta > 0xffffffffull) {
    /* the oscillator addition is modular, so the high part folds in cleanly */
    clock_oscillator(static_cast<uint32_t>(delta >> 32));
  }

  clock_envelope(static_cast<uint32_t>(
    (delta > kMaxEnvelopeCatchup) ? kMaxEnvelopeCatchup : delta));
}

void SidVoice3::write(reg_t reg, data_t value, cycle_t now)
{
  switch (reg) {
    case kSidRegV3FreqLo:
    case kSidRegV3FreqHi:
    case kSidRegV3PwLo:
    case kSidRegV3PwHi:
    case kSidRegV3Ctrl:
    case kSidRegV3Ad:
    case kSidRegV3Sr:
      break;
    default:
      return; /* not ours */
  }

  clock_to(now);

  switch (reg) {
    case kSidRegV3FreqLo:
      freq_ = static_cast<uint16_t>((freq_ & 0xff00) | value);
      break;
    case kSidRegV3FreqHi:
      freq_ = static_cast<uint16_t>((freq_ & 0x00ff) | (value << 8));
      break;
    case kSidRegV3PwLo:
      pw_ = static_cast<uint16_t>((pw_ & 0x0f00) | value);
      break;
    case kSidRegV3PwHi:
      pw_ = static_cast<uint16_t>((pw_ & 0x00ff) | ((value & 0x0f) << 8));
      break;

    case kSidRegV3Ctrl: {
      const bool was_gated = (control_ & kCtrlGate) != 0;
      const bool now_gated = (value & kCtrlGate) != 0;
      control_ = value;

      if (!was_gated && now_gated) {
        env_state_ = EnvState::Attack;
        rate_period_ = kRatePeriod[(attack_decay_ >> 4) & 0x0f];
        hold_zero_ = false;
      } else if (was_gated && !now_gated) {
        env_state_ = EnvState::Release;
        rate_period_ = kRatePeriod[sustain_release_ & 0x0f];
      }

      if ((control_ & kCtrlTest) != 0) {
        accumulator_ = 0;
        shift_ = 0x7ffff8;
      }
      break;
    }

    case kSidRegV3Ad:
      attack_decay_ = value;
      if (env_state_ == EnvState::Attack) {
        rate_period_ = kRatePeriod[(value >> 4) & 0x0f];
      } else if (env_state_ == EnvState::DecaySustain) {
        rate_period_ = kRatePeriod[value & 0x0f];
      }
      break;

    case kSidRegV3Sr:
      sustain_release_ = value;
      if (env_state_ == EnvState::Release) {
        rate_period_ = kRatePeriod[value & 0x0f];
      }
      break;

    default:
      break;
  }
}

data_t SidVoice3::osc3(cycle_t now)
{
  clock_to(now);

  const data_t wave = static_cast<data_t>(control_ & 0xf0);
  if (wave == 0) return 0;

  /* Each waveform is the top eight bits of what the accumulator drives. Two
   * waveforms at once are wired together on the chip and come out as
   * something close to their bitwise and, which is what every emulator uses
   * and what a program reading $d41b sees. */
  data_t value = 0xff;

  if ((wave & kCtrlTriangle) != 0) {
    uint32_t msb_xor = ((accumulator_ & 0x800000) != 0) ? 0xffffff : 0;
    const uint32_t tri = (accumulator_ ^ msb_xor) & 0x7fffff;
    value = static_cast<data_t>(value & static_cast<data_t>((tri >> 15) & 0xff));
  }
  if ((wave & kCtrlSawtooth) != 0) {
    value = static_cast<data_t>(value & static_cast<data_t>(accumulator_ >> 16));
  }
  if ((wave & kCtrlPulse) != 0) {
    const uint16_t upper = static_cast<uint16_t>((accumulator_ >> 12) & 0xfff);
    value = static_cast<data_t>(
      value & ((upper >= (pw_ & 0x0fff)) ? 0xff : 0x00));
  }
  if ((wave & kCtrlNoise) != 0) {
    const data_t noise = static_cast<data_t>(
      (((shift_ >> 22) & 0x01) << 7) | (((shift_ >> 20) & 0x01) << 6) |
      (((shift_ >> 16) & 0x01) << 5) | (((shift_ >> 13) & 0x01) << 4) |
      (((shift_ >> 11) & 0x01) << 3) | (((shift_ >>  7) & 0x01) << 2) |
      (((shift_ >>  4) & 0x01) << 1) | (((shift_ >>  2) & 0x01) << 0));
    value = static_cast<data_t>(value & noise);
  }

  return value;
}

data_t SidVoice3::env3(cycle_t now)
{
  clock_to(now);
  return envelope_;
}

} /* namespace usbsid */
