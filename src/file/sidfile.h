/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sidfile.h
 * PSID and RSID file parsing, versions 1 to 4.
 *
 * Header layout per the HVSC SID file format documentation, and matching
 * player-repo/src/psid so both players read the same tunes the same way.
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

  char name[33] = { 0 };
  char author[33] = { 0 };
  char released[33] = { 0 };

  uint16_t flags = 0;
  uint8_t start_page = 0;   /* where the driver may be relocated to */
  uint8_t max_pages = 0;
  uint16_t reserved = 0;    /* holds the second and third SID addresses */

  const data_t * data = nullptr;
  size_t data_size = 0;

  /* worked out from the flags and the reserved word */
  VideoModel video_model = VideoModel::Pal6569;
  bool video_known = false;
  SidModel sid_model = SidModel::Unknown;
  addr_t sid_addr[4] = { 0xd400, 0, 0, 0 };
  uint8_t sid_count = 1;

  /** @brief True when this song is driven by a CIA timer rather than the raster */
  bool song_uses_cia(uint16_t song) const
  {
    if (song < 1) song = 1;
    const uint16_t bit = static_cast<uint16_t>((song > 32) ? 32 : song);
    return (speed & (1u << (bit - 1))) != 0;
  }
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
