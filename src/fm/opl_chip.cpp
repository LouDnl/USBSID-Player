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

namespace usbsid {

namespace {
/* Samples rendered per call into the scratch buffer. Nuked has no limit of its
 * own here, unlike the browser engine's 512; this is only to keep one frame's
 * worth of stereo pairs on the stack of the vector rather than growing it to
 * whatever the longest mix happens to be. */
constexpr size_t kBlock = 1024;
} /* namespace */

void OplChip::configure(unsigned sample_rate)
{
  if (sample_rate == 0) return;
  OPL3_Reset(&chip_, static_cast<Bit32u>(sample_rate));
  sample_rate_ = sample_rate;
  address_ = 0;
  writes_ = 0;
  clipped_ = 0;
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
  OPL3_WriteRegBuffered(&chip_, address_, value);
  writes_++;
}

void OplChip::mix_into(int16_t * out, size_t count)
{
  if (!ready_ || out == nullptr || count == 0) return;

  size_t done = 0;
  while (done < count) {
    const size_t n = ((count - done) > kBlock) ? kBlock : (count - done);
    OPL3_GenerateStream(&chip_, scratch_.data(), static_cast<Bit32u>(n));
    for (size_t i = 0; i < n; i++) {
      /* Mono by summing the pair and halving it, which is the same mixdown the
       * SID side does, and then the FM attenuation on top. */
      const int32_t mono = (static_cast<int32_t>(scratch_[i * 2]) +
                            static_cast<int32_t>(scratch_[i * 2 + 1])) / 2;
      int32_t v = static_cast<int32_t>(out[done + i]) + (mono >> kFmAttenuation);
      if (v > 32767) { v = 32767; clipped_++; }
      else if (v < -32768) { v = -32768; clipped_++; }
      out[done + i] = static_cast<int16_t>(v);
    }
    done += n;
  }
}

void OplChip::reset(void)
{
  if (!ready_) return;
  /* A full re-reset rather than keying everything off by hand: it is the same
   * few hundred microseconds of table setup and it cannot leave a register
   * behind, which silencing by hand can. */
  OPL3_Reset(&chip_, static_cast<Bit32u>(sample_rate_));
  address_ = 0;
  writes_ = 0;
  clipped_ = 0;
}

} /* namespace usbsid */
