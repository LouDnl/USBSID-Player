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
  for (uint8_t i = 0; i < 0x80; i++) regs_[i] = 0;
  last_event_ = bus_.cycles();
  for (uint8_t i = 0; i < 4; i++) {
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
 * A single SID is decoded by the PLA across the whole of $d400-$d7ff, so the
 * chip answers all thirty two mirrors of itself. That is not a convenience:
 * $d7ff really is register $1f on a real machine, which is why the test
 * suites use it as a scratch address.
 *
 * With more than one chip configured the bases claim their own ranges, and
 * the spare addresses tunes like to write to are folded onto the first chip,
 * which is what the existing player does.
 */
bool Mos6581_8580::custom_address(addr_t addr) const
{
  if (addr >= kSidPageFirst && addr <= kSidPageLast) return true;
  const addr_t page = static_cast<addr_t>(addr & 0xfff0);
  return (page >= 0xd420 && page <= 0xd45f) ||
         (page >= 0xd5c0 && page <= 0xd5df);
}

data_t Mos6581_8580::translate(addr_t addr, uint8_t & chip) const
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
      return static_cast<data_t>(((chip - 1) * 0x20) + (addr & 0x1f));
    }
    chip = 5; /* nothing claims it, park it out of the way */
    return static_cast<data_t>(0x80 + (addr & 0x1f));
  }

  const uint8_t count = (config_.count == 0) ? 1
                      : (config_.count > 4)  ? 4 : config_.count;

  for (uint8_t n = 0; n < count; n++) {
    const addr_t base = config_.base[n];
    if (base == 0) continue;
    if (addr >= base && addr < static_cast<addr_t>(base + 0x20)) {
      chip = static_cast<uint8_t>(n + 1);
      const data_t reg = static_cast<data_t>((n * 0x20) + (addr & 0x1f));
      /* only the first chip can be pushed into the other socket */
      return (n == 0) ? static_cast<data_t>(socket_offset + (addr & 0x1f)) : reg;
    }
  }

  /* anything else inside the SID page belongs to the first chip */
  if (custom_address(addr)) {
    chip = 1;
    return static_cast<data_t>(socket_offset + (addr & 0x1f));
  }

  return kSidNotMapped;
}

/**
 * @brief Cycles since the previous SID event, as the hardware wants them.
 *
 * Two adjustments to the raw gap:
 *
 * Anything longer than sixteen bits is handed to the backend as a wait first,
 * so the remainder always fits alongside the write.
 *
 * And one cycle is taken off, because performing the access on USBSID-Pico
 * costs a cycle of its own. Sending the full gap makes every write one cycle
 * late, and the error accumulates across a frame.
 *
 * It is called only for accesses that actually reach the hardware. An access
 * that stops here, a read served from the register mirror or a write to an
 * address no chip claims, must leave the base where it is: consuming the gap
 * without sending it anywhere loses that time for good. Tunes read $d41b and
 * $d41c constantly for their random numbers, and on the device, where these
 * deltas are the only clock there is, that leak alone ran playback fast.
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
inline uint8_t control_reg_voice(data_t local)
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
inline uint8_t sr_reg_voice(data_t local)
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
 * Two bits' worth of change, on two registers of a muted voice:
 *
 *   control ($04/$0b/$12)  the gate bit is forced to 0
 *   sustain ($06/$0d/$14)  the sustain nibble is forced to 0
 *
 * The gate alone is not enough. Clearing it starts the release phase, and
 * release can be up to 24 seconds, so a voice muted mid note stays audible for
 * as long as the tune happens to have asked for. Holding sustain at 0 as well
 * takes the envelope's floor away, so the voice goes quiet and stays quiet
 * however the tune re-gates it.
 *
 * Everything else is passed through: the release nibble, the waveform, ring
 * modulation and sync, and every other register. So a muted voice keeps
 * following the tune and comes back exactly where the tune has got to.
 */
data_t Mos6581_8580::mask_for_output(data_t reg, data_t value) const
{
  const uint8_t chip = static_cast<uint8_t>((reg >> 5) & 0x03);
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
  if (chip < 1 || chip > 4 || voice < 1 || voice > 3) return;
  const uint8_t bit = static_cast<uint8_t>(1u << (voice - 1));
  uint8_t & mask = config_.voice_mute[chip - 1];
  const bool was = (mask & bit) != 0;
  if (was == muted) return;
  mask = static_cast<uint8_t>(muted ? (mask | bit) : (mask & ~bit));

  /* Setting the mask does not silence a note that is already sounding: the chip
   * holds its gate until something writes to that register, and a long sustain
   * may not be written again for seconds. So push both registers now, and on the
   * way back too, so the voice returns without waiting for the tune.
   *
   * Sustain goes first, both directions, and the order is not arbitrary.
   *
   * Muting: dropping sustain to 0 while the voice is still gated makes the
   * envelope fall to zero at the decay rate, and clearing the gate then releases
   * from zero. The other order releases from whatever level the note was holding,
   * at the release rate, which is the long tail this is meant to avoid.
   *
   * Unmuting: putting the tune's sustain back before the gate goes high means a
   * restarted note has the right level from its first cycle rather than climbing
   * to it after the fact.
   *
   * `regs_[]` is the save slot for the sustain nibble, and no separate copy is
   * needed: the mirror is written before the mask is applied, so it always holds
   * what the tune last wrote, whether that write happened before the mute or
   * during it. */
  static const data_t kSustainRelease[3] = { 0x06, 0x0d, 0x14 };
  static const data_t kControl[3]        = { 0x04, 0x0b, 0x12 };
  if (backend_ != nullptr) {
    const data_t base = static_cast<data_t>((chip - 1) * 0x20);
    const data_t order[2] = {
      static_cast<data_t>(base + kSustainRelease[voice - 1]),
      static_cast<data_t>(base + kControl[voice - 1]),
    };
    for (const data_t reg : order) {
      backend_->write(reg, mask_for_output(reg, regs_[reg & 0x7f]),
                      cycles_since_last_event());
      ++writes_;
    }
  }
}

void Mos6581_8580::io_write(addr_t addr, data_t value)
{
  uint8_t chip = 0;
  const data_t reg = translate(addr, chip);

  if (reg == kSidNotMapped) return;

  regs_[reg & 0x7f] = value;

  /* The FM/OPL addresses when no SID claims them, chip 5 out of translate().
   *
   * On a board that is the end of it: nothing can play them, so nothing is sent.
   * Over ASID it is not, because ASID carries FM itself, in its own SysEx, and a
   * receiver that has an OPL can play it whatever the board does. So they go to
   * the backend as $80 for $df40 and $90 for $df50, out of the 0..$7f range any
   * SID register lives in, and a backend that cannot use them drops them.
   *
   * That is why the three hardware transports guard on `reg >= 0x80`: they never
   * used to be handed these and sending them to a board would be junk. */
  if (chip == 5) {
    backend_->write(reg, value, cycles_since_last_event());
    ++writes_;
    return;
  }

  if (chip >= 1 && chip <= 4) {
    US_LOG_IF(sid_rw, "[W SID%u] $%04x $%02x:%02x [C]%5u\n", chip, addr, reg,
              value, static_cast<unsigned>(bus_.cycles() - last_event_));
    /* Voice three follows along, so $d41b and $d41c can answer. It is fed the
     * register within the chip and the cycle the write happens on, which is
     * what lets it be caught up lazily and still come out exact. */
    voice3_[chip - 1].write(static_cast<reg_t>(addr & 0x1f), value,
                            bus_.cycles());
    /* The only place a mute is applied: on the way out, after the mirror and
     * voice three have both seen what the tune actually wrote. */
    backend_->write(reg, mask_for_output(reg, value), cycles_since_last_event());
    // backend_->write(reg, value, cycles_since_last_event());
    ++writes_;
  }
}

data_t Mos6581_8580::io_read(addr_t addr)
{
  uint8_t chip = 0;
  const data_t reg = translate(addr, chip);

  if (reg == kSidNotMapped) return 0xff;

  ++reads_;

  /* Only two registers of a SID can be read: voice three's oscillator and its
   * envelope. With `real_reads` the answer comes from the chip itself, which
   * is the most faithful thing available and costs a bus turnaround. Without
   * it, voice three is emulated, and that is not a nicety: tunes poll $d41b
   * for a random number, as a timer, and to wait until the oscillator has
   * moved, and one that never moves is a tune that never starts. */
  const reg_t local = static_cast<reg_t>(addr & 0x1f);

  if (config_.real_reads && chip >= 1 && chip <= 4) {
    return backend_->read(reg, cycles_since_last_event());
  }

  if (chip >= 1 && chip <= 4) {
    if (local == kSidRegOsc3 || local == kSidRegEnv3) {
      const data_t value = (local == kSidRegOsc3)
        ? voice3_[chip - 1].osc3(bus_.cycles())
        : voice3_[chip - 1].env3(bus_.cycles());
      US_LOG_IF(sid_rw, "[R SID%u] $%04x $%02x:%02x\n", chip, addr, reg, value);
      return value;
    }
  }
  US_LOG_IF(sid_rw, "[R SID%u] $%04x $%02x:%02x (mirror)\n", chip, addr, reg,
            regs_[reg & 0x7f]);

  /* Everything else floats on real hardware. The mirror is the most useful
   * thing to hand back for it. */
  return regs_[reg & 0x7f];
}

void Mos6581_8580::vic_frame_ended(void)
{
  /* A software SID renders only when it is told that time has passed, and it is
   * told by the gap carried on an access. A tune that writes nothing for a
   * while therefore produces no audio at all for that while, and a player that
   * paces itself by "emulate until the ring has enough samples" reads that as
   * "not enough yet" and emulates flat out. Vicious_SID_2-Greets writes once in
   * its first fifty frames and 47 672 times by frame 150; c64_mp3 writes nothing
   * at all for its first three hundred. Both raced.
   *
   * So for a software SID the outstanding time goes out here, at the frame
   * boundary, in the same kMaxDelta chunks an access would have used. The delta
   * base moves with it, which is safe precisely because the time has been
   * delivered rather than dropped.
   *
   * Hardware keeps the old behaviour, and must: there the deltas are the clock,
   * the board idles when it has no work, and handing it idle time to wait out
   * would be inventing work. See cycles_since_last_event(). */
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

  /* A flush is a transport event, not a timing one, so the delta base is left
   * alone: the first write of the next frame carries the whole gap since the
   * last write, across the frame boundary.
   *
   * It used to be reset here, and that was wrong. Tunes stop writing part way
   * into a frame, so resetting threw away everything between the last write
   * and the end of the frame: around 3700 cycles a frame for a typical tune,
   * which is nearly a fifth of the frame. On the desktop the pacer hides it,
   * because real time is kept by the host and the device simply idles when it
   * runs out of work. On the device there is no pacer at all: the cycle
   * deltas *are* the clock, and playback ran that fifth too fast. */
  backend_->flush();
}

} /* namespace usbsid */
