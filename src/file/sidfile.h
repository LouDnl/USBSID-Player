/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sidfile.h
 * PSID and RSID file parsing, versions 1 to 5
 * Header layout follows the HVSC SID file format documentation
 *
 * v5 adds variable length metadata strings (fixed 32 byte fallback), multi SID
 * config for up to 15 chips, panning hints, embedded song lengths and an FM/OPL flag.
 * Emulation and backends are capped at 4 chips, a v5 tune with more plays its
 * first 4 addresses (the rest is display only), panning is data only
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
#ifndef _US_FILE_SIDFILE_H_
#define _US_FILE_SIDFILE_H_

#include "types.h"
#include "vic_timing.h"

namespace usbsid {

enum class SidModel : uint8_t { Unknown = 0, Mos6581, Mos8580, Any };

/** @brief Most SID addresses a v5 header multiSidConfig can describe */
inline constexpr uint8_t kMaxSids = 15;

/** @brief v5 SID panning layout, flags bits 6-7 */
enum class SidPanLayout : uint8_t { Standard = 0, LCR = 1, CenterFirst = 2, FullyCentered = 3 };

/** @brief v5 SID panning mode, flags bits 8-9 */
enum class SidPanMode : uint8_t { Direct = 0, Reverse = 1, Group = 2, Spread = 3 };

/** @brief Where one chip output is hinted to go */
enum class SidPan : uint8_t { Left, Center, Right };

/**
 * @brief Fill out[0..n-1] with the v5 panning table positions
 *
 * @param layout v5 panning layout
 * @param mode v5 panning mode
 * @param n Chip count, 1 to kMaxSids (one chip is always Center)
 * @param out Receives n positions
 */
void compute_panning(SidPanLayout layout, SidPanMode mode, uint8_t n, SidPan * out);

/** @brief A parsed SID file, data points into the caller's bytes */
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

  /* v1 to v4 use at most 32 characters, v5 allows a variable length string up to almost the 96 byte metadata area */
  static constexpr size_t kMetaFieldSize = 96;
  char name[kMetaFieldSize] = { 0 };
  char author[kMetaFieldSize] = { 0 };
  char released[kMetaFieldSize] = { 0 };

  uint16_t flags = 0;

  /**
   * @brief RSID that is a C64 BASIC program, started by typing RUN
   *
   * Flags bit 1 in an RSID means BASIC (initAddress must be zero)
   */
  bool is_basic = false;

  /**
   * @brief Flags bit 0, payload is Compute!'s Sidplayer MUS data (v2-5 only)
   *
   * There is no MUS decoder, callers must refuse to play it
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

  /* v5 specific: flags bits 6-9 (SID panning), 10 (embedded song lengths) and 11 (FM OPL).
   * Panning is computed for every version, default is Standard/Direct */
  SidPanLayout pan_layout = SidPanLayout::Standard;
  SidPanMode pan_mode = SidPanMode::Direct;
  SidPan sid_pan[kMaxSids] = { SidPan::Center };

  bool has_fm_opl = false;
  bool has_embedded_song_lengths = false;

  /* Set when has_embedded_song_lengths and the trailer was found at the end of the file */
  const data_t * song_length_table = nullptr;
  uint16_t song_length_table_count = 0;

  /** @brief True if this song is driven by a CIA timer (else raster) */
  bool song_uses_cia(uint16_t song) const
  {
    if (song < 1) song = 1;
    uint16_t bit;
    if (song <= 32) {
      bit = song;
    } else if (version == 1 || (flags & 0x0002) != 0) {
      /* v1, or v2-5 with the PlaySID flag (bit 1) set, the 32 bit pattern repeats past tune 32 */
      bit = static_cast<uint16_t>(((song - 1) % 32) + 1);
    } else {
      /* v2-5 with the flag clear, tune 32 and up take bit 31 */
      bit = 32;
    }
    return (speed & (1u << (bit - 1))) != 0;
  }

  /**
   * @brief Song length in milliseconds from the embedded table, 0 if none
   *
   * @param song 1 based
   */
  uint32_t embedded_song_length_ms(uint16_t song) const;
};

/**
 * @brief Parse a SID file
 *
 * @param bytes Whole file
 * @param len Length of the file
 * @param out Filled in on success, out.valid tells
 */
bool sidfile_parse(const data_t * bytes, size_t len, SidFile & out);

/* Buffer size that holds any SidFile string converted to UTF-8 */
constexpr size_t kUtf8FieldSize = 3 * (SidFile::kMetaFieldSize - 1) + 1;

/**
 * @brief Convert a header string (name, author, released) from Windows-1252 to UTF-8 for display
 *
 * @param src NUL terminated Windows-1252 text
 * @param dst Destination, NUL terminated when cap > 0
 * @param cap Size of dst, kUtf8FieldSize fits any SidFile string
 */
const char * win1252_to_utf8(const char * src, char * dst, size_t cap);

} /* namespace usbsid */

#endif /* _US_FILE_SIDFILE_H_ */
