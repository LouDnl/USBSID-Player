/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sidfile.cpp
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

#include <cstring>

#include "sidfile.h"

namespace usbsid {

namespace {

US_ALWAYS_INLINE uint16_t be16(const data_t * p)
{
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
US_ALWAYS_INLINE uint32_t be32(const data_t * p)
{
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void copy_text(char * dst, const data_t * src, size_t n)
{
  memcpy(dst, src, n);
  dst[n] = 0;
}

/* A second or third SID address is a byte holding the middle nybbles of
 * $dxx0. Only some of them are legal. */
addr_t decode_sid_addr(uint8_t byte)
{
  if (byte == 0) return 0;
  const addr_t addr = static_cast<addr_t>(0xd000 | (byte << 4));
  if ((addr >= 0xd420 && addr < 0xd800) || addr >= 0xde00) {
    if ((addr & 0x10) == 0) return addr;
  }
  return 0;
}

} /* namespace */

bool sidfile_parse(const data_t * bytes, size_t len, SidFile & out)
{
  out = SidFile{};
  if (bytes == nullptr || len < 0x76) return false;

  const bool psid = memcmp(bytes, "PSID", 4) == 0;
  const bool rsid = memcmp(bytes, "RSID", 4) == 0;
  if (!psid && !rsid) return false;

  out.is_rsid = rsid;
  out.version = be16(bytes + 0x04);
  /* Versions 1 to 4 are the documented ones. The four SID community also
   * ships an extended header, 130 bytes long, that carries a fourth chip
   * address; old player lists those offsets as SIDFILEPLUS. It is accepted
   * here rather than rejected, because the tunes exist and play. */
  const bool plus = (out.version > 4) && (len >= 0x82) && (be16(bytes + 0x06) >= 0x82);
  if (!plus && (out.version < 1 || out.version > 4)) return false;

  out.data_offset = be16(bytes + 0x06);
  out.load_addr   = be16(bytes + 0x08);
  out.init_addr   = be16(bytes + 0x0a);
  out.play_addr   = be16(bytes + 0x0c);
  out.songs       = be16(bytes + 0x0e);
  out.start_song  = be16(bytes + 0x10);
  out.speed       = be32(bytes + 0x12);

  copy_text(out.name,     bytes + 0x16, 32);
  copy_text(out.author,   bytes + 0x36, 32);
  copy_text(out.released, bytes + 0x56, 32);

  if (out.version >= 2 || plus) {
    if (len < 0x7c) return false;
    out.flags      = be16(bytes + 0x76);
    out.start_page = bytes[0x78];
    out.max_pages  = bytes[0x79];
    out.reserved   = be16(bytes + 0x7a);
  }

  if (out.data_offset == 0 || out.data_offset >= len) return false;

  const data_t * payload = bytes + out.data_offset;
  size_t payload_size = len - out.data_offset;

  /* A load address of zero means the first two bytes of the payload are the
   * address, exactly like a PRG. */
  if (out.load_addr == 0) {
    if (payload_size < 2) return false;
    out.load_addr = static_cast<addr_t>(payload[0] | (payload[1] << 8));
    payload += 2;
    payload_size -= 2;
  }

  out.data = payload;
  out.data_size = payload_size;
  out.load_last_addr =
    static_cast<addr_t>(out.load_addr + payload_size - 1);

  /* Flags bit 1 in an RSID says the tune is a C64 BASIC program. The spec then
   * requires initAddress to be zero, and the tune is started by RUN rather than
   * by calling anything. Read before the defaulting below, which would otherwise
   * hide the zero that identifies it. */
  out.is_basic = out.is_rsid && ((out.flags & 0x02) != 0) && (out.init_addr == 0);

  /* An init address of zero means "the load address", except for the BASIC case
   * above, where there is no init address at all. */
  if (out.init_addr == 0) out.init_addr = out.load_addr;

  if (out.songs == 0) out.songs = 1;
  if (out.start_song == 0) out.start_song = 1;
  if (out.start_song > out.songs) out.start_song = 1;

  /* flags bits 2 and 3: the video standard the tune was written for */
  switch ((out.flags >> 2) & 0x03) {
    case 0x01: out.video_model = VideoModel::Pal6569;    out.video_known = true; break;
    case 0x02: out.video_model = VideoModel::Ntsc6567R8; out.video_known = true; break;
    default: break; /* unknown or "any", the caller decides */
  }

  /* flags bits 4 and 5: which SID the tune expects */
  switch ((out.flags >> 4) & 0x03) {
    case 0x01: out.sid_model = SidModel::Mos6581; break;
    case 0x02: out.sid_model = SidModel::Mos8580; break;
    case 0x03: out.sid_model = SidModel::Any;     break;
    default:   out.sid_model = SidModel::Unknown; break;
  }

  /* Extra chips. Version 3 puts the second chip in the high byte of the
   * reserved word, version 4 adds a third in the low byte. */
  out.sid_addr[0] = 0xd400;
  out.sid_count = 1;
  if (plus) {
    /* the extended header puts one address per word */
    const addr_t second = decode_sid_addr(bytes[0x7a]);
    const addr_t third  = decode_sid_addr(bytes[0x7c]);
    const addr_t fourth = decode_sid_addr(bytes[0x7e]);
    if (second != 0) out.sid_addr[out.sid_count++] = second;
    if (third  != 0) out.sid_addr[out.sid_count++] = third;
    if (fourth != 0) out.sid_addr[out.sid_count++] = fourth;
  } else {
    if (out.version >= 3) {
      const addr_t second = decode_sid_addr(static_cast<uint8_t>(out.reserved >> 8));
      if (second != 0) out.sid_addr[out.sid_count++] = second;
    }
    if (out.version >= 4) {
      const addr_t third = decode_sid_addr(static_cast<uint8_t>(out.reserved & 0xff));
      if (third != 0) out.sid_addr[out.sid_count++] = third;
    }
  }

  out.valid = true;
  return true;
}

} /* namespace usbsid */
