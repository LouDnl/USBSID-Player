/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * mos6581_8580.cpp
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

#include "mos6581_8580.h"

#include "util/logging.h"

namespace usbsid {

namespace {
/* USBSID-Pico takes a 16 bit cycle delta with every write, longer gaps are sent as waits first */
constexpr uint32_t kMaxDelta = 0xffff;

/* The FM/OPL addresses live in the second expansion IO page */
constexpr addr_t kFmOplAddrA = 0xdf40;
constexpr addr_t kFmOplAddrB = 0xdf50;
/* The cartridge decodes its ports (status read included) in this window */
constexpr addr_t kFmOplWindowFirst = 0xdf40;
constexpr addr_t kFmOplWindowLast  = 0xdf7f;

/* Park block of an unclaimed FM/OPL write, 32 bytes past the last real chip (kMaxSids * 0x20 = 0x1e0) in regs_[] */
constexpr addr_t kFmOplParkBase = 0x01e0;

/* The PLA decodes this whole range to the SID */
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
 * @brief The addresses a chip answers beyond its own base
 *
 * A single SID is decoded across all of $d400-$d7ff (32 mirrors)
 * $d7ff is register $1f, used as test scratch address
 * With multiple chips spare addresses fold onto the first chip
 */
bool Mos6581_8580::custom_address(addr_t addr) const
{
  if (addr >= kSidPageFirst && addr <= kSidPageLast) return true;
  const addr_t page = static_cast<addr_t>(addr & 0xfff0);
  return (page >= 0xd420 && page <= 0xd45f) ||
         (page >= 0xd5c0 && page <= 0xd5df);
}

/**
 * @brief Board register the first chip of the tune is forced to start at
 *
 * Return forced_address with -fa, first register of socket two with -f, else 0
 */
addr_t Mos6581_8580::socket_offset(void) const
{
  if (config_.force_address) return config_.forced_address;
  if (config_.force_socket_two) {
    return (config_.sids_socket_one == 1) ? 0x20
         : (config_.sids_socket_one == 2) ? 0x40
                                          : 0x00;
  }
  return 0;
}

/**
 * @brief Shift a tune chip register by the forced socket offset
 *
 * @param logical  register in the tune chip layout, (chip - 1) * 0x20
 * Return the board register, kSidNotMapped when the shift pushes it past the last real chip
 */
addr_t Mos6581_8580::physical_reg(addr_t logical) const
{
  const addr_t reg = static_cast<addr_t>(logical + socket_offset());
  return (reg < kFmOplParkBase) ? reg : kSidNotMapped;
}

addr_t Mos6581_8580::translate(addr_t addr, uint8_t & chip) const
{
  chip = 0;

  /* The FM/OPL address goes to the chip configured for it, up to kMaxSids
     (a logical slot with several boards) */
  if (addr == kFmOplAddrA || addr == kFmOplAddrB) {
    if (config_.fmopl_sid >= 1 && config_.fmopl_sid <= kMaxSids) {
      chip = static_cast<uint8_t>(config_.fmopl_sid);
      return static_cast<addr_t>(((chip - 1) * 0x20) + (addr & 0x1f));
    }
    /* Unclaimed: park past every real chip range, a backend with more than 8 chips
     * can tell it from a chip 5+ write, chip is one past the last real chip */
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
      /* -f/-fa move every chip along, tune chip order stays */
      return physical_reg(static_cast<addr_t>((n * 0x20) + (addr & 0x1f)));
    }
  }

  /* Anything else in the SID page belongs to the first chip */
  if (custom_address(addr)) {
    chip = 1;
    return physical_reg(static_cast<addr_t>(addr & 0x1f));
  }

  return kSidNotMapped;
}

/**
 * @brief Cycles since the previous SID event as the hardware wants them
 *
 * Gaps over 16 bits are sent as wait() first, one cycle is subtracted (the access costs a cycle)
 * Only call for accesses that reach the hardware, a mirror read or unclaimed write
 * must not consume the gap or playback runs fast
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

  /* The access is a cycle of hardware time */
  const uint32_t overhead = config_.access_overhead;
  return static_cast<uint16_t>((delta > overhead) ? (delta - overhead) : 0);
}

namespace {

/* Voice a chip local control register belongs to, 0 for none */
inline uint8_t control_reg_voice(addr_t local)
{
  switch (local & 0x1f) {
    case 0x04: return 1;
    case 0x0b: return 2;
    case 0x12: return 3;
    default:   return 0;
  }
}

/* Voice a chip local sustain/release register belongs to, 0 for none
 * With the control register above the only two registers a mute touches */
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
/* Sustain is the high nibble of sustain/release, release (low nibble) is left as written */
constexpr data_t kSustainMask = 0xf0;

} /* namespace */

/**
 * @brief The value the hardware should see
 *
 * Muted voice: control ($04/$0b/$12) gate bit forced to 0, sustain ($06/$0d/$14)
 * nibble forced to 0 (gate alone leaves up to 24s of release audible)
 * Everything else passes unchanged
 */
data_t Mos6581_8580::mask_for_output(addr_t reg, data_t value) const
{
  /* Called with a reg inside a real chip range
     (io_write() guard chip >= 1 && chip <= kMaxSids),
     no mask needed for voice_mute[] */
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

  /* A set mask does not silence a sounding note (gate holds until the following write)
   * Push both registers immediately, sustain first in both directions
   * Mute: sustain to 0 while gated (envelope falls at decay rate), then gate off (release from zero)
   * Unmute: sustain restored before gate goes high */
  static const addr_t kSustainRelease[3] = { 0x06, 0x0d, 0x14 };
  static const addr_t kControl[3]        = { 0x04, 0x0b, 0x12 };
  if (backend_ != nullptr) {
    io_addr_ = 0;
    io_chip_ = 0;
    const addr_t base = static_cast<addr_t>((chip - 1) * 0x20);
    const addr_t order[2] = {
      static_cast<addr_t>(base + kSustainRelease[voice - 1]),
      static_cast<addr_t>(base + kControl[voice - 1]),
    };
    for (const addr_t reg : order) {
      const addr_t out = physical_reg(reg);
      if (out == kSidNotMapped) continue;
      backend_->write(out, mask_for_output(reg, regs_[reg & (kRegsSize - 1)]),
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
 * @brief Hold a whole chip silent, dropping its writes
 *
 * Dropping freezes a chip, volume goes to zero first, then the drop starts
 * Only the low nibble of $18 is cleared (high nibble is filter mode)
 * The work is done in apply_chip_mute_pending() on the emulating core
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
 * @brief Apply the pending chip mutes on the core that owns the backend
 *
 * Unmute replays everything the tune wrote while silent, volume last
 * Voice mutes still apply on the way out
 */
void Mos6581_8580::apply_chip_mute_pending(void)
{
  const uint16_t pending = chip_mute_pending_;
  chip_mute_pending_ = 0;
  if (backend_ == nullptr) return;
  io_addr_ = 0;
  io_chip_ = 0;

  /* Reset last_event_ to the current time, writes of a muted chip are dropped before cycle accounting
   * The first write after unmute would carry the whole silent stretch as wait() calls
   * and freeze playback, the board lived through that silence in real time
   * No-op when another chip kept writing */
  last_event_ = bus_.cycles();

  for (uint8_t chip = 1; chip <= kMaxSids; ++chip) {
    const uint16_t bit = static_cast<uint16_t>(1u << (chip - 1));
    if ((pending & bit) == 0) continue;

    const addr_t base = static_cast<addr_t>((chip - 1) * 0x20);
    const addr_t vol = static_cast<addr_t>(base + 0x18);
    /* Skip a chip pushed past the last real one by -f/-fa */
    if (physical_reg(vol) == kSidNotMapped) continue;

    if ((config_.chip_mute & bit) != 0) {
      backend_->write(physical_reg(vol),
                      static_cast<data_t>(regs_[vol & (kRegsSize - 1)] & 0xf0),
                      cycles_since_last_event());
      ++writes_;
      continue;
    }

    for (addr_t r = 0; r <= 0x17; ++r) {
      const addr_t reg = static_cast<addr_t>(base + r);
      backend_->write(physical_reg(reg),
                      mask_for_output(reg, regs_[reg & (kRegsSize - 1)]),
                      cycles_since_last_event());
      ++writes_;
    }
    backend_->write(physical_reg(vol), regs_[vol & (kRegsSize - 1)],
                    cycles_since_last_event());
    ++writes_;
  }
}

void Mos6581_8580::io_write(addr_t addr, data_t value)
{
  uint8_t chip = 0;
  const addr_t reg = translate(addr, chip);

  if (reg == kSidNotMapped) return;

  /* A mute changed since the last write, serviced in this function (core that owns the backend and cycle accounting) before the write */
  if (chip_mute_pending_ != 0) apply_chip_mute_pending();

  io_addr_ = addr;
  io_chip_ = chip;
  /* Mirror in the tune chip layout, unshifted by -f/-fa */
  const addr_t mirror = (chip >= 1 && chip <= kMaxSids)
    ? static_cast<addr_t>(((chip - 1) * 0x20) + (addr & 0x1f)) : reg;
  regs_[mirror & (kRegsSize - 1)] = value;

  /* Unclaimed FM/OPL addresses (kMaxSids + 1 from translate()) reach the backend as
   * regs outside any real chip range, backends drop them (reg >= 0x80, below kFmOplParkBase) */
  if (chip == kMaxSids + 1) {
    US_LOG_IF(sid_rw, "[W FM] $%04x $%03x:%02x [C]%5u\n", addr, reg, value,
              static_cast<unsigned>(bus_.cycles() - last_event_));
    backend_->write(reg, value, cycles_since_last_event());
    ++writes_;
    return;
  }

  if (chip >= 1 && chip <= kMaxSids) {
    const bool chip_is_muted = (config_.chip_mute & (1u << (chip - 1))) != 0;

    /* --solo: local < 0x15 is one of 3 voice blocks (7 bytes: freq lo/hi, pulse lo/hi, control, ad, sr)
     * 0x15-0x18 is the shared filter/volume block, dropped only when the chip has
     * no solo'd voice, a solo'd voice needs it to be heard */
    bool solo_dropped = false;
    if (config_.solo_active) {
      const addr_t local = mirror & 0x1f;
      const uint8_t keep = config_.voice_solo[chip - 1];
      solo_dropped = (local >= 0x15) ? (keep == 0)
                                     : ((keep & (1u << (local / 7))) == 0);
    }

    /* Gated on mute/solo state, -srw/-sidw show only the writes that reach the backend */
    if (!chip_is_muted && !solo_dropped) {
      US_LOG_IF(sid_rw, "[W SID%u] $%04x $%03x:%02x [C]%5u\n", chip, addr, reg,
                value, static_cast<unsigned>(bus_.cycles() - last_event_));
      US_LOG_IF(sid_w, "[W! SID%u] $%04x $%03x:%02x [C]%5u\n", chip, addr, reg,
                value, static_cast<unsigned>(bus_.cycles() - last_event_));
    }
    /* Voice three follows along to answer $d41b/$d41c, fed register and cycle
     * to catch up lazily. Runs when muted/solo-dropped too */
    voice3_[chip - 1].write(static_cast<reg_t>(addr & 0x1f), value,
                            bus_.cycles());
    /* Writes of a muted or solo-dropped chip are dropped only in this function, after the
     * mirror and voice three ($d41b/$d41c answer the same, tunes poll them
     * as timer and random source) */
    if (chip_is_muted || solo_dropped) return;

    /* Voice mute is applied only on the way out */
    backend_->write(reg, mask_for_output(mirror, value), cycles_since_last_event());
    ++writes_;
  }
}

data_t Mos6581_8580::io_read(addr_t addr)
{
  uint8_t chip = 0;
  const addr_t reg = translate(addr, chip);

  if (reg == kSidNotMapped) {
    /* OPL window reads (status port) are not emulated and float high
     * Reported to the backend for a trace to record, others ignore it */
    if (addr >= kFmOplWindowFirst && addr <= kFmOplWindowLast &&
        backend_ != nullptr) {
      io_addr_ = addr;
      io_chip_ = static_cast<uint8_t>(kMaxSids + 1);
      backend_->note_read(static_cast<addr_t>(kFmOplParkBase + (addr & 0x1f)), 0xff,
        static_cast<uint16_t>(bus_.cycles() - last_event_));
    }
    return 0xff;
  }

  io_addr_ = addr;
  io_chip_ = chip;
  ++reads_;

  /* Only voice three's oscillator/envelope registers are readable.
   * with real_reads the value comes from the chip onboard USBSID-Pico
   * and when output is audio the value comes from ReSIDfp.
   * otherwise three is emulated and $d41b/$d41c (timer, random source)
   * still update like they should. */
  const reg_t local = static_cast<reg_t>(addr & 0x1f);

  if ((config_.real_reads || backend_->read_v3_backend()) && chip >= 1 && chip <= kMaxSids) {
    const data_t value = backend_->read(reg, cycles_since_last_event());
    US_LOG_IF(sid_r, "[RR SID%u] $%04x $%03x:%02x\n", chip, addr, reg, value);
    return value;
  }

  /* --solo: a chip with no solo'd voice never reaches the backend (see io_write())
   * Its reads are noise when isolating a SPEC with -srw/-sidr, suppressed like writes
   * Chip granular, a tune polls $d41b/$d41c whatever voice it cares about */
  const bool solo_read_dropped = config_.solo_active && chip >= 1 &&
    chip <= kMaxSids && config_.voice_solo[chip - 1] == 0;

  if (backend_->read_v3_virtual() && (chip >= 1 && chip <= kMaxSids)) {
    if (local == kSidRegOsc3 || local == kSidRegEnv3) {
      const data_t value = voice3_[chip - 1].read(local, bus_.cycles());
      if (!solo_read_dropped) {
        US_LOG_IF(sid_rw, "[R SID%u] $%04x $%03x:%02x\n", chip, addr, reg, value);
        US_LOG_IF(sid_r, "[R SID%u] $%04x $%03x:%02x\n", chip, addr, reg, value);
      }
      return value;
    }
  }
  /* Mirror index in the tune chip layout, see io_write() */
  const addr_t mirror = (chip >= 1 && chip <= kMaxSids)
    ? static_cast<addr_t>(((chip - 1) * 0x20) + local) : reg;
  if (!solo_read_dropped) {
    US_LOG_IF(sid_rw, "[R SID%u] $%04x $%03x:%02x (mirror)\n", chip, addr, reg,
              regs_[mirror & (kRegsSize - 1)]);
    US_LOG_IF(sid_r, "[R SID%u] $%04x $%03x:%02x (mirror)\n", chip, addr, reg,
              regs_[mirror & (kRegsSize - 1)]);
  }

  /* Everything else floats on real hardware, hand back the mirror */
  return regs_[mirror & (kRegsSize - 1)];
}

void Mos6581_8580::vic_frame_ended(void)
{
  /* Software SID render_idle: push outstanding time at the frame boundary
   * with the same kMaxDelta chunking as an access
   * Keeps rendering silence for a tune that writes nothing for many frames
   * Hardware skips this, deltas are the clock */
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

  /* A flush is a transport event, the delta base is left alone
   * The first write of the following frame carries the whole gap since the last write */
  backend_->flush();
}

} /* namespace usbsid */
