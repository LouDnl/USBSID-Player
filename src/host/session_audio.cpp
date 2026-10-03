/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session_audio.cpp
 * See session_audio.h.
 *
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

#include "session_audio.h"

#include <chrono>
#include <thread>

#include "mos6581_8580.h"

namespace usbsid {

namespace {

/** @brief Percent volume to a gain, negative clamped to 0. */
float gain(int percent)
{
  return static_cast<float>((percent < 0) ? 0 : percent) / 100.0f;
}

/**
 * @brief Replay the register file of every chip into a backend.
 *
 * @param machine  source of the register file
 * @param out      backend to write to
 */
void replay_registers(Machine & machine, SidBackend & out)
{
  Mos6581_8580 & sid = machine.sid();
  const uint8_t chips = (sid.config().count == 0) ? 1 : sid.config().count;
  for (uint8_t c = 0; c < chips; c++) {
    for (data_t r = 0; r <= 0x18; r++) {
      const addr_t logical = static_cast<addr_t>(c * 0x20 + r);
      const addr_t reg = sid.physical_reg(logical);
      if (reg == kSidNotMapped) continue;
      out.write(reg, sid.peek(logical), 4);
    }
  }
  out.flush();
}

} /* namespace */

AudioSessionSink::AudioSessionSink(const AudioSinkOptions & options)
  : options_(options)
{
  buf_.resize(65536);
}

AudioSessionSink::~AudioSessionSink(void)
{
  audio_.close();
}

bool AudioSessionSink::attach(Machine & machine)
{
  if (!audio_.is_open() &&
      !audio_.open(options_.rate, options_.stereo ? 2 : 1, options_.buffer_ms)) {
    return false;
  }
  machine.set_sid_backend(soft_);
  return true;
}

bool AudioSessionSink::prepare(Machine & machine, const SidFile * tune)
{
  /* The file's own model and chip count; a program says nothing, so two
   * chips, the same guess the command line player makes. */
  const SoftSidModel model = (tune != nullptr && tune->sid_model == SidModel::Mos8580)
                           ? SoftSidModel::Csg8580 : SoftSidModel::Mos6581;
  const uint8_t chips = static_cast<uint8_t>((tune != nullptr) ? tune->sid_count : 2);
  const uint32_t clock_hz = machine.vic().timing().clock_hz;

  if (!soft_.configure(chips, static_cast<double>(clock_hz), audio_.rate(),
                       options_.quality, model, options_.stereo)) {
    return false;
  }
  soft_.set_sid_gain(gain(options_.sid_volume));
  soft_.set_fm_gain(gain(options_.fm_volume));
  if (options_.stereo && tune != nullptr) {
    for (uint8_t c = 0; c < soft_.chips(); c++) {
      soft_.set_pan(static_cast<uint8_t>(c + 1), tune->sid_pan[c]);
    }
  }
  soft_.attach(machine);
  held_ = false;
  return true;
}

bool AudioSessionSink::frame_done(const std::atomic<bool> & stopping)
{
  const size_t ch = soft_.channels();
  size_t got;
  while ((got = soft_.take(buf_.data(), buf_.size() / ch)) != 0) {
    const size_t n = got * ch;
    size_t at = 0;
    while (at < n) {
      const size_t put = audio_.push(buf_.data() + at, n - at);
      at += put;
      if (put == 0) {
        /* Ring full: the device is the clock, wait for it to drain */
        if (stopping) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }
  return audio_.is_open();
}

void AudioSessionSink::silence(Machine & machine)
{
  (void)machine;
  if (held_) return;
  held_ = true;
  held_since_ = audio_.underruns();
  soft_.discard();
}

void AudioSessionSink::restore(Machine & machine)
{
  if (held_) {
    held_ = false;
    held_total_ += audio_.underruns() - held_since_;
  }
  replay_registers(machine, soft_);
}

void AudioSessionSink::detach(Machine & machine)
{
  silence(machine);
}

uint64_t AudioSessionSink::underruns(void) const
{
  const uint64_t now = audio_.underruns();
  const uint64_t held = held_ ? (now - held_since_) : 0;
  return now - held_total_ - held;
}

} /* namespace usbsid */
