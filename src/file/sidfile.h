/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sidfile.h
 * PSID and RSID file parsing, versions 1 to 5.
 *
 * Header layout per the HVSC SID file format documentation, and matching
 * old player ~ src/psid so both players read the same tunes the same way.
 *
 * Version 5 adds: variable length metadata strings (with a fallback to the
 * fixed 32 byte layout when the variable one does not parse), a multi-SID
 * address configuration that can describe up to 15 chips, a stereo panning
 * hint per chip, an embedded song length table, and an FM/OPL flag. This
 * player's emulation and every backend (mos6581_8580.h's SidConfig, the
 * network SID device client) are hard capped at 4 chips, so a v5 tune asking
 * for more plays its first 4 addresses, in the order the file's own address
 * configuration generates them, and the rest are exposed here for display
 * only. Panning is likewise exposed as data rather than rendered: the audio
 * path is deliberately one channel (see sid_residfp.cpp), and the spec
 * itself allows a player to ignore the hint and fall back to its native
 * output.
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
#ifndef _US_FILE_SIDFILE_H_
#define _US_FILE_SIDFILE_H_

#include "types.h"
#include "vic_timing.h"

namespace usbsid {

enum class SidModel : uint8_t { Unknown = 0, Mos6581, Mos8580, Any };

/** @brief The most SID addresses a v5 header's multiSidConfig can describe. */
inline constexpr uint8_t kMaxSids = 15;

/** @brief v5 SID Panning Layout, flags bits 6-7. */
enum class SidPanLayout : uint8_t { Standard = 0, LCR = 1, CenterFirst = 2, FullyCentered = 3 };

/** @brief v5 SID Panning Mode, flags bits 8-9. */
enum class SidPanMode : uint8_t { Direct = 0, Reverse = 1, Group = 2, Spread = 3 };

/** @brief Where one chip's output is hinted to go. */
enum class SidPan : uint8_t { Left, Center, Right };

/**
 * @brief A parsed SID file.
 *
 * The payload is not copied: the caller owns the bytes and this points into
 * them, which keeps the embedded build free of a second 64 KB buffer.
 */
struct SidFile {
  bool valid = false;
  bool is_rsid = false;
  uint16_t version = 0;
  uint16_t data_offset = 0;

  addr_t load_addr = 0;
  addr_t init_addr = 0;
  addr_t play_addr = 0;
  addr_t load_last_addr = 0;

  uint16_t songs = 1;
  uint16_t start_song = 1;
  uint32_t speed = 0;       /* one bit per song: 0 = raster, 1 = CIA */

  /* Versions 1 to 4 fill at most 32 characters here. Version 5 allows a
   * variable length string that, in the extreme, uses almost the whole 96
   * byte metadata area for one field, so the buffer is sized for that
   * worst case rather than the historical 32+1. */
  static constexpr size_t kMetaFieldSize = 96;
  char name[kMetaFieldSize] = { 0 };
  char author[kMetaFieldSize] = { 0 };
  char released[kMetaFieldSize] = { 0 };

  uint16_t flags = 0;

  /**
   * @brief An RSID that is a C64 BASIC program, started by typing RUN.
   *
   * Flags bit 1 means two different things depending on the file. In a PSID it
   * marks a tune that uses PlaySID's own extensions; in an **RSID** it says the
   * tune is BASIC, and then `initAddress` must be zero and the machine is meant
   * to run it the way a person would: boot, and RUN.
   *
   * There is no init routine to call and no driver to install. A player that
   * treats it as an ordinary tune calls the load address as if it were code,
   * which for a BASIC program is a jump into a tokenised line.
   */
  bool is_basic = false;

  /**
   * @brief Flags bit 0: the payload is Compute!'s Sidplayer MUS data, not a
   * built-in music player (v2-5 only; always false for v1, which has no
   * flags field).
   *
   * This player has no MUS decoder. A tune with this set has no init/play
   * routine of its own - `init_addr` still defaults to `load_addr` below,
   * which for MUS data is not code, so nothing here should call it. Callers
   * should check this and refuse to play rather than run the MUS data as
   * 6502.
   */
  bool is_mus_player = false;

  uint8_t start_page = 0;   /* where the driver may be relocated to */
  uint8_t max_pages = 0;
  uint16_t reserved = 0;    /* holds the second and third SID addresses, v3/v4 only */

  const data_t * data = nullptr;
  size_t data_size = 0;

  /* worked out from the flags and the reserved word */
  VideoModel video_model = VideoModel::Pal6569;
  bool video_known = false;
  SidModel sid_model = SidModel::Unknown;
  addr_t sid_addr[kMaxSids] = { 0xd400 };
  uint8_t sid_count = 1;

  /* v5 specific: flags bits 6-9 (SID panning), 10 (embedded song lengths) and
   * 11 (FM OPL). Panning is computed for every version, not just v5: a tune
   * with no opinion is Standard/Direct, which for one chip is Center and for
   * several alternates L/R, and that default is worth having uniformly
   * rather than leaving callers to special case "no panning data". */
  SidPanLayout pan_layout = SidPanLayout::Standard;
  SidPanMode pan_mode = SidPanMode::Direct;
  SidPan sid_pan[kMaxSids] = { SidPan::Center };

  bool has_fm_opl = false;
  bool has_embedded_song_lengths = false;

  /* Set only when has_embedded_song_lengths and the trailer was actually
   * found at the end of the file. Points into the caller's bytes, same as
   * `data` above: nothing here is copied. */
  const data_t * song_length_table = nullptr;
  uint16_t song_length_table_count = 0;

  /** @brief True when this song is driven by a CIA timer rather than the raster */
  bool song_uses_cia(uint16_t song) const
  {
    if (song < 1) song = 1;
    uint16_t bit;
    if (song <= 32) {
      bit = song;
    } else if (version == 1 || (flags & 0x0002) != 0) {
      /* v1, or v2-5 with the PlaySID-specific flag (bit 1) set: the 32 bit
       * pattern repeats past tune 32 (spec: "tune 33 uses bit 0, tune 34
       * uses bit 1, and so on"). */
      bit = static_cast<uint16_t>(((song - 1) % 32) + 1);
    } else {
      /* v2-5 with the flag clear: tune 32 and everything past it takes bit 31. */
      bit = 32;
    }
    return (speed & (1u << (bit - 1))) != 0;
  }

  /**
   * @brief One song's length from the file's own embedded table, in
   * milliseconds, or 0 when there is none (has_embedded_song_lengths is
   * false, the trailer did not fit, or `song` is out of range).
   *
   * @param song  1 based, as everywhere else in this struct
   */
  uint32_t embedded_song_length_ms(uint16_t song) const;
};

/**
 * @brief Parse a SID file.
 *
 * @param bytes  the whole file
 * @param len    its length
 * @param out    filled in on success, out.valid tells you which
 */
bool sidfile_parse(const data_t * bytes, size_t len, SidFile & out);

} /* namespace usbsid */

#endif /* _US_FILE_SIDFILE_H_ */
