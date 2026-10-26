/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * wav_write.h
 * 16 bit PCM WAV writer for --output=wav
 *
 * Needs no device and no library, usable on a headless CI runner
 *
 * Header is written twice, zero lengths first and the real ones on close.
 * A file left by a crash only has wrong length fields
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
#ifndef _US_HOST_WAV_WRITE_H_
#define _US_HOST_WAV_WRITE_H_

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace usbsid {

/**
 * @brief Stream 16 bit PCM into a RIFF/WAVE file
 */
class WavWriter
{
  public:
    WavWriter(void) = default;
    ~WavWriter(void) { close(); }

    WavWriter(const WavWriter &) = delete;
    WavWriter & operator=(const WavWriter &) = delete;

    /**
     * @brief Open a file and write a placeholder header
     *
     * @param path      file path, "-" is not special
     * @param rate      sample rate in Hz
     * @param channels  1 or 2
     */
    bool open(const char * path, unsigned rate, unsigned channels = 1)
    {
      close();
      if (path == nullptr || rate == 0 || channels < 1 || channels > 2) return false;
      f_ = fopen(path, "wb");
      if (f_ == nullptr) return false;
      rate_ = rate;
      channels_ = channels;
      frames_ = 0;
      return write_header();
    }

    bool is_open(void) const { return f_ != nullptr; }

    /**
     * @brief Write n samples, stereo is interleaved (n counts samples)
     */
    bool write(const int16_t * samples, size_t n)
    {
      if (f_ == nullptr || samples == nullptr) return false;
      if (n == 0) return true;
      if (fwrite(samples, sizeof(int16_t), n, f_) != n) return false;
      frames_ += n;
      return true;
    }

    /** @brief Rewrite the two length fields and close, safe to call twice */
    bool close(void)
    {
      if (f_ == nullptr) return true;
      const uint32_t data_bytes = static_cast<uint32_t>(frames_ * sizeof(int16_t));
      bool ok = true;
      /* RIFF size at offset 4, data size at offset 40 */
      if (fseek(f_, 4, SEEK_SET) != 0) ok = false;
      else ok = put32(36u + data_bytes);
      if (ok && fseek(f_, 40, SEEK_SET) != 0) ok = false;
      else if (ok) ok = put32(data_bytes);
      if (fclose(f_) != 0) ok = false;
      f_ = nullptr;
      return ok;
    }

    /** @brief Samples written */
    uint64_t samples(void) const { return frames_; }
    /** @brief Seconds written */
    double seconds(void) const
    {
      if (rate_ == 0 || channels_ == 0) return 0.0;
      return static_cast<double>(frames_) /
             (static_cast<double>(rate_) * static_cast<double>(channels_));
    }

  private:
    bool put32(uint32_t v)
    {
      const uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff),
        static_cast<uint8_t>((v >> 16) & 0xff), static_cast<uint8_t>((v >> 24) & 0xff)
      };
      return fwrite(b, 1, 4, f_) == 4;
    }
    bool put16(uint16_t v)
    {
      const uint8_t b[2] = {
        static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff)
      };
      return fwrite(b, 1, 2, f_) == 2;
    }

    bool write_header(void)
    {
      const uint16_t bits = 16;
      const uint16_t ch = static_cast<uint16_t>(channels_);
      const uint32_t byte_rate = rate_ * channels_ * (bits / 8u);
      const uint16_t align = static_cast<uint16_t>(channels_ * (bits / 8u));

      bool ok = fwrite("RIFF", 1, 4, f_) == 4;
      ok = ok && put32(0);                 /* patched by close() */
      ok = ok && fwrite("WAVEfmt ", 1, 8, f_) == 8;
      ok = ok && put32(16);                /* fmt chunk size */
      ok = ok && put16(1);                 /* PCM */
      ok = ok && put16(ch);
      ok = ok && put32(rate_);
      ok = ok && put32(byte_rate);
      ok = ok && put16(align);
      ok = ok && put16(bits);
      ok = ok && fwrite("data", 1, 4, f_) == 4;
      ok = ok && put32(0);                 /* patched by close() */
      return ok;
    }

    FILE * f_ = nullptr;
    unsigned rate_ = 0;
    unsigned channels_ = 0;
    uint64_t frames_ = 0;
};

} /* namespace usbsid */

#endif /* _US_HOST_WAV_WRITE_H_ */
