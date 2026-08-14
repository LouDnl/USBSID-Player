/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * util/md5.h
 * MD5, RFC 1321, for one purpose: the key into HVSC's Songlengths database.
 *
 * That key is the MD5 of the **whole .sid file**, bytes as they are on disk,
 * which was verified against HVSC's own `DOCUMENTS/Songlengths.md5` rather than
 * assumed: eight entries picked at random out of 60572 all matched `md5sum` of
 * the file exactly.
 *
 * Worth writing down because the obvious guess is wrong. Older players key this
 * on the **PSID MD5**, a hash over selected header fields and the C64 data
 * rather than the file, and libsidplayfp carries two variants of it. Any of
 * those would miss every entry in this database, silently, since a miss and a
 * tune with no known length look the same.
 *
 * MD5 is not used here for anything to do with security, and would be the wrong
 * choice if it were.
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
#ifndef _US_UTIL_MD5_H_
#define _US_UTIL_MD5_H_

#include <cstddef>
#include <cstdint>

namespace usbsid {

/**
 * @brief MD5 of a block of bytes, as 32 lowercase hex characters.
 *
 * @param data  the bytes
 * @param len   how many
 * @param out   at least 33 bytes; written with the hex and a terminator
 */
inline void md5_hex(const uint8_t * data, size_t len, char * out)
{
  static const uint32_t K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
  };
  static const uint8_t S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
  };

  uint32_t h[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };

  const auto rotl = [](uint32_t v, uint8_t c) -> uint32_t {
    return static_cast<uint32_t>((v << c) | (v >> (32 - c)));
  };

  /* One 64 byte block, with the message words read little endian. */
  const auto block = [&](const uint8_t * p) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
      m[i] = static_cast<uint32_t>(p[i * 4])
           | (static_cast<uint32_t>(p[i * 4 + 1]) << 8)
           | (static_cast<uint32_t>(p[i * 4 + 2]) << 16)
           | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f = 0;
      int g = 0;
      if (i < 16)      { f = (b & c) | (~b & d);            g = i; }
      else if (i < 32) { f = (d & b) | (~d & c);            g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d;                     g = (3 * i + 5) & 15; }
      else             { f = c ^ (b | static_cast<uint32_t>(~d)); g = (7 * i) & 15; }
      const uint32_t tmp = d;
      d = c;
      c = b;
      b = static_cast<uint32_t>(b + rotl(a + f + K[i] + m[g], S[i]));
      a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  };

  size_t at = 0;
  for (; at + 64 <= len; at += 64) block(data + at);

  /* The tail, the 0x80 terminator, and the bit count. Two blocks when the
   * remainder leaves no room for the eight length bytes. */
  uint8_t tail[128];
  const size_t rem = len - at;
  for (size_t i = 0; i < rem; i++) tail[i] = data[at + i];
  tail[rem] = 0x80;
  const size_t total = (rem + 1 + 8 <= 64) ? 64 : 128;
  for (size_t i = rem + 1; i < total - 8; i++) tail[i] = 0;
  const uint64_t bits = static_cast<uint64_t>(len) * 8u;
  for (int i = 0; i < 8; i++) {
    tail[total - 8 + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
  }
  block(tail);
  if (total == 128) block(tail + 64);

  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    for (int b = 0; b < 4; b++) {
      const uint8_t v = static_cast<uint8_t>((h[i] >> (8 * b)) & 0xff);
      out[i * 8 + b * 2]     = hex[v >> 4];
      out[i * 8 + b * 2 + 1] = hex[v & 0x0f];
    }
  }
  out[32] = '\0';
}

} /* namespace usbsid */

#endif /* _US_UTIL_MD5_H_ */
