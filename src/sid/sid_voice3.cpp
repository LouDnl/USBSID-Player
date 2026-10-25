/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_voice3.cpp
 *
 * Waveform and envelope generator follow Dag Lem's reSID based of off WilfredC64's code.
 * Left out are the parts a read of $d41b and $d41c cannot show: the filter, the mixer,
 * the other two voices and the sync and ring modulation inputs of voice three
 * from voice two. Those only change the sound, real chips make that
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
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

#include "sid_voice3.h"

namespace usbsid {

namespace {

/* Control register bits */
constexpr data_t kCtrlGate     = 0x01;
constexpr data_t kCtrlSync     = 0x02;
constexpr data_t kCtrlRing     = 0x04;
constexpr data_t kCtrlTest     = 0x08;

constexpr int kVoice1 = 0;
constexpr int kVoice2 = 1;
constexpr int kVoice3 = 2;

constexpr uint32_t kBit19 = 1u << 19;
constexpr uint32_t kBit23 = 1u << 23;
constexpr uint32_t kAccSize = 1u << 24;
constexpr uint32_t kAccMask = kAccSize - 1;
constexpr uint64_t kNoisePhaseSize = 1ull << 20;
constexpr uint8_t kNoiseTaps[8] = { 22, 20, 16, 13, 11, 7, 4, 2 };
constexpr uint32_t kLfsrMask = 0x7fffff;
constexpr uint32_t kLfsrInitial = 0x7ffffc;
constexpr uint32_t kNoiseResetCycles = 0x8000;
constexpr uint32_t kWaveMask = 0xfff;

/* The rate counter is 15 bits, its wrap sequence skips zero */
constexpr uint32_t kRateCounterLimit  = 0x8000;
constexpr uint32_t kRateCounterPeriod = kRateCounterLimit - 1;

/* Cycles per envelope step per rate nibble, from reSID (periods that give the data sheet attack and decay times) */
constexpr uint16_t kRatePeriod[16] = {
    0x0009, // ~2ms
    0x0020, // ~8ms
    0x003f, // ~16ms
    0x005f, // ~24ms
    0x0095, // ~38ms
    0x00dc, // ~56ms
    0x010b, // ~68ms
    0x0139, // ~80ms
    0x0188, // ~100ms
    0x03d1, // ~250ms
    0x07a2, // ~500ms
    0x0c36, // ~800ms
    0x0f43, // ~1s
    0x2dc8, // ~3s
    0x4c4c, // ~5s
    0x7a13, // ~8s
};


} /* namespace */

void SidVoice3::reset(void)
{
  for (Osc &o : osc_) o = Osc();
  shift_ = kLfsrInitial;
  pw_ = 0;
  noise_pipeline_ = 0;
  noise_reset_cycles_ = 0;

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

/* Oscillator */

void SidVoice3::shift_lfsr(uint32_t feedback)
{
  shift_ = ((shift_ << 1) | feedback) & kLfsrMask;
}

bool SidVoice3::sync_active(void) const
{
  return ((osc_[kVoice1].ctrl | osc_[kVoice2].ctrl | osc_[kVoice3].ctrl)
          & kCtrlSync) != 0;
}

bool SidVoice3::combined_noise_active(void) const
{
  return (osc_[kVoice3].ctrl & 0xf0) > 0x80
      && !osc_[kVoice3].test()
      && shift_ != 0;
}

/**
 * @brief Advance the noise shift register
 *
 * Shifts on each rising edge of accumulator bit 19, two cycles late
 * Test held long enough fills the register with ones
 */
void SidVoice3::clock_noise(uint32_t cycles)
{
  const Osc &v3 = osc_[kVoice3];
  if (v3.test()) {
    if (noise_reset_cycles_ != 0) {
      noise_reset_cycles_ =
        (noise_reset_cycles_ > cycles) ? noise_reset_cycles_ - cycles : 0;
      if (noise_reset_cycles_ == 0) shift_ = kLfsrMask;
    }
    return;
  }

  uint64_t shifts = 0;
  if (noise_pipeline_ != 0) {
    if (noise_pipeline_ <= cycles) {
      shifts++;
      noise_pipeline_ = 0;
    } else {
      noise_pipeline_ -= cycles;
    }
  }

  if (v3.freq != 0) {
    const uint64_t phase = v3.acc & (kBit19 * 2 - 1);
    const uint64_t freq = v3.freq;
    const uint64_t half = kBit19;
    /* Bit 19 rising edges in n cycles, multiple wraps included */
    auto edges = [&](uint32_t n) -> uint64_t {
      return (phase + freq * n + half) / kNoisePhaseSize
           - (phase + half) / kNoisePhaseSize;
    };
    const uint64_t completed = edges((cycles > 2) ? cycles - 2 : 0);
    shifts += completed;
    const uint64_t all = edges(cycles);
    if (all != completed) {
      /* At most one edge fits in the final two cycles, FREQ is 16 bits */
      noise_pipeline_ = (all != edges(cycles - 1)) ? 2 : 1;
    }
  }

  for (uint64_t i = 0; i < shifts; i++) {
    shift_lfsr(((shift_ >> 22) ^ (shift_ >> 17)) & 0x01);
  }
}

void SidVoice3::clock_waveforms(uint32_t cycles)
{
  clock_noise(cycles);
  for (Osc &o : osc_) {
    if (!o.test()) o.acc = (o.acc + o.freq * cycles) & kAccMask;
  }
}

/**
 * @brief Clock with hard sync and combined noise feedback
 *
 * Stops at each MSB edge that can sync a destination, the source ring is
 * 3 -> 1 -> 2 -> 3. Combined noise feeds the output back each cycle
 */
void SidVoice3::clock_oscillators(uint32_t cycles)
{
  while (cycles != 0) {
    const Osc old[3] = { osc_[0], osc_[1], osc_[2] };
    uint32_t step = cycles;
    uint32_t rising_in[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };

    for (int src = 0; src < 3; src++) {
      if ((old[(src + 1) % 3].ctrl & kCtrlSync) != 0
          && !old[src].test() && old[src].freq != 0) {
        const uint32_t distance = (old[src].acc < kBit23)
          ? kBit23 - old[src].acc
          : kAccSize + kBit23 - old[src].acc;
        rising_in[src] = (distance + old[src].freq - 1) / old[src].freq;
        if (rising_in[src] < step) step = rising_in[src];
      }
    }

    const bool feedback = combined_noise_active();
    if (feedback) step = 1;

    clock_waveforms(step);

    /* All edges first, a source synced on this same edge resets nothing */
    for (int src = 0; src < 3; src++) {
      const int dst = (src + 1) % 3;
      const int prev = (src + 2) % 3;
      if (rising_in[src] == step
          && (old[dst].ctrl & kCtrlSync) != 0
          && !((old[src].ctrl & kCtrlSync) != 0 && rising_in[prev] == step)) {
        osc_[dst].acc = 0;
      }
    }

    if (feedback) discharge_noise();
    cycles -= step;
  }
}

/* Waveform bits that read low discharge their noise cells, noise can lock up */
void SidVoice3::discharge_noise(void)
{
  const data_t out = osc3_output();
  for (int bit = 0; bit < 8; bit++) {
    if ((out & (0x80 >> bit)) == 0) shift_ &= ~(1u << kNoiseTaps[bit]);
  }
}

/* Envelope */

void SidVoice3::clock_envelope(uint32_t cycles)
{
  /* Writes never reset the rate counter. If the new comparison value
   * has already been passed, wait for the 15 bit counter to wrap.
   * Its wrap sequence skips zero, zero is used only on a rate match */
  const uint32_t distance = (rate_counter_ < rate_period_)
    ? rate_period_ - rate_counter_
    : kRateCounterPeriod - rate_counter_ + rate_period_;
  if (cycles < distance) {
    const uint32_t counter = rate_counter_ + cycles;
    rate_counter_ = (counter >= kRateCounterLimit)
      ? counter - kRateCounterPeriod
      : counter;
    return;
  }
  uint32_t remaining = cycles - distance;
  rate_counter_ = 0;
  for (;;) {
    step_envelope();
    /* A step may switch attack to decay and change the rate */
    const uint32_t period = rate_period_;
    if (remaining < period) {
      rate_counter_ = remaining;
      break;
    }
    const uint8_t sustain = static_cast<uint8_t>((sustain_release_ >> 4) * 0x11);
    if (hold_zero_ ||
        (env_state_ == EnvState::DecaySustain && envelope_ == sustain)) {
      /* Keep the divider phase even while the output holds at sustain */
      const uint32_t events = remaining / period;
      if (env_state_ != EnvState::Attack) {
        exponential_counter_ =
          (exponential_counter_ + events) % exponential_period_;
      }
      rate_counter_ = remaining % period;
      break;
    }
    remaining -= period;
  }
}

void SidVoice3::step_envelope(void)
{
  if (env_state_ == EnvState::Attack) {
    /* Reset on an actual attack step, not on a gate write */
    exponential_counter_ = 0;
  } else {
    ++exponential_counter_;
    if (exponential_counter_ != exponential_period_) return;
    exponential_counter_ = 0;
  }
  if (hold_zero_) return;

  const uint8_t sustain = static_cast<uint8_t>((sustain_release_ >> 4) * 0x11);
  if (env_state_ == EnvState::Attack) {
    envelope_ = static_cast<uint8_t>(envelope_ + 1);
    if (envelope_ == 0xff) {
      env_state_ = EnvState::DecaySustain;
      rate_period_ = kRatePeriod[attack_decay_ & 0x0f];
    }
  } else if (env_state_ == EnvState::Release || envelope_ != sustain) {
    envelope_ = static_cast<uint8_t>(envelope_ - 1);
  } else {
    return;
  }

  /* These are latches, not ranges: releasing during attack keeps
   * the last threshold crossed while the envelope was rising */
  switch (envelope_) {
    case 0xff:
    case 0x00: exponential_period_ = 1;  break;
    case 0x5d: exponential_period_ = 2;  break;
    case 0x36: exponential_period_ = 4;  break;
    case 0x1a: exponential_period_ = 8;  break;
    case 0x0e: exponential_period_ = 16; break;
    case 0x06: exponential_period_ = 30; break;
    default: break;
  }
  if (envelope_ == 0) hold_zero_ = true;
}

/* Access */

void SidVoice3::clock(uint32_t cycles)
{
    if (!sync_active() && !combined_noise_active()) {
    clock_waveforms(cycles);
  } else {
    clock_oscillators(cycles);
  }
  clock_envelope(cycles);
}

void SidVoice3::clock_to(cycle_t now)
{
  if (now <= last_cycle_) { last_cycle_ = now; return; }

  uint64_t delta = static_cast<uint64_t>(now - last_cycle_);
  last_cycle_ = now;

  while (delta != 0) {
    const uint32_t cycles = (delta > 0xffffffffull)
      ? 0xffffffffu : static_cast<uint32_t>(delta);
    clock(cycles);
    delta -= cycles;
  }
}

void SidVoice3::write(reg_t reg, data_t value, cycle_t now)
{
  /* The three voices share a seven register layout, only voice three
   * needs pulse width and envelope for the readable outputs */
  if (reg >= 0x15) return;

  clock_to(now);

  const int voice = reg / 7;
  const int offset = reg % 7;
  Osc &o = osc_[voice];

  switch (offset) {
    case 0:
      o.freq = (o.freq & 0xff00) | value;
      return;
    case 1:
      o.freq = (o.freq & 0x00ff) | (static_cast<uint32_t>(value) << 8);
      return;
    default:
      break;
  }

  if (voice != kVoice3) {
    if (offset == 4) {
      o.ctrl = value;
      if (o.test()) o.acc = 0;
    }
    return;
  }

  switch (offset) {
    case 2:
      pw_ = static_cast<uint16_t>((pw_ & 0x0f00) | value);
      break;
    case 3:
      pw_ = static_cast<uint16_t>((pw_ & 0x00ff) | ((value & 0x0f) << 8));
      break;
    case 4:
      write_control(value);
      break;
    case 5:
      attack_decay_ = value;
      if (env_state_ == EnvState::Attack) {
        rate_period_ = kRatePeriod[(value >> 4) & 0x0f];
      } else if (env_state_ == EnvState::DecaySustain) {
        rate_period_ = kRatePeriod[value & 0x0f];
      }
      break;
    case 6:
      sustain_release_ = value;
      if (env_state_ == EnvState::Release) {
        rate_period_ = kRatePeriod[value & 0x0f];
      }
      break;
    default:
      break;
  }
}

void SidVoice3::write_control(data_t value)
{
  Osc &v3 = osc_[kVoice3];

  const bool was_gated = (v3.ctrl & kCtrlGate) != 0;
  const bool now_gated = (value & kCtrlGate) != 0;
  if (!was_gated && now_gated) {
    env_state_ = EnvState::Attack;
    rate_period_ = kRatePeriod[(attack_decay_ >> 4) & 0x0f];
    hold_zero_ = false;
  } else if (was_gated && !now_gated) {
    env_state_ = EnvState::Release;
    rate_period_ = kRatePeriod[sustain_release_ & 0x0f];
  }

  const bool old_test = v3.test();
  v3.ctrl = value;
  const bool new_test = (value & kCtrlTest) != 0;

  if (!old_test && new_test) {
    v3.acc = 0;
    noise_pipeline_ = 0;
    /* Sustained test recovery approximated by a single reset */
    noise_reset_cycles_ = kNoiseResetCycles;
  } else if (old_test && !new_test) {
    shift_lfsr((~shift_ >> 17) & 0x01);
    noise_reset_cycles_ = 0;
  }
}

uint32_t SidVoice3::noise_bits(void) const
{
  uint32_t noise = 0;
  for (int bit = 0; bit < 8; bit++) {
    noise |= ((shift_ >> kNoiseTaps[bit]) & 0x01) << (11 - bit);
  }
  return noise;
}

data_t SidVoice3::osc3_output(void) const
{
  const Osc &v3 = osc_[kVoice3];
  const uint32_t waveform = v3.ctrl >> 4;
  if (waveform == 0) return 0;

  const uint32_t acc = v3.acc;
  uint32_t result = kWaveMask;

  if ((waveform & 1) != 0) {
    /* Saw selection disables the triangle MSB folding, ring included */
    const uint32_t ring = ((v3.ctrl & kCtrlRing) != 0) ? osc_[kVoice2].acc : 0;
    const bool invert = (waveform & 2) == 0 && ((acc ^ ring) & kBit23) != 0;
    const uint32_t tri = invert ? ~acc : acc;
    result &= (tri >> 11) & 0xffe;
  }
  if ((waveform & 2) != 0) {
    result &= acc >> 12;
  }
  if ((waveform & 4) != 0 && !v3.test() && (acc >> 12) < pw_) {
    return 0;
  }
  if ((waveform & 8) != 0) {
    result &= noise_bits();
  }
  if ((waveform & (waveform - 1)) != 0) {
    /* An isolated high bit loses its charge to low neighbours, at 12 bits
     * before truncating so lower bits can reach the readable byte */
    result &= (result << 1) | (result >> 1);
  }
  return static_cast<data_t>(result >> 4);
}

data_t SidVoice3::osc3(cycle_t now)
{
  clock_to(now);
  return osc3_output();
}

data_t SidVoice3::env3(cycle_t now)
{
  clock_to(now);
  return envelope_;
}

data_t SidVoice3::read(reg_t reg, cycle_t now)
{
  switch (reg) {
    case kSidRegOsc3:
    case kSidRegEnv3:
      break;
    default:
      return 0; /* not voice three */
  }

  switch (reg) {
    case kSidRegOsc3:
      return osc3(now);
    case kSidRegEnv3:
      return env3(now);
    default:
      break;
  }
  return 0;
}

} /* namespace usbsid */
