/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * songlengths.cpp
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

#include "songlengths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util/md5.h"

namespace usbsid {

namespace {

/** @brief Convert "2:45.446", "0:56" or "1:17.5" to milliseconds */
uint32_t parse_time(const char * p, const char * end)
{
  uint32_t minutes = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    minutes = minutes * 10u + static_cast<uint32_t>(*p - '0');
    p++;
  }
  if (p >= end || *p != ':') return 0;
  p++;

  uint32_t seconds = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    seconds = seconds * 10u + static_cast<uint32_t>(*p - '0');
    p++;
  }

  /* Optional fraction of any length, read three digits (milliseconds) */
  uint32_t millis = 0;
  if (p < end && *p == '.') {
    p++;
    unsigned digits = 0;
    while (p < end && *p >= '0' && *p <= '9' && digits < 3) {
      millis = millis * 10u + static_cast<uint32_t>(*p - '0');
      p++; digits++;
    }
    while (digits < 3) { millis *= 10u; digits++; }
  }

  return (minutes * 60u + seconds) * 1000u + millis;
}

bool readable(const char * path)
{
  if (path == nullptr || path[0] == '\0') return false;
  FILE * f = fopen(path, "rb");
  if (f == nullptr) return false;
  fclose(f);
  return true;
}

/** @brief Join a directory, a subpath and a name if the directory is set */
bool join(char * out, size_t out_len, const char * dir, const char * tail)
{
  if (dir == nullptr || dir[0] == '\0') return false;
  const size_t dlen = strlen(dir);
  /* HVSC_BASE often has a trailing slash */
  const bool slash = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\'));
  const int n = snprintf(out, out_len, "%s%s%s", dir, slash ? "" : "/", tail);
  return n > 0 && static_cast<size_t>(n) < out_len;
}

} /* namespace */

void songlengths_key(const uint8_t * file_bytes, size_t len, char * out)
{
  md5_hex(file_bytes, len, out);
}

SongLengths songlengths_lookup(const char * db, size_t db_len, const char * key)
{
  SongLengths out;
  if (db == nullptr || db_len == 0 || key == nullptr) return out;
  const size_t klen = strlen(key);
  if (klen != 32) return out;

  /* Walk the lines. A key line starts with the hex and has an '=' after it, and
   * a comment line starts with ';'. Nothing else is of interest. */
  const char * p = db;
  const char * const end = db + db_len;
  while (p < end) {
    const char * nl = static_cast<const char *>(memchr(p, '\n', static_cast<size_t>(end - p)));
    const char * line_end = (nl != nullptr) ? nl : end;

    if (static_cast<size_t>(line_end - p) > klen && p[klen] == '=' &&
        memcmp(p, key, klen) == 0) {
      /* Found it, times follow separated by spaces */
      const char * t = p + klen + 1;
      while (t < line_end && out.count < kMaxSongLengths) {
        while (t < line_end && (*t == ' ' || *t == '\t')) t++;
        if (t >= line_end || *t == '\r') break;
        const char * te = t;
        while (te < line_end && *te != ' ' && *te != '\t' && *te != '\r') te++;
        out.ms[out.count++] = parse_time(t, te);
        t = te;
      }
      out.valid = (out.count > 0);
      return out;
    }

    if (nl == nullptr) break;
    p = nl + 1;
  }
  return out;
}

bool songlengths_find_file(const char * wanted, char * out, size_t out_len)
{
  /* Explicit path is used as given, no fallback if missing */
  if (wanted != nullptr && wanted[0] != '\0') {
    snprintf(out, out_len, "%s", wanted);
    return readable(out);
  }

  /* Named outright */
  const char * env_db = getenv("SONGLENGTHS");
  if (readable(env_db)) { snprintf(out, out_len, "%s", env_db); return true; }

  /* HVSC ships the file capitalised, .md is also seen */
  static const char * const names[] = {
    "Songlengths.md5", "songlengths.md5", "Songlengths.md", "songlengths.md"
  };
  /* Inside an HVSC tree the file is under DOCUMENTS, also try the directory itself */
  static const char * const prefixes[] = { "DOCUMENTS/", "" };

  const auto try_dir = [&](const char * dir) -> bool {
    if (dir == nullptr || dir[0] == '\0') return false;
    for (const char * prefix : prefixes) {
      for (const char * name : names) {
        char tail[128];
        snprintf(tail, sizeof(tail), "%s%s", prefix, name);
        if (join(out, out_len, dir, tail) && readable(out)) return true;
      }
    }
    return false;
  };

  /* HVSC tree */
  static const char * const roots[] = { "HVSCROOT", "HVSC_BASE", "HVSC", "HVSC_ROOT" };
  for (const char * var : roots) {
    if (try_dir(getenv(var))) return true;
  }

  /* User directory, HOME on POSIX, USERPROFILE on Windows (MSYS2 may set both) */
  const char * home = getenv("HOME");
  const char * profile = getenv("USERPROFILE");
  if (try_dir(home)) return true;
  if (try_dir(profile)) return true;

  /* Platform specific places, searched only */
  char dir[1024];

#if defined(_WIN32)
  /* %APPDATA%\HVSC, %LOCALAPPDATA%\HVSC and the same for the player name */
  static const char * const win_vars[] = { "APPDATA", "LOCALAPPDATA", "ProgramData" };
  static const char * const win_subs[] = { "HVSC", "USBSID-Player", "usbsid" };
  for (const char * var : win_vars) {
    const char * base = getenv(var);
    if (base == nullptr || base[0] == '\0') continue;
    for (const char * sub : win_subs) {
      if (join(dir, sizeof(dir), base, sub) && try_dir(dir)) return true;
    }
    if (try_dir(base)) return true;
  }
#elif defined(__APPLE__)
  /* ~/Library/Application Support and ~/Documents */
  static const char * const mac_subs[] = {
    "Library/Application Support/HVSC",
    "Library/Application Support/USBSID-Player",
    "Documents/HVSC",
    "Music/HVSC",
  };
  for (const char * sub : mac_subs) {
    if (join(dir, sizeof(dir), home, sub) && try_dir(dir)) return true;
  }
  if (readable("/Library/Application Support/HVSC/DOCUMENTS/Songlengths.md5")) {
    snprintf(out, out_len, "%s",
             "/Library/Application Support/HVSC/DOCUMENTS/Songlengths.md5");
    return true;
  }
#else
  /* XDG paths, then the home defaults, then system wide spots */
  const char * xdg_config = getenv("XDG_CONFIG_HOME");
  const char * xdg_data   = getenv("XDG_DATA_HOME");
  static const char * const nix_subs[] = { "hvsc", "HVSC", "usbsid", "usbsid-player" };
  for (const char * sub : nix_subs) {
    if (join(dir, sizeof(dir), xdg_config, sub) && try_dir(dir)) return true;
    if (join(dir, sizeof(dir), xdg_data, sub) && try_dir(dir)) return true;
  }
  static const char * const nix_home_subs[] = {
    ".config/hvsc", ".config/usbsid", ".local/share/hvsc", "hvsc", "HVSC",
    "Music/HVSC", "Documents/HVSC"
  };
  for (const char * sub : nix_home_subs) {
    if (join(dir, sizeof(dir), home, sub) && try_dir(dir)) return true;
  }
  static const char * const nix_system[] = {
    "/usr/share/hvsc", "/usr/local/share/hvsc", "/opt/hvsc", "/var/lib/hvsc"
  };
  for (const char * d : nix_system) {
    if (try_dir(d)) return true;
  }
#endif

  /* Last, names the file and not a place */
  const char * hvscdb = getenv("HVSCDB");
  if (readable(hvscdb)) { snprintf(out, out_len, "%s", hvscdb); return true; }

  out[0] = '\0';
  return false;
}

} /* namespace usbsid */
