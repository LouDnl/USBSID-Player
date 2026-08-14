/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_residfp.cpp
 * See sid_residfp.h for the three shape differences this file exists to bridge.
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

#include "sid_residfp.h"

#include <algorithm>

#include "residfp.h"

#include "machine.h"
#include "mos6581_8580.h"

namespace usbsid {

namespace {

/* The largest delta the seam can carry, so the scratch buffer can be sized for
 * the worst case once and never grown on the write path. reSIDfp emits at most
 * one sample per clock cycle, and only when the sample rate equals the clock
 * rate, which never happens here; sizing for it anyway costs 128 KB of host
 * memory and removes a whole class of question. */
constexpr size_t kMaxDelta = 0x10000;

} /* namespace */

ResidFpSidBackend::ResidFpSidBackend(void) = default;

ResidFpSidBackend::~ResidFpSidBackend(void)
{
  for (uint8_t i = 0; i < kMaxSoftSids; i++) {
    delete sid_[i];
    sid_[i] = nullptr;
  }
}

bool ResidFpSidBackend::configure(uint8_t chips, double clock_hz,
                                  unsigned sample_rate, SoftSidQuality quality,
                                  SoftSidModel model)
{
  ready_ = false;
  if (chips < 1) chips = 1;
  if (chips > kMaxSoftSids) chips = kMaxSoftSids;
  if (clock_hz <= 0.0 || sample_rate == 0) return false;

  const reSIDfp::SamplingMethod method =
    (quality == SoftSidQuality::Good) ? reSIDfp::RESAMPLE : reSIDfp::DECIMATE;
  const reSIDfp::ChipModel chip_model =
    (model == SoftSidModel::Csg8580) ? reSIDfp::CSG8580 : reSIDfp::MOS6581;

  for (uint8_t i = 0; i < chips; i++) {
    if (sid_[i] == nullptr) sid_[i] = new reSIDfp::residfp();
    if (!sid_[i]->setChipModel(chip_model)) return false;
    /* Order matters: the sampling parameters are derived from the clock, so a
     * model change after them would leave the resampler configured for the
     * previous one. */
    if (!sid_[i]->setSamplingParameters(clock_hz, method,
                                        static_cast<double>(sample_rate))) {
      return false;
    }
    sid_[i]->reset();
  }
  /* Any chips above the new count are not kept around configured for an old
   * rate, because a later configure() with more chips would then reuse them. */
  for (uint8_t i = chips; i < kMaxSoftSids; i++) {
    delete sid_[i];
    sid_[i] = nullptr;
  }

  chips_ = chips;
  sample_rate_ = sample_rate;
  scratch_.assign(kMaxDelta, 0);
  mix_.assign(kMaxDelta, 0);
  out_.clear();
  taken_ = 0;
  produced_ = 0;
  clipped_ = 0;
  fm_writes_ = 0;
  ready_ = true;
  return true;
}

void ResidFpSidBackend::attach(Machine & machine)
{
  machine.set_sid_backend(*this);
  /* The one part of the contract that differs from a hardware backend. See the
   * header: a software SID has no bus and so no access cycle to deduct, and
   * leaving this at 1 puts every write a cycle early for the whole tune. */
  machine.sid().config().access_overhead = 0;
  /* And the other part: a software SID has to be told about the time in which
   * the tune did nothing, or a silent passage renders as no samples at all
   * rather than as silence. See SidConfig::render_idle. */
  machine.sid().config().render_idle = true;

  /* Inherit the chips as the tune has already programmed them.
   *
   * This backend is often attached **after** something has been playing: the
   * browser loads a tune, which for an RSID holding a BASIC program boots the
   * machine and RUNs it, and only then configures software audio. reSIDfp is
   * built fresh with every register at zero, so everything the tune set once and
   * never set again is lost, starting with the master volume at $18. A tune
   * whose play routine rewrites its registers every frame recovers within a
   * frame and never showed this; a program that sets the chip up once and then
   * only writes notes is silent for ever.
   *
   * That is exactly what `Beisikki_Demo_BASIC.sid` did: identical register
   * writes to the command line player, which sounds, and nothing audible here.
   *
   * The emulation keeps a mirror of every write, so the fix is to replay it.
   * Ascending order, `$00` to `$18` per chip, which is the order a driver writes
   * them in anyway: a control register carrying a gate that was already on stays
   * on, which is what "inherit" means. What cannot be inherited is where each
   * envelope had got to, and nothing can carry that across a chip that did not
   * exist a moment ago. */
  Mos6581_8580 & sid = machine.sid();
  for (uint8_t chip = 0; chip < chips_; chip++) {
    if (sid_[chip] == nullptr) continue;
    for (uint8_t reg = 0x00; reg <= 0x18; reg++) {
      const data_t value = sid.peek(static_cast<data_t>((chip << 5) | reg));
      sid_[chip]->write(reg, value);
    }
  }
}

void ResidFpSidBackend::advance(uint32_t cycles)
{
  if (!ready_ || cycles == 0) return;

  cycles_clocked_ += cycles;

  /* Running through a stretch whose audio is not wanted: the time is accounted
   * for above and nothing is clocked, which is the whole saving. See
   * set_render(). */
  if (!render_) return;

  uint32_t left = cycles;
  while (left > 0) {
    const uint32_t step = (left > kMaxDelta) ? static_cast<uint32_t>(kMaxDelta)
                                             : left;
    left -= step;

    /* Chip one sets how many samples this step produced, and the rest are
     * summed onto it. They are clocked identically and configured identically,
     * so they agree; taking the minimum rather than trusting that would hide a
     * configuration mistake instead of showing it. */
    /* **Every chip is clocked on every step, whatever came out of the first
     * one.** An earlier version skipped the rest when chip one produced no
     * samples, and most steps produce none: the sample rate is a fortieth of the
     * clock rate, so a short gap between writes yields nothing at all. The other
     * chips then only advanced on the steps where chip one happened to emit,
     * fell steadily behind, and a two or three SID tune played as one. Harmless
     * with a single chip, which is why it survived being tested. */
    const int n = sid_[0]->clock(step, scratch_.data());
    for (int s = 0; s < n; s++) mix_[static_cast<size_t>(s)] = scratch_[static_cast<size_t>(s)];

    for (uint8_t c = 1; c < chips_; c++) {
      if (sid_[c] == nullptr) continue;
      const int m = sid_[c]->clock(step, scratch_.data());
      const int k = (m < n) ? m : n;
      for (int s = 0; s < k; s++) {
        mix_[static_cast<size_t>(s)] += scratch_[static_cast<size_t>(s)];
      }
    }

    if (n <= 0) continue;

    /* Mixed down to mono by summing. Panning multi SID tunes is a real decision
     * and not an obvious default (the board does it in hardware, and the older
     * players pan), so it is deliberately not made here: one channel, and the
     * frontend can be given stereo later without this file changing shape.
     *
     * Divided by the number of chips, which is the headroom. Each reSIDfp
     * instance uses the whole of the sixteen bit range on its own, so two of
     * them summed reach twice full scale and three reach three times, and what
     * came out before was a clamp: `Industrial_Underwear_2SID.sid` pinned at
     * 32767 for 98 samples in thirty seconds and was heard as a ripple on the
     * loud parts. A clamp is distortion, and distortion that only appears on
     * multi SID tunes reads as "multi SID is broken".
     *
     * Deterministic attenuation rather than a limiter: a limiter is level
     * dependent, so the same tune would sound different depending on how loud
     * the moment before it was. The clamp stays as a guard with its counter,
     * and should now never fire. */
    const size_t before = out_.size();
    for (int s = 0; s < n; s++) {
      int32_t v = mix_[static_cast<size_t>(s)];
      if (chips_ > 1) v /= static_cast<int32_t>(chips_);
      if (v > 32767) { v = 32767; clipped_++; }
      else if (v < -32768) { v = -32768; clipped_++; }
      out_.push_back(static_cast<int16_t>(v));
    }
    /* The FM voices onto the SID voices, the way the two chips are summed on a
     * machine that has both. Nothing to do for the tunes that have no FM: the
     * chip is only built once one writes to it. */
    if (fm_.ready()) fm_.mix_into(out_.data() + before, static_cast<size_t>(n));
    produced_ += static_cast<uint64_t>(n);
  }

  /* Reclaim the space already handed out, once it is worth the move. Keeps the
   * vector from growing for the length of a tune without memmoving on every
   * take(). */
  if (taken_ > 0 && taken_ >= out_.size() / 2 && taken_ > 4096) {
    out_.erase(out_.begin(), out_.begin() + static_cast<long>(taken_));
    taken_ = 0;
  }
}

void ResidFpSidBackend::write(data_t reg, data_t value, uint16_t cycles)
{
  if (!ready_) return;

  /* $80 and $90 are $df40/$df50 with no SID claiming them. reSIDfp has no FM,
   * so the gap is still honoured (the tune's timeline does not care what the
   * write was for) and the write itself is counted and dropped. */
  if (reg >= 0x80) {
    advance(cycles);
    fm_writes_++;
    /* The gap first, above, so the write lands after the samples that came
     * before it, exactly as a SID write does. */
    if (!fm_.ready()) fm_.configure(sample_rate_);
    fm_.bus_write(reg, value);
    return;
  }

  /* The gap first, then the write: this is the ordering difference. */
  advance(cycles);

  const uint8_t chip = static_cast<uint8_t>((reg >> 5) & 0x03);
  if (chip >= chips_ || sid_[chip] == nullptr) return;
  sid_[chip]->write(static_cast<int>(reg & 0x1f), value);
}

data_t ResidFpSidBackend::read(data_t reg, uint16_t cycles)
{
  if (!ready_) return 0xff;
  advance(cycles);

  if (reg >= 0x80) return 0xff;
  const uint8_t chip = static_cast<uint8_t>((reg >> 5) & 0x03);
  if (chip >= chips_ || sid_[chip] == nullptr) return 0xff;
  return static_cast<data_t>(sid_[chip]->read(static_cast<int>(reg & 0x1f)));
}

void ResidFpSidBackend::wait(uint16_t cycles)
{
  advance(cycles);
}


void ResidFpSidBackend::reset(void)
{
  for (uint8_t i = 0; i < chips_; i++) {
    if (sid_[i] != nullptr) sid_[i]->reset();
  }
  discard();
  produced_ = 0;
  clipped_ = 0;
  fm_writes_ = 0;
  /* The OPL belongs to the tune that programmed it: carrying its registers into
   * the next one leaves that one playing the last one's instruments. */
  fm_.reset();
  cycles_clocked_ = 0;
}

size_t ResidFpSidBackend::take(int16_t * out, size_t frames)
{
  if (out == nullptr || frames == 0) return 0;
  const size_t have = out_.size() - taken_;
  const size_t n = (frames < have) ? frames : have;
  if (n != 0) {
    std::copy(out_.begin() + static_cast<long>(taken_),
              out_.begin() + static_cast<long>(taken_ + n), out);
    taken_ += n;
  }
  return n;
}

void ResidFpSidBackend::discard(void)
{
  out_.clear();
  taken_ = 0;
}

} /* namespace usbsid */
