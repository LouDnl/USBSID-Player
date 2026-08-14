/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/audio_out.h
 * An audio device for the software SID path, via miniaudio.
 *
 * The threading is the whole reason this is a separate class rather than the
 * backend handing samples to a callback. miniaudio calls back on **its own
 * thread**, and `ResidFpSidBackend` is written for one thread: it renders while
 * the emulation runs, out of a vector it also compacts. Taking from that vector
 * on an audio thread is a data race, and the kind that produces a click once a
 * minute rather than a crash.
 *
 * So there is a ring in between. The main thread emulates, takes samples out of
 * the backend and pushes them here; the device callback pops. Single producer,
 * single consumer, two atomic indices, no locks on the audio thread. Which is
 * the same shape as the board path: the emulation stays ahead of the consumer
 * and the consumer never blocks it.
 *
 * Underrun is counted rather than hidden. A frontend that is not keeping up
 * should be able to say so, and silence that is quietly inserted is
 * indistinguishable from a tune that is quiet.
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

#pragma once
#ifndef _US_HOST_AUDIO_OUT_H_
#define _US_HOST_AUDIO_OUT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbsid {

/**
 * @brief A mono 16 bit output device, fed from the main thread.
 */
class AudioOut
{
  public:
    AudioOut(void) = default;
    ~AudioOut(void);

    AudioOut(const AudioOut &) = delete;
    AudioOut & operator=(const AudioOut &) = delete;

    /**
     * @brief Open the default output device.
     *
     * @param rate         sample rate to ask for. The device may not give it;
     *                     `rate()` afterwards is what it actually runs at, and
     *                     that is what the synthesis has to be configured for.
     * @param buffer_ms    how much to keep between the emulation and the device.
     *                     Bigger is more latency and fewer underruns.
     * @returns false if no device could be opened, with `error()` set
     */
    bool open(unsigned rate, unsigned buffer_ms = 120);

    /** @brief Stop and close. Safe to call twice. */
    void close(void);

    bool is_open(void) const { return open_; }

    /** @brief What the device actually runs at, which may not be what was asked. */
    unsigned rate(void) const { return rate_; }

    /** @brief Why open() failed, or an empty string. */
    const char * error(void) const { return error_; }

    /**
     * @brief Push samples for the device to play. Main thread only.
     *
     * @returns how many were accepted. Fewer than `n` means the ring is full,
     *          which is the signal to stop emulating for a moment rather than to
     *          drop audio: the caller should retry rather than discard.
     */
    size_t push(const int16_t * samples, size_t n);

    /** @brief Room for this many more samples right now. */
    size_t space(void) const;

    /** @brief Samples queued and not yet played. */
    size_t queued(void) const;

    /** @brief How many samples of silence the device had to invent. */
    uint64_t underruns(void) const { return underruns_.load(std::memory_order_relaxed); }

    /** @brief Wait until the queue has drained, so a tune ends where it ends. */
    void drain(void);

    /**
     * @brief Fill a device buffer. **Audio thread only.**
     *
     * Public because the device callback has to reach it and the callback cannot
     * be a member: miniaudio's type takes `ma_device *`, and casting a function
     * pointer to a different signature to make a member fit is the sort of thing
     * that works until it does not. So the callback is a free function in the
     * .cpp with the exact signature, and this is what it calls.
     *
     * Whatever is short is filled with silence and counted as an underrun.
     */
    void fill(int16_t * out, size_t frames);

  private:
    size_t pop(int16_t * out, size_t n);

    /* Opaque so miniaudio.h is included by exactly one translation unit: it is
     * four megabytes of header and does not belong in everything that plays a
     * tune. */
    void * device_ = nullptr;
    bool open_ = false;
    unsigned rate_ = 0;
    const char * error_ = "";

    std::vector<int16_t> ring_;
    /* Single producer, single consumer. head is written only by the main thread
     * and tail only by the audio thread, so neither needs a lock, and release
     * and acquire are what make the samples visible before the index that
     * publishes them. */
    std::atomic<size_t> head_{ 0 };
    std::atomic<size_t> tail_{ 0 };
    std::atomic<uint64_t> underruns_{ 0 };
};

} /* namespace usbsid */

#endif /* _US_HOST_AUDIO_OUT_H_ */
