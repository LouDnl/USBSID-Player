/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * songlengths.h
 * Song lengths from the HVSC Songlengths database
 *
 * Format of DOCUMENTS/Songlengths.md5:
 *
 *   [Database]
 *   ; /MUSICIANS/L/Laxity/Aint_Somebody.sid
 *   2894faade3c427129514756d378738dd=2:45.446
 *
 * Comment line with the tune name, then the key and one time per song separated
 * by spaces. Key is the MD5 of the whole .sid file as on disk (md5.h)
 *
 * The database is about 4 MB and 60 000 entries, too big to embed. Nothing is kept,
 * the caller supplies the text and it is scanned once per tune
 * (CLI reads the file, web hands over its text, embedded has no database)
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
#ifndef _US_FILE_SONGLENGTHS_H_
#define _US_FILE_SONGLENGTHS_H_

#include <cstddef>
#include <cstdint>

namespace usbsid {

/** @brief Most songs read from a database line */
constexpr unsigned kMaxSongLengths = 64;

/** @brief Song lengths of one database entry in milliseconds */
struct SongLengths {
  bool valid = false;
  unsigned count = 0;
  uint32_t ms[kMaxSongLengths] = { 0 };

  /** @brief Length of one song (counting from 1), 0 if unknown */
  uint32_t for_song(uint16_t song) const
  {
    if (!valid || song < 1 || song > count) return 0;
    return ms[song - 1];
  }
};

/**
 * @brief MD5 key of a .sid file (MD5 of the whole file)
 *
 * @param out At least 33 bytes
 */
void songlengths_key(const uint8_t * file_bytes, size_t len, char * out);

/**
 * @brief Look one key up in the database text
 *
 * @param db Whole database as text (read only, not kept)
 * @param db_len Length of db
 * @param key 32 lowercase hex characters
 */
SongLengths songlengths_lookup(const char * db, size_t db_len, const char * key);

/**
 * @brief Find the database on a host with a filesystem, return true if found and readable
 *
 * Search order (.md5 and .md are tried at each place):
 *   1. path from the command line
 *   2. $SONGLENGTHS
 *   3. ~/songlengths.md5
 *   4. $HVSCROOT/DOCUMENTS/Songlengths.md5
 *   5. $HVSC_BASE/DOCUMENTS/Songlengths.md5
 *   6. $HVSCDB, may name the file directly
 *
 * @param wanted Path from the command line or nullptr
 * @param out Buffer for the path that worked
 */
bool songlengths_find_file(const char * wanted, char * out, size_t out_len);

} /* namespace usbsid */

#endif /* _US_FILE_SONGLENGTHS_H_ */
