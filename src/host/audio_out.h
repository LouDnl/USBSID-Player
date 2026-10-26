/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * audio_out.h
 * Audio device for the software SID path via miniaudio
 *
 * miniaudio calls back on a separate thread and ResidFpSidBackend is single
 * threaded, a ring sits between them. Main thread emulates and pushes, the
 * device callback pops. Single producer, single consumer, two atomic indices,
 * no locks on the audio thread, underruns are counted
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
#ifndef _US_HOST_AUDIO_OUT_H_
#define _US_HOST_AUDIO_OUT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbsid {

/**
 * @brief 16 bit output device fed from the main thread, mono by default
 */
class AudioOut
{
  public:
    AudioOut(void) = default;
    ~AudioOut(void);

    AudioOut(const AudioOut &) = delete;
    AudioOut & operator=(const AudioOut &) = delete;

    /**
     * @brief Open the default output device, returns false with error() set on failure
     *
     * @param rate         sample rate to ask for, the device may differ (see rate())
     * @param channels     1 or 2, must match ResidFpSidBackend::channels() (ring is a flat interleaved stream)
     * @param buffer_ms    buffer between emulation and device, bigger is more latency and fewer underruns
     */
    bool open(unsigned rate, unsigned channels = 1, unsigned buffer_ms = 120);

    /** @brief Channels the device was opened with */
    unsigned channels(void) const { return channels_; }

    /**
     * @brief Stop and close, safe to call twice
     *
     * Teardown runs on a detached thread, ma_device_uninit() can block forever (see source file)
     */
    void close(void);

    bool is_open(void) const { return open_; }

    /** @brief Sample rate the device runs at */
    unsigned rate(void) const { return rate_; }

    /** @brief Why open() failed, empty when fine */
    const char * error(void) const { return error_; }

    /**
     * @brief Push samples for the device to play, main thread only
     *
     * Samples are raw interleaved (n is 2 for one stereo frame), the output shape of
     * ResidFpSidBackend::take(). Returns the accepted count, fewer than n = ring full, retry later
     */
    size_t push(const int16_t * samples, size_t n);

    /** @brief Room for more raw samples */
    size_t space(void) const;

    /** @brief Raw samples queued and not yet played */
    size_t queued(void) const;

    /** @brief Samples of silence the device had to insert */
    uint64_t underruns(void) const { return underruns_.load(std::memory_order_relaxed); }

    /** @brief Wait until the queue has drained */
    void drain(void);

    /**
     * @brief True when the device has not called back for kStallMs or is closed, any thread
     *
     * A VM without audio hardware or a stuck backend leaves the ring full, pace on the wall clock then
     */
    bool stalled(void) const;

    /** @brief Callback silence after which the device is stalled */
    static constexpr int64_t kStallMs = 500;

    /**
     * @brief Fill a device buffer, audio thread only
     *
     * Public for the free function callback in the .cpp (miniaudio signature).
     * A shortage is filled with silence and counted as underrun
     */
    void fill(int16_t * out, size_t frames);

  private:
    size_t pop(int16_t * out, size_t n);

    /* Opaque, miniaudio.h (4MB) is included in one translation unit only */
    void * device_ = nullptr;
    bool open_ = false;
    unsigned rate_ = 0;
    unsigned channels_ = 1;
    const char * error_ = "";

    std::vector<int16_t> ring_;
    /* Single producer, single consumer. head is written by the main thread,
     * tail by the audio thread, release/acquire publishes the samples */
    std::atomic<size_t> head_{ 0 };
    std::atomic<size_t> tail_{ 0 };
    std::atomic<uint64_t> underruns_{ 0 };
    /* Steady clock in microseconds at the last callback or open() */
    std::atomic<int64_t> last_fill_us_{ 0 };
};

} /* namespace usbsid */

#endif /* _US_HOST_AUDIO_OUT_H_ */
