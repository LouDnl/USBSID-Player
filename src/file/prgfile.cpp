/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * prgfile.cpp
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

#include "prgfile.h"

namespace usbsid {

namespace {

/* The PC64 container: "C64File" and a zero, sixteen bytes of name, a zero,
 * and the record length. The program, load address and all, follows. */
constexpr size_t kP00HeaderSize = 26;
constexpr size_t kP00NameOffset = 8;
constexpr size_t kP00NameLength = 16;

bool has_p00_signature(const data_t * bytes, size_t len)
{
  static const char kMagic[] = "C64File";
  if (len < kP00HeaderSize + 2) return false;
  for (size_t i = 0; i < 7; i++) {
    if (bytes[i] != static_cast<data_t>(kMagic[i])) return false;
  }
  return bytes[7] == 0x00;
}

/**
 * @brief Find the address a BASIC startup line SYSes to.
 *
 * The line looks like this in memory:
 *   link word, line number, $9e (the token for SYS), the digits, $00
 * and this is what typing RUN on a machine code program actually does. Only
 * the first line is looked at: a program that hides its SYS further in is not
 * something to start by guessing.
 */
bool find_sys_stub(const data_t * data, size_t size, addr_t & sys_out)
{
  const size_t limit = (size < 40) ? size : 40;

  for (size_t i = 0; i < limit; i++) {
    if (data[i] != 0x9e) continue;     /* SYS */

    size_t j = i + 1;
    while (j < size && data[j] == ' ') j++;

    uint32_t value = 0;
    bool any = false;
    while (j < size && data[j] >= '0' && data[j] <= '9') {
      value = value * 10 + static_cast<uint32_t>(data[j] - '0');
      any = true;
      j++;
    }
    if (any && value <= 0xffff) {
      sys_out = static_cast<addr_t>(value);
      return true;
    }
  }
  return false;
}

} /* namespace */

bool prgfile_parse(const data_t * bytes, size_t len, PrgFile & out)
{
  out = PrgFile();

  if (bytes == nullptr || len < 3) return false;

  const data_t * p = bytes;
  size_t n = len;

  if (has_p00_signature(bytes, len)) {
    out.is_p00 = true;
    for (size_t i = 0; i < kP00NameLength; i++) {
      const data_t c = bytes[kP00NameOffset + i];
      /* the name is PETSCII padded with $a0, and only wanted for display */
      out.name[i] = (c == 0x00 || c == 0xa0) ? '\0' : static_cast<char>(c);
    }
    out.name[kP00NameLength] = '\0';
    p += kP00HeaderSize;
    n -= kP00HeaderSize;
    if (n < 3) return false;
  }

  out.load_addr = static_cast<addr_t>(p[0] | (p[1] << 8));
  out.data = p + 2;
  out.data_size = n - 2;

  /* A program that would run off the top of memory is not one */
  if (out.load_addr + out.data_size > 0x10000) return false;

  out.end_addr = static_cast<addr_t>(out.load_addr + out.data_size - 1);
  out.has_sys_stub = find_sys_stub(out.data, out.data_size, out.sys_addr);

  out.valid = true;
  return true;
}

} /* namespace usbsid */
