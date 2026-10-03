/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session_audio.h
 * Session output to the default audio device: reSIDfp synthesis through
 * AudioOut. The device ring is the clock.
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

#pragma once
#ifndef _US_HOST_SESSION_AUDIO_H_
#define _US_HOST_SESSION_AUDIO_H_

#include <vector>

#include "audio_out.h"
#include "session.h"
#include "sid_residfp.h"

namespace usbsid {

/** @brief Software synthesis settings for AudioSessionSink. */
struct AudioSinkOptions {
  unsigned rate = 48000;                 /* asked of the device, it may differ */
  bool stereo = false;                   /* pan multi-SID tunes per the v5 hint */
  SoftSidQuality quality = SoftSidQuality::Good;
  int sid_volume = 100;                  /* percent, 0-300 */
  int fm_volume = 100;                   /* percent, 0-300 */
  unsigned buffer_ms = 120;              /* ring between emulation and device */
};

/** @brief reSIDfp into the default audio device. */
class AudioSessionSink final : public SessionSink
{
  public:
    explicit AudioSessionSink(const AudioSinkOptions & options = AudioSinkOptions{});
    ~AudioSessionSink(void) override;

    const char * name(void) const override { return "audio"; }
    SidBackend & backend(void) override { return soft_; }
    bool attach(Machine & machine) override;
    bool prepare(Machine & machine, const SidFile * tune) override;
    bool frame_done(const std::atomic<bool> & stopping) override;
    bool wall_clock(void) const override { return false; }
    void silence(Machine & machine) override;
    void restore(Machine & machine) override;
    void detach(Machine & machine) override;
    uint64_t underruns(void) const override;
    uint64_t clipped(void) const override { return soft_.clipped(); }

    /** @brief The device's error text after a failed attach(). */
    const char * error(void) const { return audio_.error(); }

  private:
    AudioSinkOptions options_;
    ResidFpSidBackend soft_;
    AudioOut audio_;
    std::vector<int16_t> buf_;
    bool held_ = false;            /* silenced, nothing pushed */
    uint64_t held_since_ = 0;      /* underruns when the hold started */
    uint64_t held_total_ = 0;      /* underruns counted while held */
};

} /* namespace usbsid */

#endif /* _US_HOST_SESSION_AUDIO_H_ */
