/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * songlengths.h
 * How long a tune is, out of HVSC's Songlengths database.
 *
 * The format, from `DOCUMENTS/Songlengths.md5`:
 *
 *   [Database]
 *   ; /MUSICIANS/L/Laxity/Aint_Somebody.sid
 *   2894faade3c427129514756d378738dd=2:45.446
 *
 * A comment line naming the tune, then the key and one time per song separated
 * by spaces. The key is the MD5 of the **whole .sid file** as it sits on disk,
 * which was checked against the real database rather than assumed; see
 * `util/md5.h` for why that is worth saying.
 *
 * The database is about four megabytes and 60 000 entries, which is far too much
 * to embed and more than the RP2350 has to spare. So nothing here holds it: the
 * caller says where the text is, this scans it once, and nothing is kept. That
 * suits every frontend for a different reason:
 *
 *   CLI       the file is on disk and read once per tune
 *   web       the page hands over whatever text it has, from wherever it got it
 *   embedded  there is no database, and asking costs nothing but a null
 *
 * Scanning four megabytes to find one line takes under a millisecond and happens
 * once per tune, so there is no index and no map to build. A player that loaded
 * thousands of tunes would want one; this one does not.
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
#ifndef _US_FILE_SONGLENGTHS_H_
#define _US_FILE_SONGLENGTHS_H_

#include <cstddef>
#include <cstdint>

namespace usbsid {

/** @brief The most songs a database line is read for. */
constexpr unsigned kMaxSongLengths = 64;

/**
 * @brief The lengths one database entry lists, in milliseconds.
 */
struct SongLengths {
  bool valid = false;
  unsigned count = 0;
  uint32_t ms[kMaxSongLengths] = { 0 };

  /** @brief The length of one song, counting from 1, or 0 if unknown. */
  uint32_t for_song(uint16_t song) const
  {
    if (!valid || song < 1 || song > count) return 0;
    return ms[song - 1];
  }
};

/**
 * @brief The MD5 key for a .sid file: the MD5 of the whole file.
 *
 * @param out  at least 33 bytes
 */
void songlengths_key(const uint8_t * file_bytes, size_t len, char * out);

/**
 * @brief Look one key up in the database text.
 *
 * @param db      the whole database, as text. Not modified, not kept.
 * @param db_len  its length
 * @param key     32 lowercase hex characters
 */
SongLengths songlengths_lookup(const char * db, size_t db_len, const char * key);

/**
 * @brief Where to look for the database, in order, for a host with a filesystem.
 *
 * The order is the caller's own first, then the conventional places:
 *
 *   1. whatever was asked for on the command line
 *   2. `$SONGLENGTHS`
 *   3. `~/songlengths.md5` and `~/songlengths.md`
 *   4. `$HVSCROOT/DOCUMENTS/Songlengths.md5` and `.md`
 *   5. `$HVSC_BASE/DOCUMENTS/Songlengths.md5` and `.md`
 *   6. `$HVSCDB`, which may name the file directly
 *
 * Both `.md5` and `.md` are tried at each place, since both spellings are in
 * use and the difference is not worth a failed lookup.
 *
 * @param wanted  the command line's answer, or nullptr
 * @param out     buffer for the path that worked
 * @returns true when one of them exists and is readable
 */
bool songlengths_find_file(const char * wanted, char * out, size_t out_len);

} /* namespace usbsid */

#endif /* _US_FILE_SONGLENGTHS_H_ */
