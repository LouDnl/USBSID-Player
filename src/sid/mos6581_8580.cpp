/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6581_8580.cpp
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

#include "mos6581_8580.h"

#include "util/logging.h"

namespace usbsid {

namespace {
/* USBSID-Pico takes a cycle delta with every write and cannot carry more
 * than sixteen bits, so longer gaps are sent as explicit waits first. */
constexpr uint32_t kMaxDelta = 0xffff;

/* The FM/OPL addresses live in the second expansion IO page */
constexpr addr_t kFmOplAddrA = 0xdf40;
constexpr addr_t kFmOplAddrB = 0xdf50;

/* Where an unclaimed FM/OPL write is parked: one 32 byte block past the
 * last real chip (kMaxSids * 0x20 = 0x1e0), inside regs_[]'s own 0x200. */
constexpr addr_t kFmOplParkBase = 0x01e0;

/* The PLA decodes the whole of this range to the SID */
constexpr addr_t kSidPageFirst = 0xd400;
constexpr addr_t kSidPageLast  = 0xd7ff;
} /* namespace */

Mos6581_8580::Mos6581_8580(Bus & bus, SidBackend & backend)
  : bus_(bus), backend_(&backend)
{
  reset();
}

void Mos6581_8580::reset(void)
{
  for (size_t i = 0; i < kRegsSize; i++) regs_[i] = 0;
  last_event_ = bus_.cycles();
  for (uint8_t i = 0; i < kMaxSids; i++) {
    voice3_[i].reset();
    voice3_[i].resync(last_event_);
  }
  writes_ = 0;
  reads_ = 0;
  backend_->reset();
}

/**
 * @brief The addresses a chip answers beyond its own base.
 *
 * A single SID is decoded across all of $d400-$d7ff (32 mirrors); $d7ff is
 * really register $1f, hence its use as a test scratch address. With
 * multiple chips, spare addresses fold onto the first chip.
 */
bool Mos6581_8580::custom_address(addr_t addr) const
{
  if (addr >= kSidPageFirst && addr <= kSidPageLast) return true;
  const addr_t page = static_cast<addr_t>(addr & 0xfff0);
  return (page >= 0xd420 && page <= 0xd45f) ||
         (page >= 0xd5c0 && page <= 0xd5df);
}

addr_t Mos6581_8580::translate(addr_t addr, uint8_t & chip) const
{
  chip = 0;

  /* Where socket two starts, when writes are being forced into it */
  data_t socket_offset = 0;
  if (config_.force_address) {
    socket_offset = config_.forced_address;
  } else if (config_.force_socket_two) {
    socket_offset = (config_.sids_socket_one == 1) ? 0x20
                  : (config_.sids_socket_one == 2) ? 0x40
                                                   : 0x00;
  }

  /* The FM/OPL address goes to whichever chip is configured for it */
  if (addr == kFmOplAddrA || addr == kFmOplAddrB) {
    if (config_.fmopl_sid >= 1 && config_.fmopl_sid <= 4) {
      chip = static_cast<uint8_t>(config_.fmopl_sid);
      return static_cast<addr_t>(((chip - 1) * 0x20) + (addr & 0x1f));
    }
    /* Nothing claims it: park past every real chip's range so a backend
     * seeing more than 8 chips can still tell a real chip 5+ write apart
     * from this one. `chip` is one past the last real chip for the same
     * reason. */
    chip = static_cast<uint8_t>(kMaxSids + 1);
    return static_cast<addr_t>(kFmOplParkBase + (addr & 0x1f));
  }

  const uint8_t count = (config_.count == 0) ? 1
                      : (config_.count > kMaxSids) ? kMaxSids : config_.count;

  for (uint8_t n = 0; n < count; n++) {
    const addr_t base = config_.base[n];
    if (base == 0) continue;
    if (addr >= base && addr < static_cast<addr_t>(base + 0x20)) {
      chip = static_cast<uint8_t>(n + 1);
      const addr_t reg = static_cast<addr_t>((n * 0x20) + (addr & 0x1f));
      /* only the first chip can be pushed into the other socket */
      return (n == 0) ? static_cast<addr_t>(socket_offset + (addr & 0x1f)) : reg;
    }
  }

  /* anything else inside the SID page belongs to the first chip */
  if (custom_address(addr)) {
    chip = 1;
    return static_cast<addr_t>(socket_offset + (addr & 0x1f));
  }

  return kSidNotMapped;
}

/**
 * @brief Cycles since the previous SID event, as the hardware wants them.
 *
 * Gaps over 16 bits are sent as an explicit wait() first. One cycle is
 * subtracted (the access itself costs a cycle of hardware time). Only
 * called for accesses that actually reach the hardware - a read served
 * from the mirror or a write to an unclaimed address must not call this,
 * since consuming the gap without sending it anywhere loses that time and
 * runs playback fast.
 */
uint16_t Mos6581_8580::cycles_since_last_event(void)
{
  const cycle_t now = bus_.cycles();
  uint32_t delta = static_cast<uint32_t>(now - last_event_);

  while (delta > kMaxDelta) {
    delta -= kMaxDelta;
    backend_->wait(static_cast<uint16_t>(kMaxDelta));
  }
  last_event_ = now;

  /* the access itself is a cycle of hardware time */
  const uint32_t overhead = config_.access_overhead;
  return static_cast<uint16_t>((delta > overhead) ? (delta - overhead) : 0);
}

namespace {

/* Which voice a chip local control register belongs to, or 0 for none. */
inline uint8_t control_reg_voice(addr_t local)
{
  switch (local & 0x1f) {
    case 0x04: return 1;
    case 0x0b: return 2;
    case 0x12: return 3;
    default:   return 0;
  }
}

/* Which voice a chip local sustain/release register belongs to, or 0 for none.
 * Together with the control register above, these are the only two registers a
 * mute touches. */
inline uint8_t sr_reg_voice(addr_t local)
{
  switch (local & 0x1f) {
    case 0x06: return 1;
    case 0x0d: return 2;
    case 0x14: return 3;
    default:   return 0;
  }
}

constexpr data_t kGateBit = 0x01;
/* Sustain is the high nibble of the sustain/release register; release is the
 * low nibble and is left exactly as the tune wrote it. */
constexpr data_t kSustainMask = 0xf0;

} /* namespace */

/**
 * @brief The value the hardware should see, which is not always what was written.
 *
 * For a muted voice: control ($04/$0b/$12) gets its gate bit forced to 0,
 * sustain ($06/$0d/$14) gets its sustain nibble forced to 0. Gate alone
 * isn't enough (release can run up to 24s audible); everything else
 * passes through unchanged.
 */
data_t Mos6581_8580::mask_for_output(addr_t reg, data_t value) const
{
  /* Only ever called with a reg translate() put inside a real chip's own
   * range (io_write()'s `chip >= 1 && chip <= kMaxSids` guard), so no mask
   * is needed here to stay inside voice_mute[]'s own kMaxSids entries. */
  const uint8_t chip = static_cast<uint8_t>(reg >> 5);
  const uint8_t mask = config_.voice_mute[chip];
  if (mask == 0) return value;

  const uint8_t control = control_reg_voice(reg);
  if (control != 0) {
    if ((mask & (1u << (control - 1))) == 0) return value;
    return static_cast<data_t>(value & ~kGateBit);
  }

  const uint8_t sr = sr_reg_voice(reg);
  if (sr != 0) {
    if ((mask & (1u << (sr - 1))) == 0) return value;
    return static_cast<data_t>(value & ~kSustainMask);
  }

  return value;
}

void Mos6581_8580::set_voice_mute(uint8_t chip, uint8_t voice, bool muted)
{
  if (chip < 1 || chip > kMaxSids || voice < 1 || voice > 3) return;
  const uint8_t bit = static_cast<uint8_t>(1u << (voice - 1));
  uint8_t & mask = config_.voice_mute[chip - 1];
  const bool was = (mask & bit) != 0;
  if (was == muted) return;
  mask = static_cast<uint8_t>(muted ? (mask | bit) : (mask & ~bit));

  /* Setting the mask alone doesn't silence an already-sounding note (gate
   * holds until next write), so push both registers now and on the way
   * back. Sustain goes first both directions: muting drops sustain to 0
   * while still gated so the envelope falls at decay rate, then clears
   * the gate (releases from zero, not from whatever level it held).
   * Unmuting restores sustain before the gate goes high. */
  static const addr_t kSustainRelease[3] = { 0x06, 0x0d, 0x14 };
  static const addr_t kControl[3]        = { 0x04, 0x0b, 0x12 };
  if (backend_ != nullptr) {
    const addr_t base = static_cast<addr_t>((chip - 1) * 0x20);
    const addr_t order[2] = {
      static_cast<addr_t>(base + kSustainRelease[voice - 1]),
      static_cast<addr_t>(base + kControl[voice - 1]),
    };
    for (const addr_t reg : order) {
      backend_->write(reg, mask_for_output(reg, regs_[reg & (kRegsSize - 1)]),
                      cycles_since_last_event());
      ++writes_;
    }
  }
}

void Mos6581_8580::set_voice_solo(uint8_t chip, uint8_t voice)
{
  if (chip < 1 || chip > kMaxSids || voice < 1 || voice > 3) return;
  config_.solo_active = true;
  config_.voice_solo[chip - 1] = static_cast<uint8_t>(
    config_.voice_solo[chip - 1] | (1u << (voice - 1)));
}

/**
 * @brief Hold a whole chip silent, dropping its writes.
 *
 * Dropping writes freezes a chip, doesn't quiet it, so volume goes to
 * zero first and the drop starts after. Only $18's low nibble is cleared
 * (high nibble is filter mode). Actual work happens in
 * apply_chip_mute_pending() on the emulating core - see its comment.
 */
void Mos6581_8580::set_chip_mute(uint8_t chip, bool muted)
{
  if (chip < 1 || chip > kMaxSids) return;
  const uint16_t bit = static_cast<uint16_t>(1u << (chip - 1));
  const bool was = (config_.chip_mute & bit) != 0;
  if (was == muted) return;

  config_.chip_mute = static_cast<uint16_t>(muted ? (config_.chip_mute | bit)
                                                  : (config_.chip_mute & ~bit));
  chip_mute_pending_ = static_cast<uint16_t>(chip_mute_pending_ | bit);
}

/**
 * @brief Do what set_chip_mute() could not, on the core that owns the backend.
 *
 * Unmuting replays everything the tune wrote while silent, register order
 * with volume last, so the chip resumes as the tune believes it to be.
 * Voice mutes still apply on the way out.
 */
void Mos6581_8580::apply_chip_mute_pending(void)
{
  const uint16_t pending = chip_mute_pending_;
  chip_mute_pending_ = 0;
  if (backend_ == nullptr) return;

  /* Reset last_event_ to now: a muted chip's writes are dropped before
   * cycle accounting, so the first write after unmute would otherwise
   * carry a delta covering the whole silent stretch and turn into a flood
   * of wait() calls, freezing playback for as long as it had been muted.
   * The board already lived through that silence in real time. No-op if
   * another chip kept writing (last_event_ is already now). */
  last_event_ = bus_.cycles();

  for (uint8_t chip = 1; chip <= kMaxSids; ++chip) {
    const uint16_t bit = static_cast<uint16_t>(1u << (chip - 1));
    if ((pending & bit) == 0) continue;

    const addr_t base = static_cast<addr_t>((chip - 1) * 0x20);
    const addr_t vol = static_cast<addr_t>(base + 0x18);

    if ((config_.chip_mute & bit) != 0) {
      backend_->write(vol, static_cast<data_t>(regs_[vol & (kRegsSize - 1)] & 0xf0),
                      cycles_since_last_event());
      ++writes_;
      continue;
    }

    for (addr_t r = 0; r <= 0x17; ++r) {
      const addr_t reg = static_cast<addr_t>(base + r);
      backend_->write(reg, mask_for_output(reg, regs_[reg & (kRegsSize - 1)]),
                      cycles_since_last_event());
      ++writes_;
    }
    backend_->write(vol, regs_[vol & (kRegsSize - 1)], cycles_since_last_event());
    ++writes_;
  }
}

void Mos6581_8580::io_write(addr_t addr, data_t value)
{
  uint8_t chip = 0;
  const addr_t reg = translate(addr, chip);

  if (reg == kSidNotMapped) return;

  /* A mute changed since the last write; serviced here since this runs on
   * the core that owns the backend and cycle accounting. Before the write
   * below, so a chip coming back is already itself for the next write. */
  if (chip_mute_pending_ != 0) apply_chip_mute_pending();

  regs_[reg & (kRegsSize - 1)] = value;

  /* Unclaimed FM/OPL addresses (kMaxSids + 1 from translate()) go to the
   * backend as reg values out of any real chip's range; a backend that
   * can't use them drops them (UsbSidBackend/EmbeddedSidBackend both
   * guard on reg >= 0x80, well below kFmOplParkBase). */
  if (chip == kMaxSids + 1) {
    backend_->write(reg, value, cycles_since_last_event());
    ++writes_;
    return;
  }

  if (chip >= 1 && chip <= kMaxSids) {
    const bool chip_is_muted = (config_.chip_mute & (1u << (chip - 1))) != 0;

    /* --solo: local < 0x15 is one of the three per-voice register blocks
     * (7 bytes each: freq lo/hi, pulse lo/hi, control, ad, sr); 0x15-0x18
     * is the chip-shared filter/volume block, dropped only when the chip
     * has no solo'd voice at all - a solo'd voice still needs its shared
     * filter/volume to reach the backend to be heard. */
    bool solo_dropped = false;
    if (config_.solo_active) {
      const addr_t local = reg & 0x1f;
      const uint8_t keep = config_.voice_solo[chip - 1];
      solo_dropped = (local >= 0x15) ? (keep == 0)
                                     : ((keep & (1u << (local / 7))) == 0);
    }

    /* Gated on mute/solo state so -srw shows only the writes that
     * actually reach the backend, not the full stream. */
    if (!chip_is_muted && !solo_dropped) {
      US_LOG_IF(sid_rw, "[W SID%u] $%04x $%03x:%02x [C]%5u\n", chip, addr, reg,
                value, static_cast<unsigned>(bus_.cycles() - last_event_));
    }
    /* Voice three follows along so $d41b/$d41c can answer, fed the
     * register and cycle so it can be caught up lazily and stay exact.
     * Kept running even when muted/solo-dropped, same reasoning as the
     * muted-chip comment below. */
    voice3_[chip - 1].write(static_cast<reg_t>(addr & 0x1f), value,
                            bus_.cycles());
    /* A muted chip's writes are dropped here, and only here (after the
     * mirror and voice three, so $d41b/$d41c answer the same muted or
     * not - tunes poll those as a timer and random source). Solo-dropped
     * writes leave the same way, for the same reason. */
    if (chip_is_muted || solo_dropped) return;

    /* Voice mute is applied only here, on the way out. */
    backend_->write(reg, mask_for_output(reg, value), cycles_since_last_event());
    ++writes_;
  }
}

data_t Mos6581_8580::io_read(addr_t addr)
{
  uint8_t chip = 0;
  const addr_t reg = translate(addr, chip);

  if (reg == kSidNotMapped) return 0xff;

  ++reads_;

  /* Only voice three's oscillator/envelope registers are readable. With
   * real_reads the answer comes from the chip itself; otherwise voice
   * three is emulated so $d41b/$d41c (timer, random source) still move. */
  const reg_t local = static_cast<reg_t>(addr & 0x1f);

  if (config_.real_reads && chip >= 1 && chip <= kMaxSids) {
    return backend_->read(reg, cycles_since_last_event());
  }

  /* --solo: a chip with no solo'd voice at all never reaches the backend
   * (see io_write()), so its reads are pure emulation noise for whoever
   * asked to isolate SPEC's traffic with -srw - suppressed the same way
   * writes are, chip-granular since $d41b/$d41c are voice three's alone
   * but a tune polls them regardless of which voice it cares about. */
  const bool solo_read_dropped = config_.solo_active && chip >= 1 &&
    chip <= kMaxSids && config_.voice_solo[chip - 1] == 0;

  if (chip >= 1 && chip <= kMaxSids) {
    if (local == kSidRegOsc3 || local == kSidRegEnv3) {
      const data_t value = (local == kSidRegOsc3)
        ? voice3_[chip - 1].osc3(bus_.cycles())
        : voice3_[chip - 1].env3(bus_.cycles());
      if (!solo_read_dropped) {
        US_LOG_IF(sid_rw, "[R SID%u] $%04x $%03x:%02x\n", chip, addr, reg, value);
      }
      return value;
    }
  }
  if (!solo_read_dropped) {
    US_LOG_IF(sid_rw, "[R SID%u] $%04x $%03x:%02x (mirror)\n", chip, addr, reg,
              regs_[reg & (kRegsSize - 1)]);
  }

  /* Everything else floats on real hardware; the mirror is the most
   * useful thing to hand back. */
  return regs_[reg & (kRegsSize - 1)];
}

void Mos6581_8580::vic_frame_ended(void)
{
  /* Software SID render_idle: pushes outstanding time at the frame
   * boundary, same kMaxDelta chunking an access would use, so a tune that
   * writes nothing for many frames still renders silence instead of
   * racing ahead. Hardware skips this - there the deltas are the clock
   * and handing it idle time would invent work. */
  if (config_.render_idle) {
    const cycle_t now = bus_.cycles();
    uint32_t delta = static_cast<uint32_t>(now - last_event_);
    while (delta > kMaxDelta) {
      delta -= kMaxDelta;
      backend_->wait(static_cast<uint16_t>(kMaxDelta));
    }
    if (delta > 0) backend_->wait(static_cast<uint16_t>(delta));
    last_event_ = now;
  }

  /* A flush is a transport event, not a timing one - the delta base is
   * left alone so the next frame's first write carries the whole gap
   * since the last write, across the frame boundary. */
  backend_->flush();
}

} /* namespace usbsid */
