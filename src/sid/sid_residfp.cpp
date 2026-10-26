/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_residfp.cpp
 * Bridges the shape differences described in sid_residfp.h
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

#include "sid_residfp.h"

#include <algorithm>
#include <cmath>

#include "residfp.h"

#include "machine.h"
#include "mos6581_8580.h"

namespace usbsid {

namespace {

/* Largest delta the seam can carry, scratch buffers are sized for it once
 * reSIDfp emits at most one sample per cycle, costs 128 KB of host memory */
constexpr size_t kMaxDelta = 0x10000;

/* Output rate bounds for set_speed(). Above ~215 kHz the intermediate rate of the
 * two pass sinc resampler drops below the output rate */
constexpr double kMinOutRate = 4000.0;
constexpr double kMaxOutRate = 192000.0;
constexpr double kMinSpeed = 0.1;
constexpr double kMaxSpeed = 8.0;

/* Registers at or above this are the kFmOplParkBase block (mos6581_8580.cpp)
 * $df40/$df50 with no SID claiming them, kMaxSids * 0x20 is exactly kFmOplParkBase */
constexpr addr_t kFmOplRange = static_cast<addr_t>(kMaxSids * 0x20);

/* Scope frames kept for a reader that falls behind, about a second at 48 kHz. Older ones are dropped */
constexpr size_t kScopeMaxFrames = 0x10000;

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
                                  SoftSidModel model, bool stereo)
{
  ready_ = false;
  if (chips < 1) chips = 1;
  if (chips > kMaxSoftSids) chips = kMaxSoftSids;
  if (clock_hz <= 0.0 || sample_rate == 0) return false;

  const reSIDfp::SamplingMethod method =
    (quality == SoftSidQuality::Good) ? reSIDfp::RESAMPLE : reSIDfp::DECIMATE;
  const reSIDfp::ChipModel chip_model =
    (model == SoftSidModel::Csg8580) ? reSIDfp::CSG8580 : reSIDfp::MOS6581;

  sample_rate_ = sample_rate;
  out_rate_ = speed_rate_();

  for (uint8_t i = 0; i < chips; i++) {
    if (sid_[i] == nullptr) sid_[i] = new reSIDfp::residfp();
    if (!sid_[i]->setChipModel(chip_model)) return false;
    /* Model first, the sampling parameters are derived from the clock */
    if (!sid_[i]->setSamplingParameters(clock_hz, method,
                                        static_cast<double>(out_rate_))) {
      return false;
    }
    sid_[i]->reset();
    apply_filter_(i, true, filter_);
  }
  /* Drop the chips above the new count */
  for (uint8_t i = chips; i < kMaxSoftSids; i++) {
    delete sid_[i];
    sid_[i] = nullptr;
  }

  chips_ = chips;
  clock_hz_ = clock_hz;
  quality_ = quality;
  stereo_ = stereo;
  /* Chips start Center, see set_pan() */
  for (uint8_t i = 0; i < kMaxSoftSids; i++) pan_[i] = SidPan::Center;
  recompute_pan_headroom();
  scratch_.assign(kMaxDelta, 0);
  mix_.assign(kMaxDelta, 0);
  mix_r_.assign(stereo_ ? kMaxDelta : 0, 0);
  fm_scratch_.assign(stereo_ ? kMaxDelta : 0, 0);
  out_.clear();
  taken_ = 0;
  scope_.clear();
  scope_taken_ = 0;
  produced_ = 0;
  clipped_ = 0;
  fm_writes_ = 0;
  ready_ = true;
  return true;
}

unsigned ResidFpSidBackend::speed_rate_(void) const
{
  double rate = static_cast<double>(sample_rate_) / speed_;
  if (rate < kMinOutRate) rate = kMinOutRate;
  if (rate > kMaxOutRate) rate = kMaxOutRate;
  return static_cast<unsigned>(std::lround(rate));
}

double ResidFpSidBackend::set_speed(double mult)
{
  if (!(mult > 0.0)) mult = 1.0;
  if (mult < kMinSpeed) mult = kMinSpeed;
  if (mult > kMaxSpeed) mult = kMaxSpeed;
  speed_ = mult;
  if (!ready_) return speed();

  const unsigned rate = speed_rate_();
  if (rate == out_rate_) return speed();

  /* Swaps the resampler only, voice/envelope/filter state carries on */
  const reSIDfp::SamplingMethod method =
    (quality_ == SoftSidQuality::Good) ? reSIDfp::RESAMPLE : reSIDfp::DECIMATE;
  for (uint8_t i = 0; i < chips_; i++) {
    if (sid_[i] == nullptr) continue;
    if (!sid_[i]->setSamplingParameters(clock_hz_, method,
                                        static_cast<double>(rate))) {
      /* Put back the chips already switched, keep the old rate. */
      for (uint8_t j = 0; j < i; j++) {
        if (sid_[j] != nullptr) {
          sid_[j]->setSamplingParameters(clock_hz_, method,
                                         static_cast<double>(out_rate_));
        }
      }
      return speed();
    }
  }
  out_rate_ = rate;
  fm_.set_rate(out_rate_);
  return speed();
}

void ResidFpSidBackend::set_filter(const SoftSidFilter & filter)
{
  const SoftSidFilter was = filter_;
  filter_ = filter;
  for (uint8_t i = 0; i < chips_; i++) {
    if (sid_[i] != nullptr) apply_filter_(i, false, was);
  }
}

void ResidFpSidBackend::apply_filter_(uint8_t chip, bool all, const SoftSidFilter & was)
{
  reSIDfp::residfp * sid = sid_[chip];
  const SoftSidFilter & f = filter_;
  if (all || f.enabled != was.enabled) sid->enableFilter(f.enabled);
  if (all || f.curve_6581 != was.curve_6581) sid->setFilter6581Curve(f.curve_6581);
  if (all || f.range_6581 != was.range_6581) sid->setFilter6581Range(f.range_6581);
  if (all || f.curve_8580 != was.curve_8580) sid->setFilter8580Curve(f.curve_8580);
  if (all || f.waveforms != was.waveforms) {
    sid->setCombinedWaveforms(
      (f.waveforms == SoftSidWaveforms::Weak)   ? reSIDfp::WEAK :
      (f.waveforms == SoftSidWaveforms::Strong) ? reSIDfp::STRONG : reSIDfp::AVERAGE);
  }
}

void ResidFpSidBackend::set_pan(uint8_t chip, SidPan pan)
{
  if (chip < 1 || chip > kMaxSoftSids) return;
  pan_[chip - 1] = pan;
  recompute_pan_headroom();
}

void ResidFpSidBackend::recompute_pan_headroom(void)
{
  uint8_t l = 0, r = 0;
  for (uint8_t i = 0; i < chips_; i++) {
    if (pan_[i] != SidPan::Right)  l++; /* Left or Center */
    if (pan_[i] != SidPan::Left)   r++; /* Right or Center */
  }
  pan_count_l_ = (l > 0) ? l : 1;
  pan_count_r_ = (r > 0) ? r : 1;
}

void ResidFpSidBackend::attach(Machine & machine)
{
  machine.set_sid_backend(*this);
  /* Software SID has no access cycle to deduct, see the header */
  machine.sid().config().access_overhead = 0;
  /* Software SID needs the idle time too, see SidConfig::render_idle */
  machine.sid().config().render_idle = true;

  /* Inherit the chips as the tune programmed them, attach can happen after playback started
   * Replay the write mirror $00 to $18 per chip, master volume ($18) and gates survive
   * Envelope positions cannot be inherited */
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

  /* Skipped stretch, account the time and clock nothing, see set_render() */
  if (!render_) return;

  uint32_t left = cycles;
  while (left > 0) {
    const uint32_t step = (left > kMaxDelta) ? static_cast<uint32_t>(kMaxDelta)
                                             : left;
    left -= step;

    /* Chip one sets the sample count of this step, the rest are summed onto it
     * Stereo: samples go to mix_ (left), mix_r_ (right) or both per pan_[], hard panning
     * Mono: summed into mix_ only */
    if (scope_on_) sid_[0]->setScopeTap(tap_.data());
    const int n = sid_[0]->clock(step, scratch_.data());
    size_t scope_base = 0;
    if (scope_on_ && n > 0) {
      scope_base = scope_.size();
      scope_.resize(scope_base + static_cast<size_t>(n) * scope_voices(), 0);
      store_scope_(0, n, scope_base);
    }
    if (stereo_) {
      const bool l0 = (pan_[0] != SidPan::Right);
      const bool r0 = (pan_[0] != SidPan::Left);
      for (int s = 0; s < n; s++) {
        mix_[static_cast<size_t>(s)]   = l0 ? scratch_[static_cast<size_t>(s)] : 0;
        mix_r_[static_cast<size_t>(s)] = r0 ? scratch_[static_cast<size_t>(s)] : 0;
      }
    } else {
      for (int s = 0; s < n; s++) mix_[static_cast<size_t>(s)] = scratch_[static_cast<size_t>(s)];
    }

    /* Clock every chip each step, most steps produce no samples */
    for (uint8_t c = 1; c < chips_; c++) {
      if (sid_[c] == nullptr) continue;
      if (scope_on_) sid_[c]->setScopeTap(tap_.data());
      const int m = sid_[c]->clock(step, scratch_.data());
      const int k = (m < n) ? m : n;
      if (scope_on_ && k > 0) store_scope_(c, k, scope_base);
      if (stereo_) {
        const bool lc = (pan_[c] != SidPan::Right);
        const bool rc = (pan_[c] != SidPan::Left);
        if (lc) for (int s = 0; s < k; s++) mix_[static_cast<size_t>(s)]   += scratch_[static_cast<size_t>(s)];
        if (rc) for (int s = 0; s < k; s++) mix_r_[static_cast<size_t>(s)] += scratch_[static_cast<size_t>(s)];
      } else {
        for (int s = 0; s < k; s++) mix_[static_cast<size_t>(s)] += scratch_[static_cast<size_t>(s)];
      }
    }

    if (n <= 0) continue;

    /* Divide by the per-channel headroom (chips_ mono, pan_count_l_/pan_count_r_ stereo)
     * See recompute_pan_headroom(). A clamp would distort on loud multi-chip passages
     * The clamp below is a guard with a counter */
    const size_t before = out_.size();
    if (stereo_) {
      for (int s = 0; s < n; s++) {
        int32_t l = mix_[static_cast<size_t>(s)];
        int32_t r = mix_r_[static_cast<size_t>(s)];
        if (pan_count_l_ > 1) l /= static_cast<int32_t>(pan_count_l_);
        if (pan_count_r_ > 1) r /= static_cast<int32_t>(pan_count_r_);
        /* sid_gain_ before the clamp, unity by default, see set_sid_gain() */
        if (sid_gain_ != 1.0f) {
          l = static_cast<int32_t>(std::lround(l * sid_gain_));
          r = static_cast<int32_t>(std::lround(r * sid_gain_));
        }
        if (l > 32767) { l = 32767; clipped_++; } else if (l < -32768) { l = -32768; clipped_++; }
        if (r > 32767) { r = 32767; clipped_++; } else if (r < -32768) { r = -32768; clipped_++; }
        out_.push_back(static_cast<int16_t>(l));
        out_.push_back(static_cast<int16_t>(r));
      }
      /* FM has no pan yet (always center), rendered once as it is stateful
       * Into fm_scratch_ (zeroed first), then added to both channels */
      if (fm_.ready()) {
        std::fill(fm_scratch_.begin(), fm_scratch_.begin() + n, 0);
        fm_.mix_into(fm_scratch_.data(), static_cast<size_t>(n));
        for (int s = 0; s < n; s++) {
          const size_t li = before + static_cast<size_t>(s) * 2;
          const int32_t add = fm_scratch_[static_cast<size_t>(s)];
          int32_t l = static_cast<int32_t>(out_[li]) + add;
          int32_t r = static_cast<int32_t>(out_[li + 1]) + add;
          if (l > 32767) { l = 32767; clipped_++; } else if (l < -32768) { l = -32768; clipped_++; }
          if (r > 32767) { r = 32767; clipped_++; } else if (r < -32768) { r = -32768; clipped_++; }
          out_[li]     = static_cast<int16_t>(l);
          out_[li + 1] = static_cast<int16_t>(r);
        }
      }
    } else {
      for (int s = 0; s < n; s++) {
        int32_t v = mix_[static_cast<size_t>(s)];
        if (chips_ > 1) v /= static_cast<int32_t>(chips_);
        if (sid_gain_ != 1.0f) v = static_cast<int32_t>(std::lround(v * sid_gain_));
        if (v > 32767) { v = 32767; clipped_++; }
        else if (v < -32768) { v = -32768; clipped_++; }
        out_.push_back(static_cast<int16_t>(v));
      }
      /* Sum FM onto the SID voices, built once a tune writes to it */
      if (fm_.ready()) fm_.mix_into(out_.data() + before, static_cast<size_t>(n));
    }
    produced_ += static_cast<uint64_t>(n);
  }

  /* Reclaim handed out space when worth the move
   * taken_ counts frames, out_ elements (channels() per frame) */
  const size_t taken_elems = taken_ * channels();
  if (taken_ > 0 && taken_elems >= out_.size() / 2 && taken_ > 4096) {
    out_.erase(out_.begin(), out_.begin() + static_cast<long>(taken_elems));
    taken_ = 0;
  }

  /* Same for the scope frames, after dropping unread ones */
  if (scope_on_) {
    const size_t v = scope_voices();
    const size_t waiting = scope_.size() / v - scope_taken_;
    if (waiting > kScopeMaxFrames) scope_taken_ += waiting - kScopeMaxFrames;
    if (scope_taken_ > 4096 && scope_taken_ * v >= scope_.size() / 2) {
      scope_.erase(scope_.begin(),
                   scope_.begin() + static_cast<long>(scope_taken_ * v));
      scope_taken_ = 0;
    }
  }
}

void ResidFpSidBackend::store_scope_(uint8_t chip, int count, size_t base)
{
  const size_t v = scope_voices();
  int16_t * dst = scope_.data() + base + 3u * chip;
  const int16_t * src = tap_.data();
  for (int s = 0; s < count; s++, dst += v, src += 3) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
  }
}

void ResidFpSidBackend::set_scope(bool on)
{
  if (on == scope_on_) return;
  scope_on_ = on;
  scope_.clear();
  scope_taken_ = 0;
  if (on) {
    tap_.assign(kMaxDelta * 3, 0);
    /* Silent scope frame per rendered frame not yet taken, keeps take_scope() in line with take() */
    if (ready_) scope_.assign(available() * scope_voices(), 0);
    return;
  }
  for (uint8_t i = 0; i < kMaxSoftSids; i++) {
    if (sid_[i] != nullptr) sid_[i]->setScopeTap(nullptr);
  }
  std::vector<int16_t>().swap(tap_);
  std::vector<int16_t>().swap(scope_);
}

size_t ResidFpSidBackend::take_scope(int16_t * out, size_t frames, unsigned voices)
{
  const unsigned all = scope_voices();
  if (out == nullptr || frames == 0 || all == 0 || !scope_on_) return 0;
  if (voices > all) voices = all;
  const size_t have = scope_.size() / all - scope_taken_;
  const size_t n = (frames < have) ? frames : have;
  const int16_t * src = scope_.data() + scope_taken_ * all;
  for (size_t f = 0; f < n; f++, src += all, out += voices) {
    std::copy(src, src + voices, out);
  }
  scope_taken_ += n;
  return n;
}

void ResidFpSidBackend::write(addr_t reg, data_t value, uint16_t cycles)
{
  if (!ready_) return;

  /* kFmOplRange and up is $df40/$df50 with no SID claiming them
   * The gap is honoured, the write goes to the OplChip and is counted */
  if (reg >= kFmOplRange) {
    advance(cycles);
    fm_writes_++;
    /* Local offset in the kFmOplParkBase block is 0 for $df40 (index port) or 0x10 for $df50 (data port)
     * See translate() in mos6581_8580.cpp, mapped to the kFmAddressReg/kFmDataReg pair for OplChip::bus_write() */
    if (!fm_.ready()) fm_.configure(out_rate_);
    const uint8_t opl_reg = ((reg & 0x1f) == 0x10) ? kFmDataReg : kFmAddressReg;
    fm_.bus_write(opl_reg, value);
    return;
  }

  /* Gap first, then the write */
  advance(cycles);

  const uint8_t chip = static_cast<uint8_t>(reg >> 5);
  if (chip >= chips_ || sid_[chip] == nullptr) return;
  sid_[chip]->write(static_cast<int>(reg & 0x1f), value);
}

data_t ResidFpSidBackend::read(addr_t reg, uint16_t cycles)
{
  if (!ready_) return 0xff;
  advance(cycles);

  if (reg >= kFmOplRange) return 0xff;
  const uint8_t chip = static_cast<uint8_t>(reg >> 5);
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
  /* The OPL belongs to the tune that programmed it */
  fm_.reset();
  cycles_clocked_ = 0;
}

size_t ResidFpSidBackend::take(int16_t * out, size_t frames)
{
  if (out == nullptr || frames == 0) return 0;
  const unsigned ch = channels();
  /* taken_ counts frames, out_ elements (interleaved L/R when stereo) */
  const size_t have = (out_.size() / ch) - taken_;
  const size_t n = (frames < have) ? frames : have;
  if (n != 0) {
    const size_t from = taken_ * ch;
    const size_t count = n * ch;
    std::copy(out_.begin() + static_cast<long>(from),
              out_.begin() + static_cast<long>(from + count), out);
    taken_ += n;
  }
  return n;
}

void ResidFpSidBackend::discard(void)
{
  out_.clear();
  taken_ = 0;
  scope_.clear();
  scope_taken_ = 0;
}

} /* namespace usbsid */
