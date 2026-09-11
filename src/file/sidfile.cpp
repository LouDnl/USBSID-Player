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
#include <initializer_list>

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

/**
 * @brief Decode the v5 96 byte metadata area at +0x16.
 *
 * Three Windows-1252 strings, name then author then released, each
 * null-terminated except released may instead fill the area exactly. Returns
 * false when the layout does not hold (any field missing, empty, or run past
 * the 96 bytes), which tells the caller to fall back to the fixed 32 byte
 * fields instead. See the v5 section of the file format doc for the worked
 * example this mirrors exactly.
 */
bool parse_meta_v5(const data_t * area, char * name, size_t name_cap,
                    char * author, size_t author_cap,
                    char * released, size_t released_cap)
{
  constexpr size_t kArea = 96;

  size_t p = 0;
  while (p < kArea && area[p] != 0) p++;
  if (p == 0 || p >= kArea) return false; /* name empty, or never terminated */
  const size_t name_len = p;

  size_t q = p + 1;
  while (q < kArea && area[q] == 0) q++; /* skip padding */
  if (q >= kArea) return false;          /* no room left for author */
  size_t q2 = q;
  while (q2 < kArea && area[q2] != 0) q2++;
  if (q2 >= kArea) return false;         /* author never terminated */
  const size_t author_len = q2 - q;
  if (author_len == 0) return false;

  size_t r = q2 + 1;
  while (r < kArea && area[r] == 0) r++; /* skip padding */
  if (r >= kArea) return false;          /* no room left for released */
  size_t rp = r;
  while (rp < kArea && area[rp] != 0) rp++;
  const size_t released_len = rp - r;    /* rp == kArea: fills exactly, no terminator needed */
  if (released_len == 0) return false;

  auto copy = [](char * dst, size_t cap, const data_t * src, size_t n) {
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
  };
  copy(name, name_cap, area, name_len);
  copy(author, author_cap, area + q, author_len);
  copy(released, released_cap, area + r, released_len);
  return true;
}

/* ---- v5 SID panning ------------------------------------------------------
 *
 * flags bits 6-7 (layout) and 8-9 (mode). Every formula below is transcribed
 * from the spec's own description of each mode/layout combination and was
 * checked against every row of the spec's worked tables (N = 1 to 15) while
 * this was written; nothing here is guessed.
 */

void pan_fill(SidPan * out, uint8_t start, uint8_t count, SidPan p)
{
  for (uint8_t i = 0; i < count; i++) out[start + i] = p;
}

void pan_direct(SidPanLayout layout, uint8_t n, SidPan * out)
{
  switch (layout) {
    case SidPanLayout::Standard:
      for (uint8_t i = 0; i < n; i++) out[i] = (i % 2 == 0) ? SidPan::Left : SidPan::Right;
      break;
    case SidPanLayout::LCR: {
      static constexpr SidPan seq[3] = { SidPan::Left, SidPan::Center, SidPan::Right };
      for (uint8_t i = 0; i < n; i++) out[i] = seq[i % 3];
      break;
    }
    case SidPanLayout::CenterFirst: {
      const uint8_t c_count = static_cast<uint8_t>(n - (n / 3) * 2);
      uint8_t i = 0;
      for (; i < c_count; i++) out[i] = SidPan::Center;
      bool left = true;
      for (; i < n; i++) { out[i] = left ? SidPan::Left : SidPan::Right; left = !left; }
      break;
    }
    case SidPanLayout::FullyCentered:
      pan_fill(out, 0, n, SidPan::Center);
      break;
  }
}

void pan_reverse_lr(SidPan * out, uint8_t n)
{
  for (uint8_t i = 0; i < n; i++) {
    if (out[i] == SidPan::Left) out[i] = SidPan::Right;
    else if (out[i] == SidPan::Right) out[i] = SidPan::Left;
  }
}

/* Stable partition: every non-Center entry first, in order, then every Center. */
void pan_move_center_to_end(SidPan * out, uint8_t n)
{
  SidPan tmp[kMaxSids];
  uint8_t w = 0;
  for (uint8_t i = 0; i < n; i++) if (out[i] != SidPan::Center) tmp[w++] = out[i];
  for (uint8_t i = 0; i < n; i++) if (out[i] == SidPan::Center) tmp[w++] = out[i];
  for (uint8_t i = 0; i < n; i++) out[i] = tmp[i];
}

void pan_group(SidPanLayout layout, uint8_t n, SidPan * out)
{
  if (layout == SidPanLayout::FullyCentered) { pan_fill(out, 0, n, SidPan::Center); return; }
  if (n == 1) { out[0] = SidPan::Center; return; }

  if (layout == SidPanLayout::Standard) {
    if (n == 2) { out[0] = SidPan::Left; out[1] = SidPan::Right; return; }
    uint8_t i = 0;
    bool left = true;
    while (i < n) {
      const uint8_t sz = static_cast<uint8_t>((n - i >= 2) ? 2 : 1);
      pan_fill(out, i, sz, left ? SidPan::Left : SidPan::Right);
      i = static_cast<uint8_t>(i + sz);
      left = !left;
    }
    return;
  }

  if (layout == SidPanLayout::LCR) {
    /* 1-9 SIDs: one group. 10-15: a group of 6, then the remaining 4-9. */
    const uint8_t g1 = static_cast<uint8_t>((n <= 9) ? n : 6);
    const uint8_t g2 = static_cast<uint8_t>(n - g1);
    uint8_t pos = 0;
    for (uint8_t g : { g1, g2 }) {
      if (g == 0) continue;
      const uint8_t lc = static_cast<uint8_t>((g + 1) / 3);
      const uint8_t rc = lc;
      const uint8_t cc = static_cast<uint8_t>(g - lc - rc);
      pan_fill(out, pos, lc, SidPan::Left);   pos = static_cast<uint8_t>(pos + lc);
      pan_fill(out, pos, cc, SidPan::Center); pos = static_cast<uint8_t>(pos + cc);
      pan_fill(out, pos, rc, SidPan::Right);  pos = static_cast<uint8_t>(pos + rc);
    }
    return;
  }

  /* CenterFirst */
  const uint8_t lc = static_cast<uint8_t>(n / 3);
  const uint8_t rc = lc;
  const uint8_t cc = static_cast<uint8_t>(n - lc - rc);
  pan_fill(out, 0, cc, SidPan::Center);
  uint8_t pos = cc;
  if (lc == 1) {
    out[pos++] = SidPan::Left;
    out[pos++] = SidPan::Right;
  } else if (lc > 0) {
    uint8_t remaining_l = lc, remaining_r = rc;
    bool left = true;
    if (lc % 2 != 0) { /* odd, > 1: three of each first */
      pan_fill(out, pos, 3, SidPan::Left);  pos = static_cast<uint8_t>(pos + 3);
      pan_fill(out, pos, 3, SidPan::Right); pos = static_cast<uint8_t>(pos + 3);
      remaining_l = static_cast<uint8_t>(remaining_l - 3);
      remaining_r = static_cast<uint8_t>(remaining_r - 3);
    }
    while (remaining_l > 0 || remaining_r > 0) {
      pan_fill(out, pos, 2, left ? SidPan::Left : SidPan::Right);
      pos = static_cast<uint8_t>(pos + 2);
      if (left) remaining_l = static_cast<uint8_t>(remaining_l - 2);
      else      remaining_r = static_cast<uint8_t>(remaining_r - 2);
      left = !left;
    }
  }
}

void pan_spread(SidPanLayout layout, uint8_t n, SidPan * out)
{
  if (layout == SidPanLayout::FullyCentered || n == 1) { pan_fill(out, 0, n, SidPan::Center); return; }

  if (layout == SidPanLayout::Standard) {
    const uint8_t lc = static_cast<uint8_t>((n + 1) / 2); /* odd total adds 1 to L */
    const uint8_t rc = static_cast<uint8_t>(n - lc);
    pan_fill(out, 0, lc, SidPan::Left);
    pan_fill(out, lc, rc, SidPan::Right);
    return;
  }

  const uint8_t lc = static_cast<uint8_t>(n / 3);
  const uint8_t rc = lc;
  const uint8_t cc = static_cast<uint8_t>(n - lc - rc);
  if (layout == SidPanLayout::LCR) {
    pan_fill(out, 0, lc, SidPan::Left);
    pan_fill(out, lc, cc, SidPan::Center);
    pan_fill(out, static_cast<uint8_t>(lc + cc), rc, SidPan::Right);
  } else { /* CenterFirst: C block, then L, then R */
    pan_fill(out, 0, cc, SidPan::Center);
    pan_fill(out, cc, lc, SidPan::Left);
    pan_fill(out, static_cast<uint8_t>(cc + lc), rc, SidPan::Right);
  }
}

void compute_panning(SidPanLayout layout, SidPanMode mode, uint8_t n, SidPan * out)
{
  if (n == 0) return;
  if (n > kMaxSids) n = kMaxSids;
  if (n == 1) { out[0] = SidPan::Center; return; }
  if (layout == SidPanLayout::FullyCentered) { pan_fill(out, 0, n, SidPan::Center); return; }

  switch (mode) {
    case SidPanMode::Direct:
      pan_direct(layout, n, out);
      break;
    case SidPanMode::Reverse:
      pan_direct(layout, n, out);
      pan_reverse_lr(out, n);
      if (layout == SidPanLayout::CenterFirst) pan_move_center_to_end(out, n);
      break;
    case SidPanMode::Group:
      pan_group(layout, n, out);
      break;
    case SidPanMode::Spread:
      pan_spread(layout, n, out);
      break;
  }
}

} /* namespace */

uint32_t SidFile::embedded_song_length_ms(uint16_t song) const
{
  if (song_length_table == nullptr || song < 1 || song > song_length_table_count) return 0;
  const data_t * p = song_length_table + (static_cast<size_t>(song - 1) * 4);
  /* Each byte is two BCD digits. Word 0 is MM:SS, word 1 is milliseconds with
   * its top nibble always zero (0x0500 = 500ms), per the spec's own example. */
  auto bcd = [](data_t b) -> uint32_t {
    return static_cast<uint32_t>((b >> 4) * 10u + (b & 0x0fu));
  };
  const uint32_t minutes = bcd(p[0]);
  const uint32_t seconds = bcd(p[1]);
  const uint32_t ms = (p[2] & 0x0fu) * 100u + (p[3] >> 4) * 10u + (p[3] & 0x0fu);
  return ((minutes * 60u + seconds) * 1000u) + ms;
}

bool sidfile_parse(const data_t * bytes, size_t len, SidFile & out)
{
  out = SidFile{};
  if (bytes == nullptr || len < 0x76) return false;

  const bool psid = memcmp(bytes, "PSID", 4) == 0;
  const bool rsid = memcmp(bytes, "RSID", 4) == 0;
  if (!psid && !rsid) return false;

  out.is_rsid = rsid;
  out.version = be16(bytes + 0x04);
  /* Versions 1 to 5 are the documented ones. The four SID community also
   * ships an extended header, 130 bytes long, that carries a fourth chip
   * address; old player lists those offsets as SIDFILEPLUS, and real files
   * ship version numbers above 4 for it too. It is accepted here rather than
   * rejected, because the tunes exist and play. It cannot collide with a
   * real v5 file: v5 keeps the ordinary 0x7c byte header, so "plus" is only
   * ever true when dataOffset says the header is bigger than that. */
  const bool plus = (out.version > 4) && (len >= 0x82) && (be16(bytes + 0x06) >= 0x82);
  const bool v5 = (out.version == 5) && !plus;
  if (!plus && (out.version < 1 || out.version > 5)) return false;

  out.data_offset = be16(bytes + 0x06);
  out.load_addr   = be16(bytes + 0x08);
  out.init_addr   = be16(bytes + 0x0a);
  out.play_addr   = be16(bytes + 0x0c);
  out.songs       = be16(bytes + 0x0e);
  out.start_song  = be16(bytes + 0x10);
  out.speed       = be32(bytes + 0x12);

  /* v5 allows a variable length metadata area; fall back to the classic fixed
   * 32 byte fields whenever it does not parse cleanly (see parse_meta_v5). */
  if (!v5 || !parse_meta_v5(bytes + 0x16, out.name, sizeof(out.name),
                             out.author, sizeof(out.author),
                             out.released, sizeof(out.released))) {
    copy_text(out.name,     bytes + 0x16, 32);
    copy_text(out.author,   bytes + 0x36, 32);
    copy_text(out.released, bytes + 0x56, 32);
  }

  if (out.version >= 2 || plus) {
    if (len < 0x7c) return false;
    out.flags      = be16(bytes + 0x76);
    out.start_page = bytes[0x78];
    out.max_pages  = bytes[0x79];
    out.reserved   = be16(bytes + 0x7a);
  }

  /* v5 specific flags bits: 6-7 panning layout, 8-9 panning mode, 10 embedded
   * song lengths, 11 FM OPL. Versions 3 and 4 use bits 6-9 for the second and
   * third chip's SID model instead; that meaning is not read into anything
   * today, so nothing needs guarding here beyond not misreading it as v5. */
  if (v5) {
    out.pan_layout = static_cast<SidPanLayout>((out.flags >> 6) & 0x03);
    out.pan_mode   = static_cast<SidPanMode>((out.flags >> 8) & 0x03);
    out.has_embedded_song_lengths = (out.flags & 0x0400) != 0;
    out.has_fm_opl                = (out.flags & 0x0800) != 0;
  }

  if (out.data_offset == 0 || out.data_offset >= len) return false;

  const data_t * payload = bytes + out.data_offset;
  size_t payload_size = len - out.data_offset;

  /* The song length table, when present, is the last (songs * 4) bytes of
   * the whole file and is not part of the C64 payload; strip it before the
   * load-address-prefix handling below, which operates on the payload only. */
  if (out.has_embedded_song_lengths && out.songs > 0) {
    const size_t table_bytes = static_cast<size_t>(out.songs) * 4;
    if (table_bytes <= payload_size && table_bytes <= len) {
      payload_size -= table_bytes;
      out.song_length_table = bytes + (len - table_bytes);
      out.song_length_table_count = out.songs;
    } else {
      out.has_embedded_song_lengths = false; /* declared but does not fit: ignore rather than fail the whole file */
    }
  }

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
   * reserved word, version 4 adds a third in the low byte, v5 replaces both
   * bytes with multiSidConfig (chip count + address step configuration) and
   * sidAddressConfigStart (an optional override for SID2's address). */
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
  } else if (v5) {
    const uint8_t multi_cfg = bytes[0x7a];
    const uint8_t addr_start_byte = bytes[0x7b];
    const uint8_t total = multi_cfg & 0x0f; /* 0 = single SID, 2-15 = chip count */

    if (total >= 2) {
      const uint8_t cfg_sel = static_cast<uint8_t>((multi_cfg >> 4) & 0x0f);
      static constexpr uint16_t kSingleStep[4] = { 0x0020, 0x0040, 0x0080, 0x0100 };
      uint16_t steps[2] = { 0, 0 };
      uint8_t step_count = 0;
      if (cfg_sel <= 3) {
        steps[0] = kSingleStep[cfg_sel];
        step_count = 1;
      } else if (cfg_sel == 4) { steps[0] = 0x0020; steps[1] = 0x0060; step_count = 2; }
      else if (cfg_sel == 5)   { steps[0] = 0x0020; steps[1] = 0x00e0; step_count = 2; }
      else if (cfg_sel == 6)   { steps[0] = 0x0100; steps[1] = 0x0300; step_count = 2; }
      /* 7-15 are reserved: step_count stays 0 and no further chip is generated. */

      if (step_count > 0) {
        /* 0x00 or anything outside the valid ranges means "use the standard
         * start", i.e. no override: decode_sid_addr() already returns 0 for
         * both, which is exactly what falling through to the plain step
         * below needs. */
        const addr_t override_addr = decode_sid_addr(addr_start_byte);
        const uint8_t want = (total > kMaxSids) ? kMaxSids : total;
        uint32_t addr = 0xd400;
        for (uint8_t k = 1; k < want; k++) {
          uint32_t next;
          if (k == 1 && override_addr != 0) {
            next = override_addr;
          } else {
            next = addr + steps[(k - 1) % step_count];
            if (next > 0xd7e0 && next < 0xde00) {
              /* Landed in the invalid $D800-$DDF0 gap: resume at $DE00,
               * keeping whatever remainder carried past $D800. A value
               * already at or past $DE00 is either valid as is or past
               * $DFE0, which the check below stops. */
              next = 0xde00 + (next - 0xd800);
            }
          }
          if (next > 0xdfe0) break; /* past the end of the high block: stop */
          /* An FM/OPL tune reserves the whole $DF00-$DFFF range; a SID chip
           * generated inside it is an invalid configuration, not just an
           * ignorable one. */
          if (out.has_fm_opl && next >= 0xdf00 && next <= 0xdfff) return false;
          addr = next;
          out.sid_addr[out.sid_count++] = static_cast<addr_t>(addr);
        }
      }
    }
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

  compute_panning(out.pan_layout, out.pan_mode, out.sid_count, out.sid_pan);

  out.valid = true;
  return true;
}

} /* namespace usbsid */
