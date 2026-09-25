/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback, for embedding on RP2350 (Pico2), and in a browser.
 *
 * src/fm/opl_chip.cpp
 * See opl_chip.h for what this is and why it exists.
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

#include "opl_chip.h"

#include <cmath>

namespace usbsid {

namespace {
/* Samples rendered per call into the scratch buffer. Nuked has no limit of its
 * own here, unlike the browser engine's 512; this is only to keep one frame's
 * worth of stereo pairs on the stack of the vector rather than growing it to
 * whatever the longest mix happens to be. */
constexpr size_t kBlock = 1024;

#if defined(US_FM_YMFM) && US_FM_YMFM
/* The clock an OPL2 sits on in a C64 cartridge is the NTSC colour burst
 * crystal, which gives the familiar 49716 Hz native output rate. */
constexpr uint32_t kOplClock = 3579545;
#endif

/* FM-YAM digi mode - see the class comment in opl_chip.h. */
constexpr uint8_t kFmTestReg   = 0x01; /**< arms/disarms digi mode */
constexpr uint8_t kDigiArmValue = 0x04; /**< the value that arms it */
constexpr uint8_t kFmDigiAddrA = 0xa0;  /**< channel A's PCM port (normally F-Num-lo) */
constexpr uint8_t kFmDigiAddrB = 0xa1;  /**< channel B's PCM port */
/* Centers the incoming byte on 128 (standard unsigned 8-bit PCM) and scales
 * it to sit alongside a normal SID/FM peak, rather than SIDKick-pico's own
 * reference implementation, which sums the raw unsigned byte straight in and
 * relies on a later stage to remove the resulting DC bias - this mix has no
 * such stage, so centering happens here instead. */
constexpr int32_t kDigiScale = 32;
} /* namespace */

void OplChip::chip_reset_(void)
{
#if defined(US_FM_YMFM) && US_FM_YMFM
  chip_.reset();
  native_step_ = static_cast<double>(chip_.sample_rate(kOplClock)) /
                 static_cast<double>(sample_rate_);
  native_pos_ = 0.0;
  prev_ = next_ = 0;
#else
  OPL3_Reset(&chip_, static_cast<uint32_t>(sample_rate_));
#endif
}

void OplChip::chip_write_(uint8_t reg, uint8_t value)
{
#if defined(US_FM_YMFM) && US_FM_YMFM
  chip_.write_address(reg);
  chip_.write_data(value);
#else
  OPL3_WriteRegBuffered(&chip_, reg, value);
#endif
}

#if defined(US_FM_YMFM) && US_FM_YMFM
int32_t OplChip::chip_sample_(void)
{
  /* Linear interpolation between the two native samples the output position
   * falls between, pulling native samples in as the position passes them. */
  native_pos_ += native_step_;
  while (native_pos_ >= 1.0) {
    native_pos_ -= 1.0;
    ymfm::ym3812::output_data out;
    chip_.generate(&out, 1);
    prev_ = next_;
    next_ = ymfm::clamp(out.data[0], -32768, 32767);
  }
  return prev_ + static_cast<int32_t>(
    std::lround(static_cast<double>(next_ - prev_) * native_pos_));
}
#endif

void OplChip::configure(unsigned sample_rate)
{
  if (sample_rate == 0) return;
  sample_rate_ = sample_rate;
  chip_reset_();
  address_ = 0;
  writes_ = 0;
  clipped_ = 0;
  digi_armed_ = false;
  digi_value_[0] = digi_value_[1] = 0;
  /* Stereo pairs: Nuked writes two shorts per sample and they are summed to
   * mono in mix_into(). */
  scratch_.assign(kBlock * 2, 0);
  ready_ = true;
}

void OplChip::bus_write(uint8_t reg, uint8_t value)
{
  if (!ready_) return;
  if (reg == kFmAddressReg) { address_ = value; return; }
  if (reg != kFmDataReg) return;

  if (address_ == kFmTestReg) digi_armed_ = (value == kDigiArmValue);

  if (digi_armed_ && (address_ == kFmDigiAddrA || address_ == kFmDigiAddrB)) {
    const int32_t centered = static_cast<int32_t>(value) - 128;
    digi_value_[address_ - kFmDigiAddrA] = static_cast<int16_t>(centered * kDigiScale);
    writes_++;
    return;
  }

  chip_write_(address_, value);
  writes_++;
}

void OplChip::mix_into(int16_t * out, size_t count)
{
  if (!ready_ || out == nullptr || count == 0) return;

  if (digi_armed_) {
    /* Held flat between real writes (sample-and-hold, same as the real DAC
     * between register writes - this tune's stream runs at roughly 2.5kHz,
     * well under the audio rate). Synthesis is skipped entirely while armed:
     * SIDKick-pico's own reference implementation discards the OPL's
     * synthesized output the same way once this mode is active. */
    const int32_t sample = static_cast<int32_t>(digi_value_[0]) +
                            static_cast<int32_t>(digi_value_[1]);
    const int32_t scaled = static_cast<int32_t>(std::lround(sample * gain_));
    for (size_t i = 0; i < count; i++) {
      int32_t v = static_cast<int32_t>(out[i]) + scaled;
      if (v > 32767) { v = 32767; clipped_++; }
      else if (v < -32768) { v = -32768; clipped_++; }
      out[i] = static_cast<int16_t>(v);
    }
    return;
  }

  size_t done = 0;
  while (done < count) {
    const size_t n = ((count - done) > kBlock) ? kBlock : (count - done);
#if defined(US_FM_YMFM) && US_FM_YMFM
    for (size_t i = 0; i < n; i++) {
      const int32_t scaled = static_cast<int32_t>(std::lround(chip_sample_() * gain_));
      int32_t v = static_cast<int32_t>(out[done + i]) + scaled;
      if (v > 32767) { v = 32767; clipped_++; }
      else if (v < -32768) { v = -32768; clipped_++; }
      out[done + i] = static_cast<int16_t>(v);
    }
#else
    OPL3_GenerateStream(&chip_, scratch_.data(), static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) {
      /* Mono by summing the pair and halving it, which is the same mixdown the
       * SID side does, and then gain_ on top. */
      const int32_t mono = (static_cast<int32_t>(scratch_[i * 2]) +
                            static_cast<int32_t>(scratch_[i * 2 + 1])) / 2;
      const int32_t scaled = static_cast<int32_t>(std::lround(mono * gain_));
      int32_t v = static_cast<int32_t>(out[done + i]) + scaled;
      if (v > 32767) { v = 32767; clipped_++; }
      else if (v < -32768) { v = -32768; clipped_++; }
      out[done + i] = static_cast<int16_t>(v);
    }
#endif
    done += n;
  }
}

void OplChip::reset(void)
{
  if (!ready_) return;
  /* A full re-reset rather than keying everything off by hand: it is the same
   * few hundred microseconds of table setup and it cannot leave a register
   * behind, which silencing by hand can. */
  chip_reset_();
  address_ = 0;
  writes_ = 0;
  clipped_ = 0;
  digi_armed_ = false;
  digi_value_[0] = digi_value_[1] = 0;
}

} /* namespace usbsid */
