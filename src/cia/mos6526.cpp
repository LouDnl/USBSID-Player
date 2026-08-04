/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6526.cpp
 * The timer state machine follows VICE (src/core/ciatimer.h, GPLv2), the rest
 * follows the delay pipeline model in assets/cia6526_example, with the TOD and
 * the serial register filled in. See mos6526.h for why the timer is the VICE
 * shape and not the shift register.
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

#include "mos6526.h"
#include "util/profile.h"

#include "util/logging.h"

namespace usbsid {

Mos6526::Mos6526(Bus & bus, CiaLine line)
  : bus_(bus), line_(line)
{
  reset();
}

void Mos6526::reset(void)
{
  ticks_ = bus_.cycles();
  pra_ = prb_ = 0;
  ddra_ = ddrb_ = 0;
  pa_input_ = pb_input_ = 0xff;
  pa_out_ = pb_out_ = 0xff;

  ta_.reset();
  tb_.reset();
  int_delay_ = 0;
  pb6_pulse_ = pb7_pulse_ = 0;
  cra_ = crb_ = 0;
  icr_ = imr_ = 0;
  pb67_timer_mode_ = pb67_timer_out_ = pb67_toggle_ = 0;

  cnt_ = cnt_prev_ = true;
  flag_ = true;
  sp_input_ = true;

  sdr_ = sdr_shift_ = 0;
  sdr_bits_ = 0;
  sdr_pending_ = false;

  tod_[0] = tod_[1] = tod_[2] = 0;
  tod_[3] = 0x01;
  tod_latched_[0] = tod_latched_[1] = tod_latched_[2] = 0;
  tod_latched_[3] = 0x01;
  tod_alarm_[0] = tod_alarm_[1] = tod_alarm_[2] = tod_alarm_[3] = 0;
  tod_halted_ = true;
  tod_latch_ = false;
  tod_alarm_write_ = false;
  tod_cycles_ = 0;

  clear_keys();

  set_int_line(false);
  update_ports();
}

void Mos6526::set_int_line(bool asserted)
{
  int_low_ = asserted;
  if (line_ == CiaLine::Irq) {
    bus_.set_irq(IrqSource::Cia1, asserted);
  } else {
    bus_.set_nmi(NmiSource::Cia2, asserted);
  }
}

/* ------------------------------------------------------------------------ *
 * ports
 * ------------------------------------------------------------------------ */

data_t Mos6526::port_a(void) const
{
  data_t value = static_cast<data_t>((pra_ | ~ddra_) & pa_input_);

  /* The matrix reads both ways round. Port A drives the rows and port B reads
   * the columns, which is how the KERNAL scans, but the connection is just a
   * switch: driving a column low from port B pulls every row that has a key
   * held on that column low as well. Programs that read the joystick on port
   * A see this, and so does anything checking for a key without scanning. */
  if (keys_pressed_) {
    const data_t cols = static_cast<data_t>((prb_ | ~ddrb_) & pb_input_);
    for (uint8_t r = 0; r < 8; r++) {
      /* keyboard_ is active low: a zero bit is a key held down */
      if ((static_cast<data_t>(~keyboard_[r]) & ~cols) != 0) {
        value = static_cast<data_t>(value & ~(1u << r));
      }
    }
  }
  return value;
}

data_t Mos6526::port_b(void) const
{
  data_t value = static_cast<data_t>((prb_ | ~ddrb_) & pb_input_);

  /* the keyboard matrix: every row driven low pulls its columns low */
  if (keys_pressed_) {
    const data_t rows = static_cast<data_t>((pra_ | ~ddra_) & pa_input_);
    for (uint8_t r = 0; r < 8; r++) {
      if ((rows & (1u << r)) == 0) value &= keyboard_[r];
    }
  }

  /* PB6 and PB7 belong to the timers when the timer output modes are on */
  value = static_cast<data_t>((value & ~pb67_timer_mode_) |
                              (pb67_timer_out_ & pb67_timer_mode_));
  return value;
}

void Mos6526::update_ports(void)
{
  const data_t a = static_cast<data_t>(pra_ | ~ddra_);
  const data_t b = static_cast<data_t>(
    ((prb_ | ~ddrb_) & ~pb67_timer_mode_) |
    (pb67_timer_out_ & pb67_timer_mode_));

  if (a != pa_out_) {
    pa_out_ = a;
    if (observer_ != nullptr) observer_->cia_port_changed(0, a);
  }
  if (b != pb_out_) {
    pb_out_ = b;
    if (observer_ != nullptr) observer_->cia_port_changed(1, b);
  }
}

void Mos6526::set_port_a_input(data_t value)
{
  pa_input_ = value;
}

void Mos6526::set_port_b_input(data_t value)
{
  pb_input_ = value;
}

void Mos6526::set_cnt(bool high)
{
  catch_up_now();
  cnt_ = high;
}

void Mos6526::set_flag(bool high)
{
  catch_up_now();
  /* FLAG raises its interrupt on the falling edge */
  if (flag_ && !high) trigger_interrupt(kIntFlag);
  flag_ = high;
}

void Mos6526::set_key(uint8_t row, uint8_t col, bool pressed)
{
  if (row > 7 || col > 7) return;
  catch_up_now();
  if (pressed) {
    keyboard_[row] = static_cast<data_t>(keyboard_[row] & ~(1u << col));
  } else {
    keyboard_[row] = static_cast<data_t>(keyboard_[row] | (1u << col));
  }
  update_keys_pressed();
}

void Mos6526::clear_keys(void)
{
  for (uint8_t r = 0; r < 8; r++) keyboard_[r] = 0xff;
  keys_pressed_ = false;
}

void Mos6526::update_keys_pressed(void)
{
  data_t all = 0xff;
  for (uint8_t r = 0; r < 8; r++) all = static_cast<data_t>(all & keyboard_[r]);
  keys_pressed_ = (all != 0xff);
}

/* ------------------------------------------------------------------------ *
 * interrupts
 * ------------------------------------------------------------------------ */

void Mos6526::trigger_interrupt(data_t source)
{
  icr_ = static_cast<data_t>(icr_ | source);
  if ((imr_ & source) != 0 && !int_low_) {
    int_delay_ |= 0x01; /* the line follows one clock later */
  }
}

/* ------------------------------------------------------------------------ *
 * time of day
 * ------------------------------------------------------------------------ */

data_t Mos6526::bcd_increment(data_t value, data_t limit, bool & carry)
{
  data_t lo = static_cast<data_t>((value & 0x0f) + 1);
  data_t hi = static_cast<data_t>(value >> 4);
  if (lo > 9) { lo = 0; hi = static_cast<data_t>(hi + 1); }
  data_t result = static_cast<data_t>((hi << 4) | lo);
  carry = false;
  if (result >= limit) { result = 0; carry = true; }
  return result;
}

void Mos6526::tod_tick(void)
{
  if (tod_halted_) return;

  bool carry = false;
  tod_[0] = bcd_increment(tod_[0], 0x10, carry); /* tenths, 0..9 */
  if (carry) {
    tod_[1] = bcd_increment(tod_[1], 0x60, carry); /* seconds */
    if (carry) {
      tod_[2] = bcd_increment(tod_[2], 0x60, carry); /* minutes */
      if (carry) {
        /* hours are 1..12 with the AM/PM flag in bit 7 */
        const data_t pm = static_cast<data_t>(tod_[3] & 0x80);
        data_t hours = static_cast<data_t>(tod_[3] & 0x1f);
        bool dummy = false;
        hours = bcd_increment(hours, 0x13, dummy);
        if (hours == 0) hours = 1;
        data_t new_pm = pm;
        if (hours == 0x12) new_pm = static_cast<data_t>(pm ^ 0x80);
        tod_[3] = static_cast<data_t>(hours | new_pm);
      }
    }
  }

  if (tod_[0] == tod_alarm_[0] && tod_[1] == tod_alarm_[1] &&
      tod_[2] == tod_alarm_[2] && tod_[3] == tod_alarm_[3]) {
    trigger_interrupt(kIntAlarm);
  }
}

/* ------------------------------------------------------------------------ *
 * the clock
 * ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ *
 * scheduling
 *
 * The bus asks how long this chip can be ignored and then ignores it. Every
 * way into the chip catches it up first, so nothing can observe the gap.
 * ------------------------------------------------------------------------ */

/**
 * @brief What the plain counter skip is good for, zero if it must walk.
 *
 * Kept separate from cycles_to_event() because the answer it gives is the one
 * `catch_up()`'s `skip()` can act on: a subtraction from each timer's counter.
 * The cascade's answer is not, and letting it reach that path put timer A's
 * counter at zero, a value it never holds while counting.
 */
uint32_t Mos6526::generic_quiet(void) const
{
  /* Anything with a pipeline stage in flight has to be walked cycle by cycle */
  if (int_delay_ != 0 || pb6_pulse_ != 0 || pb7_pulse_ != 0) { US_PROF(cia_pipeline); return 0; }
  if (cnt_ != cnt_prev_) { US_PROF(cia_cnt); return 0; }
  if (sdr_pending_ || sdr_bits_ != 0) { US_PROF(cia_serial); return 0; }

  const uint32_t a = ta_.quiet_clocks();
  if (a == 0) { US_PROF(cia_timer_a); return 0; }
  const uint32_t b = tb_.quiet_clocks();
  if (b == 0) { US_PROF(cia_timer_b); return 0; }
  US_PROF(cia_free);

  return (a < b) ? a : b;
}

uint32_t Mos6526::tod_bound(uint32_t quiet) const
{
  /* the time of day counter is the one thing that ticks regardless */
  const uint32_t hz = ((cra_ & 0x80) != 0) ? 50u : 60u;
  const uint32_t period = clock_hz_ / (hz * 10u);
  const uint32_t tod_left = (period > tod_cycles_) ? (period - tod_cycles_) : 1u;
  return (tod_left < quiet) ? tod_left : quiet;
}

uint32_t Mos6526::cycles_to_event(void) const
{
  /* Anything with a pipeline stage in flight has to be walked cycle by cycle */
  if (int_delay_ != 0 || pb6_pulse_ != 0 || pb7_pulse_ != 0) return 1;
  if (cnt_ != cnt_prev_) return 1;
  if (sdr_pending_ || sdr_bits_ != 0) return 1;

  /* The cascade is the one shape where both timers are permanently busy and
   * yet nothing is visible from outside until timer B underflows. Asked
   * individually they answer "not a moment" for ever, which is true of each of
   * them and useless. Only the bus is told this; see catch_up(). */
  uint32_t quiet = cascade_quiet();
  if (quiet == 0) quiet = generic_quiet();
  if (quiet == 0) return 1;

  quiet = tod_bound(quiet);
  return (quiet == 0) ? 1u : quiet;
}

void Mos6526::skip(uint32_t n)
{
  ta_.skip(n);
  tb_.skip(n);
  tod_cycles_ += n;
  ticks_ += n;
}

/**
 * @brief Timer A prescaling timer B, applied as arithmetic. See the header.
 */
/**
 * @brief Timer A prescaling timer B, applied as arithmetic. See the header.
 *
 * The shape is checked from the control registers only, not from the timer
 * state machines, because with a latch of two there is no clock on which both
 * timers are settled: timer B is propagating a count on two cycles out of
 * every three. So instead of deriving what a period does, one period is
 * *walked* and the result checked against what a repeat would have to look
 * like. If it comes back to the same states with timer B one lower, the period
 * is a clean repeat and the rest can be multiplied out. That is self checking,
 * which matters here because getting it wrong silently is worse than not doing
 * it at all.
 */
/**
 * @brief Timer A free running on phi2 as a prescaler, timer B counting it.
 *
 * Checked from the control registers only, never from the timer state
 * machines: with a latch of two there is no clock on which both timers are
 * settled, because timer B is propagating a count on two cycles out of every
 * three. Whether the *state* repeats is established by walking a period, in
 * cascade_skip().
 */
bool Mos6526::cascade_shape(void) const
{
  typedef Timer T;

  /* Timer A free running on phi2, its underflow raising no interrupt, driving
   * no port pin and shifting no serial bit, so a run of them is invisible. */
  if ((ta_.state & (T::kCrStart | T::kPhi2In)) != (T::kCrStart | T::kPhi2In)) return false;
  if ((ta_.state & (T::kCrOneShot | T::kOneShot0 | T::kOneShot)) != 0) return false;
  if ((imr_ & kIntTimerA) != 0) return false;
  if ((cra_ & 0x42) != 0) return false;

  /* Timer B counting timer A's underflows and nothing else. Mode 10 or 11 in
   * CRB bits 6 and 5, and for 11 the CNT line has to be high. */
  if ((crb_ & 0x40) == 0 || (crb_ & 0x02) != 0) return false;
  if ((crb_ & 0x20) != 0 && !cnt_) return false;
  if ((tb_.state & T::kCrStart) == 0) return false;
  if ((tb_.state & (T::kCrOneShot | T::kOneShot0 | T::kOneShot)) != 0) return false;

  return true;
}

/**
 * @brief How long the cascade can be left alone for.
 *
 * Without this the bus wakes the chip every clock, `catch_up()` is always one
 * clock behind, and cascade_skip() never has room to do anything. Timer B's
 * underflow is the next thing anyone outside can see, so the answer is the run
 * up to one count short of it.
 */
uint32_t Mos6526::cascade_quiet(void) const
{
  const uint32_t period = static_cast<uint32_t>(ta_.latch) + 1u;
  if (period < 3) return 0;
  if (tb_.counter < 3) return 0;
  if (!cascade_shape()) return 0;

  return (static_cast<uint32_t>(tb_.counter) - 2u) * period;
}

uint32_t Mos6526::cascade_skip(cycle_t behind)
{
  const uint32_t period = static_cast<uint32_t>(ta_.latch) + 1u;

  /* At least two periods to be worth it, and a period of one or two is too
   * tight for timer B's count to have propagated inside it. */
  if (period < 3 || behind < 2u * period) return 0;
  if (tb_.counter < 3) return 0;
  if (!cascade_shape()) return 0;

  /* Nothing else in flight */
  if (int_delay_ != 0 || pb6_pulse_ != 0 || pb7_pulse_ != 0) return 0;
  if (cnt_ != cnt_prev_) return 0;
  if (sdr_pending_ || sdr_bits_ != 0) return 0;

  /* Walk one period and see whether it is a repeat. */
  const Timer before_a = ta_;
  const Timer before_b = tb_;
  const cycle_t started = ticks_;
  for (uint32_t i = 0; i < period; i++) tick();
  const uint32_t walked = static_cast<uint32_t>(ticks_ - started);

  const bool repeats =
    ta_.state == before_a.state && ta_.counter == before_a.counter &&
    tb_.state == before_b.state &&
    tb_.counter == static_cast<uint16_t>(before_b.counter - 1u) &&
    int_delay_ == 0 && pb6_pulse_ == 0 && pb7_pulse_ == 0 &&
    !sdr_pending_ && sdr_bits_ == 0 && cnt_ == cnt_prev_;

  if (!repeats) return walked; /* not the shape after all, but time did pass */

  /* Stop one count short of timer B's own underflow: that one is visible. */
  uint64_t reps = static_cast<uint64_t>(tb_.counter) - 1u;

  const uint64_t left = static_cast<uint64_t>(behind) - walked;
  if (reps > left / period) reps = left / period;

  /* The time of day counter ticks regardless, so the run may not step over
   * it. `skip()` defers it the same way. */
  const uint32_t hz = ((cra_ & 0x80) != 0) ? 50u : 60u;
  const uint32_t tod_period = clock_hz_ / (hz * 10u);
  const uint32_t tod_left =
    (tod_period > tod_cycles_) ? (tod_period - tod_cycles_) : 1u;
  if (reps > tod_left / period) reps = tod_left / period;

  if (reps == 0) return walked;

  const uint64_t bulk = reps * period;
  tb_.counter = static_cast<uint16_t>(tb_.counter - reps);
  icr_ = static_cast<data_t>(icr_ | kIntTimerA);
  if ((reps & 1u) != 0) pb67_toggle_ = static_cast<data_t>(pb67_toggle_ ^ 0x40);
  tod_cycles_ += static_cast<uint32_t>(bulk);
  ticks_ += bulk;

  return static_cast<uint32_t>(walked + bulk);
}

void Mos6526::catch_up(cycle_t target_ticks)
{
  while (ticks_ < target_ticks) {
    const cycle_t behind = target_ticks - ticks_;

    if (cascade_skip(behind) != 0) continue;

    /* Deliberately not cycles_to_event(): that one may answer for the cascade,
     * and the skip below is a plain subtraction from both counters, which is
     * meaningless for a timer being counted by another timer rather than by
     * clocks. Letting the cascade's answer reach it put timer A's counter at
     * zero, a value it never holds while it is counting. */
    uint32_t quiet = generic_quiet();
    if (quiet != 0) quiet = tod_bound(quiet);

    if (quiet > 1) {
      const uint64_t bulk =
        (behind < static_cast<cycle_t>(quiet - 1)) ? behind : (quiet - 1);
      if (bulk != 0) {
        skip(static_cast<uint32_t>(bulk));
        continue;
      }
    }
    tick();
  }
}

void Mos6526::catch_up_now(void)
{
  /* While a cycle is being executed the bus counter still names the cycle, so
   * a chip that has kept up has run one tick more than that. */
  catch_up(bus_.catch_up_target());
}

void Mos6526::rescheduled(void)
{
  bus_.cia_rescheduled(this);
}

void Mos6526::tick(void)
{
  ++ticks_;
  ++real_ticks_;
  bool ports_changed = false;

  /* CNT counts on its rising edge. Timer A takes it when CRA bit 5 is set,
   * timer B when its input mode is "CNT". */
  const bool cnt_rising = (cnt_ && !cnt_prev_);
  cnt_prev_ = cnt_;
  if (cnt_rising) {
    if ((cra_ & 0x20) != 0) ta_.step_once();
    if ((crb_ & 0x60) == 0x20) tb_.step_once();
  }

  /* ---- timer A ---- */
  const bool ta_underflow = ta_.tick();
  if (ta_underflow) {
    icr_ = static_cast<data_t>(icr_ | kIntTimerA);
    if ((imr_ & kIntTimerA) != 0 && !int_low_) int_delay_ |= 0x01;

    pb67_toggle_ = static_cast<data_t>(pb67_toggle_ ^ 0x40);

    if ((cra_ & 0x02) != 0) {
      if ((cra_ & 0x04) == 0) {
        /* one clock high pulse on PB6 */
        pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ | 0x40);
        pb6_pulse_ = 2;
      } else {
        pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ ^ 0x40);
      }
      ports_changed = true;
    }

    /* the serial register shifts on every second underflow in output mode */
    if ((cra_ & 0x40) != 0 && (sdr_bits_ != 0 || sdr_pending_)) {
      if (sdr_pending_ && sdr_bits_ == 0) {
        sdr_shift_ = sdr_;
        sdr_bits_ = 16; /* two underflows per bit */
        sdr_pending_ = false;
      }
      if (sdr_bits_ != 0) {
        sdr_bits_--;
        if ((sdr_bits_ & 1) == 0) {
          sdr_shift_ = static_cast<data_t>(sdr_shift_ << 1);
        }
        if (sdr_bits_ == 0) trigger_interrupt(kIntSerial);
      }
    }

  }

  /* ---- timer B ---- */
  const bool tb_underflow = tb_.tick();
  if (tb_underflow) {
    icr_ = static_cast<data_t>(icr_ | kIntTimerB);
    if ((imr_ & kIntTimerB) != 0 && !int_low_) int_delay_ |= 0x01;

    pb67_toggle_ = static_cast<data_t>(pb67_toggle_ ^ 0x80);

    if ((crb_ & 0x02) != 0) {
      if ((crb_ & 0x04) == 0) {
        pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ | 0x80);
        pb7_pulse_ = 2;
      } else {
        pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ ^ 0x80);
      }
      ports_changed = true;
    }
  }

  /* Timer A feeding timer B in the cascade modes. This is applied after both
   * timers have been clocked, so the step lands on the next clock: doing it
   * before timer B ticks makes it count one cycle early, which the Lorenz
   * cia1tab test spots immediately. */
  if (ta_underflow && (crb_ & 0x40) != 0 && ((crb_ & 0x20) == 0 || cnt_)) {
    tb_.step_once();
  }

  /* ---- the PB6/PB7 pulses last exactly one clock ---- */
  if (pb6_pulse_ != 0 && --pb6_pulse_ == 0) {
    pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ & ~0x40);
    ports_changed = true;
  }
  if (pb7_pulse_ != 0 && --pb7_pulse_ == 0) {
    pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ & ~0x80);
    ports_changed = true;
  }

  if (ports_changed) update_ports();

  /* ---- the interrupt line goes low one clock after the event ---- */
  if ((int_delay_ & 0x02) != 0 && !int_low_) {
    set_int_line(true);
  }
  int_delay_ = static_cast<uint8_t>((int_delay_ << 1) & 0x02);

  /* ---- time of day, clocked from the mains ---- */
  {
    /* CRA bit 7 selects a 50 Hz or a 60 Hz supply */
    const uint32_t hz = ((cra_ & 0x80) != 0) ? 50u : 60u;
    const uint32_t period = clock_hz_ / (hz * 10u); /* tenths of a second */
    if (++tod_cycles_ >= period) {
      tod_cycles_ = 0;
      tod_tick();
    }
  }
}

/* ------------------------------------------------------------------------ *
 * registers
 * ------------------------------------------------------------------------ */

data_t Mos6526::peek(uint8_t reg) const
{
  switch (reg & 0x0f) {
    case 0x00: return port_a();
    case 0x01: return port_b();
    case 0x02: return ddra_;
    case 0x03: return ddrb_;
    case 0x04: return static_cast<data_t>(ta_.counter & 0xff);
    case 0x05: return static_cast<data_t>(ta_.counter >> 8);
    case 0x06: return static_cast<data_t>(tb_.counter & 0xff);
    case 0x07: return static_cast<data_t>(tb_.counter >> 8);
    case 0x08: return tod_latch_ ? tod_latched_[0] : tod_[0];
    case 0x09: return tod_latch_ ? tod_latched_[1] : tod_[1];
    case 0x0a: return tod_latch_ ? tod_latched_[2] : tod_[2];
    case 0x0b: return tod_[3];
    case 0x0c: return sdr_;
    case 0x0d: return static_cast<data_t>(icr_ | (int_low_ ? kIntAny : 0));
    /* the start bit reads back from the timer, because a one shot timer
     * clears it by itself */
    case 0x0e: return static_cast<data_t>((cra_ & ~0x01) | (ta_.running() ? 1 : 0));
    default:   return static_cast<data_t>((crb_ & ~0x01) | (tb_.running() ? 1 : 0));
  }
}

data_t Mos6526::io_read(addr_t addr)
{
  catch_up_now();
#if US_LOGGING
  if (US_UNLIKELY(line_ == CiaLine::Irq ? us_log.cia1_rw : us_log.cia2_rw)) {
    const data_t value = peek(static_cast<uint8_t>(addr & 0x0f));
    printf("[R CIA%d] $%04x:%02x\n", (line_ == CiaLine::Irq) ? 1 : 2, addr, value);
  }
#endif
  switch (addr & 0x0f) {
    case 0x00: return port_a();
    case 0x01: return port_b();
    case 0x02: return ddra_;
    case 0x03: return ddrb_;
    case 0x04: return static_cast<data_t>(ta_.counter & 0xff);
    case 0x05: return static_cast<data_t>(ta_.counter >> 8);
    case 0x06: return static_cast<data_t>(tb_.counter & 0xff);
    case 0x07: return static_cast<data_t>(tb_.counter >> 8);

    /* Reading the hours freezes the whole time, reading the tenths lets it
     * run again. Anything else would let the time change between two reads. */
    case 0x08: {
      const data_t v = tod_latch_ ? tod_latched_[0] : tod_[0];
      tod_latch_ = false;
      return v;
    }
    case 0x09: return tod_latch_ ? tod_latched_[1] : tod_[1];
    case 0x0a: return tod_latch_ ? tod_latched_[2] : tod_[2];
    case 0x0b:
      if (!tod_latch_) {
        tod_latch_ = true;
        tod_latched_[0] = tod_[0];
        tod_latched_[1] = tod_[1];
        tod_latched_[2] = tod_[2];
        tod_latched_[3] = tod_[3];
      }
      return tod_latched_[3];

    case 0x0c: return sdr_;

    case 0x0d: {
      /* Reading the ICR returns the events, releases the interrupt line and
       * clears every event, all at once. That single behaviour is the source
       * of most CIA race conditions. */
      data_t value = icr_;
      if (int_low_) {
        value = static_cast<data_t>(value | kIntAny);
        set_int_line(false);
      }
      int_delay_ = 0;
      icr_ = 0;
      return value;
    }

    case 0x0e: return static_cast<data_t>((cra_ & ~0x01) | (ta_.running() ? 1 : 0));
    default:   return static_cast<data_t>((crb_ & ~0x01) | (tb_.running() ? 1 : 0));
  }
}

void Mos6526::io_write(addr_t addr, data_t value)
{
  catch_up_now();
#if US_LOGGING
  if (US_UNLIKELY(line_ == CiaLine::Irq ? us_log.cia1_rw : us_log.cia2_rw)) {
    printf("[W CIA%d] $%04x:%02x\n", (line_ == CiaLine::Irq) ? 1 : 2, addr, value);
  }
#endif
  switch (addr & 0x0f) {
    case 0x00:
      pra_ = value;
      update_ports();
      break;
    case 0x01:
      prb_ = value;
      update_ports();
      break;
    case 0x02:
      ddra_ = value;
      update_ports();
      break;
    case 0x03:
      ddrb_ = value;
      update_ports();
      break;

    case 0x04: ta_.set_latch_lo(value); break;
    case 0x05: ta_.set_latch_hi(value); break;
    case 0x06: tb_.set_latch_lo(value); break;
    case 0x07: tb_.set_latch_hi(value); break;

    /* TOD. CRB bit 7 decides whether a write sets the clock or the alarm.
     * Writing the hours stops the clock, writing the tenths starts it. */
    case 0x08:
      if (tod_alarm_write_) {
        tod_alarm_[0] = static_cast<data_t>(value & 0x0f);
      } else {
        tod_[0] = static_cast<data_t>(value & 0x0f);
        tod_halted_ = false;
        tod_cycles_ = 0;
      }
      break;
    case 0x09:
      if (tod_alarm_write_) tod_alarm_[1] = static_cast<data_t>(value & 0x7f);
      else                  tod_[1] = static_cast<data_t>(value & 0x7f);
      break;
    case 0x0a:
      if (tod_alarm_write_) tod_alarm_[2] = static_cast<data_t>(value & 0x7f);
      else                  tod_[2] = static_cast<data_t>(value & 0x7f);
      break;
    case 0x0b:
      if (tod_alarm_write_) {
        tod_alarm_[3] = static_cast<data_t>(value & 0x9f);
      } else {
        tod_[3] = static_cast<data_t>(value & 0x9f);
        tod_halted_ = true;
      }
      break;

    case 0x0c:
      sdr_ = value;
      if ((cra_ & 0x40) != 0) sdr_pending_ = true;
      break;

    case 0x0d:
      /* bit 7 says whether the other bits are set or cleared */
      if ((value & 0x80) != 0) {
        imr_ = static_cast<data_t>(imr_ | (value & 0x1f));
      } else {
        imr_ = static_cast<data_t>(imr_ & ~(value & 0x1f));
      }
      /* an already pending event now delivers its interrupt */
      if ((imr_ & icr_) != 0 && !int_low_) {
        int_delay_ |= 0x01;
      }
      break;

    case 0x0e: {
      ta_.set_control(value);

      /* starting the timer sets the toggle output high */
      if ((value & 0x01) != 0 && (cra_ & 0x01) == 0) {
        pb67_toggle_ = static_cast<data_t>(pb67_toggle_ | 0x40);
      }

      if ((value & 0x02) == 0) {
        pb67_timer_mode_ = static_cast<data_t>(pb67_timer_mode_ & ~0x40);
      } else {
        pb67_timer_mode_ = static_cast<data_t>(pb67_timer_mode_ | 0x40);
        if ((value & 0x04) == 0) {
          if (pb7_pulse_ == 0) {
            pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ & ~0x40);
          } else {
            pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ | 0x40);
          }
        } else {
          pb67_timer_out_ = static_cast<data_t>(
            (pb67_timer_out_ & ~0x40) | (pb67_toggle_ & 0x40));
        }
      }

      /* Force load is a strobe: it does its work and always reads back as 0 */
      cra_ = static_cast<data_t>(value & ~0x10);
      update_ports();
      break;
    }

    default: {
      /* Timer B has four input modes in bits 5 and 6. Only mode 00 counts
       * phi2; the other three are stepped from CNT or from timer A, so the
       * timer is told to ignore phi2 by setting the CNT bit. */
      tb_.set_control((value & 0x40) != 0
                        ? static_cast<data_t>(value | 0x20)
                        : value);

      if ((value & 0x01) != 0 && (crb_ & 0x01) == 0) {
        pb67_toggle_ = static_cast<data_t>(pb67_toggle_ | 0x80);
      }

      if ((value & 0x02) == 0) {
        pb67_timer_mode_ = static_cast<data_t>(pb67_timer_mode_ & ~0x80);
      } else {
        pb67_timer_mode_ = static_cast<data_t>(pb67_timer_mode_ | 0x80);
        if ((value & 0x04) == 0) {
          if (pb7_pulse_ == 0) {
            pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ & ~0x80);
          } else {
            pb67_timer_out_ = static_cast<data_t>(pb67_timer_out_ | 0x80);
          }
        } else {
          pb67_timer_out_ = static_cast<data_t>(
            (pb67_timer_out_ & ~0x80) | (pb67_toggle_ & 0x80));
        }
      }

      tod_alarm_write_ = (value & 0x80) != 0;
      crb_ = static_cast<data_t>(value & ~0x10);
      update_ports();
      break;
    }
  }

  /* the schedule the bus is holding may no longer be right */
  rescheduled();
}

} /* namespace usbsid */
