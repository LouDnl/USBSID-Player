/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/audio_out.cpp
 * The one translation unit that compiles miniaudio. See audio_out.h for why the
 * ring exists.
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

#include "audio_out.h"

#include <chrono>
#include <cstring>
#include <thread>

/* miniaudio is vendored under lib/miniaudio and is public domain or MIT-0, so
 * it sits happily inside a GPLv2 project. Only the parts that are used: it
 * defaults to compiling every backend on the platform plus decoders for several
 * file formats, none of which this needs, and the build time shows it. */
#define MA_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#include "miniaudio.h"

namespace usbsid {

namespace {

/* The device callback, with miniaudio's exact signature so no cast is needed.
 * Audio thread: it may not allocate, lock, or touch anything the main thread
 * writes apart from the ring's tail index. */
void us_audio_callback(ma_device * dev, void * output, const void * input,
                       ma_uint32 frames)
{
  (void)input;
  if (dev == nullptr || output == nullptr) return;
  AudioOut * self = static_cast<AudioOut *>(dev->pUserData);
  if (self == nullptr) return;
  self->fill(static_cast<int16_t *>(output), frames);
}

} /* namespace */

AudioOut::~AudioOut(void)
{
  close();
}

void AudioOut::fill(int16_t * out, size_t frames)
{
  if (out == nullptr) return;
  const size_t got = pop(out, frames);
  if (got < frames) {
    /* Silence for the rest, and say so. Inventing silence is the only thing a
     * device callback can do when it is starved, but doing it quietly makes a
     * host that cannot keep up look like a quiet tune. */
    std::memset(out + got, 0, (frames - got) * sizeof(int16_t));
    underruns_.fetch_add(frames - got, std::memory_order_relaxed);
  }
}

bool AudioOut::open(unsigned rate, unsigned buffer_ms)
{
  close();
  if (rate == 0) { error_ = "sample rate of zero"; return false; }

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_s16;
  cfg.playback.channels = 1;
  cfg.sampleRate = rate;
  cfg.dataCallback = &us_audio_callback;
  cfg.pUserData = this;

  ma_device * dev = new ma_device();
  if (ma_device_init(nullptr, &cfg, dev) != MA_SUCCESS) {
    delete dev;
    error_ = "no audio device could be opened";
    return false;
  }

  /* What it actually gave us. Asking for 44100 on a device fixed at 48000 gets
   * a resampler for free whether or not that was wanted, so the synthesis is
   * configured for this and not for the request. */
  rate_ = dev->sampleRate;
  if (rate_ == 0) rate_ = rate;

  const size_t ms = (buffer_ms < 20u) ? 20u : buffer_ms;
  size_t frames = (static_cast<size_t>(rate_) * ms) / 1000u;
  if (frames < 1024) frames = 1024;
  ring_.assign(frames + 1, 0);   /* one spare, so full and empty differ */
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  underruns_.store(0, std::memory_order_relaxed);

  if (ma_device_start(dev) != MA_SUCCESS) {
    ma_device_uninit(dev);
    delete dev;
    error_ = "the audio device would not start";
    return false;
  }

  device_ = dev;
  open_ = true;
  error_ = "";
  return true;
}

void AudioOut::close(void)
{
  if (device_ != nullptr) {
    ma_device * dev = static_cast<ma_device *>(device_);
    ma_device_uninit(dev);
    delete dev;
    device_ = nullptr;
  }
  open_ = false;
  ring_.clear();
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
}

size_t AudioOut::space(void) const
{
  if (ring_.empty()) return 0;
  const size_t cap = ring_.size();
  const size_t h = head_.load(std::memory_order_relaxed);
  const size_t t = tail_.load(std::memory_order_acquire);
  return (t + cap - h - 1) % cap;
}

size_t AudioOut::queued(void) const
{
  if (ring_.empty()) return 0;
  const size_t cap = ring_.size();
  const size_t h = head_.load(std::memory_order_acquire);
  const size_t t = tail_.load(std::memory_order_relaxed);
  return (h + cap - t) % cap;
}

size_t AudioOut::push(const int16_t * samples, size_t n)
{
  if (samples == nullptr || ring_.empty()) return 0;
  const size_t cap = ring_.size();
  size_t h = head_.load(std::memory_order_relaxed);
  const size_t t = tail_.load(std::memory_order_acquire);
  const size_t room = (t + cap - h - 1) % cap;
  const size_t take = (n < room) ? n : room;

  for (size_t i = 0; i < take; i++) {
    ring_[h] = samples[i];
    h = (h + 1) % cap;
  }
  /* Release: the samples above must be visible before the index that says so. */
  head_.store(h, std::memory_order_release);
  return take;
}

size_t AudioOut::pop(int16_t * out, size_t n)
{
  if (out == nullptr || ring_.empty()) return 0;
  const size_t cap = ring_.size();
  const size_t h = head_.load(std::memory_order_acquire);
  size_t t = tail_.load(std::memory_order_relaxed);
  const size_t have = (h + cap - t) % cap;
  const size_t give = (n < have) ? n : have;

  for (size_t i = 0; i < give; i++) {
    out[i] = ring_[t];
    t = (t + 1) % cap;
  }
  tail_.store(t, std::memory_order_release);
  return give;
}

void AudioOut::drain(void)
{
  if (!open_) return;
  /* Wait for the device to play what is already queued, so a tune that has
   * finished is heard to its end instead of being cut off by close(). Bounded,
   * because a device that has stopped consuming would otherwise hang the exit. */
  for (int i = 0; i < 400 && queued() > 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

} /* namespace usbsid */
