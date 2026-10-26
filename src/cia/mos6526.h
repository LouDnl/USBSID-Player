/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * mos6526.h
 * Cycle exact MOS 6526 CIA.
 *
 * Timer is the VICE state machine (src/core/ciatimer.h, GPLv2, Andre Fachat),
 * a refinement of the delay pipeline in cia6526_example
 * Load and count stages have their own propagation rules (reload on an
 * underflow on the same clock as the stop) Interrupt delay, PB6/PB7 output
 * modes, TOD and serial register follow cia6526_example
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

#pragma once
#ifndef _US_CIA_MOS6526_H_
#define _US_CIA_MOS6526_H_

#include "core/bus.h"
#include "io_device.h"
#include "types.h"

namespace usbsid {

/* Line pulled on an interrupt, CIA1 = IRQ, CIA2 = NMI */
enum class CiaLine : uint8_t { Irq, Nmi };

/**
 * @brief Called when a CIA port output changes (CIA2 port A carries the VIC bank)
 */
class CiaPortObserver
{
  public:
    virtual ~CiaPortObserver(void) = default;
    virtual void cia_port_changed(uint8_t port, data_t value) = 0;
};

class Mos6526 final : public ClockedDevice, public IoDevice
{
  public:
    Mos6526(Bus & bus, CiaLine line);
    ~Mos6526(void) override = default;

    void tick(void) override US_RAM_ATTR;

    /**
     * @brief Run up to the given tick count in as few steps as possible
     *
     * Any access (read/write/input change) catches the chip up first
     */
    void catch_up(cycle_t target_ticks) US_RAM_ATTR;

    /** @brief Clocks the chip can be left alone for, at least one */
    uint32_t cycles_to_event(void) const US_RAM_ATTR;

    /**
     * @brief Apply a run of timer A underflows in one go, returns clocks applied or 0
     *
     * Timer A free-running on phi2 as prescaler for timer B, nothing is visible until timer B underflows
     */
    uint32_t cascade_skip(cycle_t behind) US_RAM_ATTR;

  private:
    /** @brief True when timer A prescales timer B and nothing else */
    bool cascade_shape(void) const US_RAM_ATTR;
    /** @brief Clocks the cascade can be left alone for, 0 if not in it */
    uint32_t cascade_quiet(void) const US_RAM_ATTR;
    /** @brief Clocks the plain counter skip is good for, 0 if it must walk */
    uint32_t generic_quiet(void) const US_RAM_ATTR;
    /** @brief Trim a skip to stay before a time of day tick */
    uint32_t tod_bound(uint32_t quiet) const US_RAM_ATTR;

  public:

    /** @brief Ticks actually run */
    cycle_t ticks(void) const { return ticks_; }

    /* Cycles walked, for tuning */
    cycle_t real_ticks(void) const { return real_ticks_; }

    void reset(void) override;

    data_t io_read(addr_t addr) override US_RAM_ATTR;
    void io_write(addr_t addr, data_t value) override US_RAM_ATTR;

    /* Register access without read side effects */
    data_t peek(uint8_t reg) const US_RAM_ATTR;
    void set_port_observer(CiaPortObserver * obs) { observer_ = obs; }

    /* External inputs */
    void set_port_a_input(data_t value);
    void set_port_b_input(data_t value);
    void set_cnt(bool high);
    void set_flag(bool high);     /* interrupt on the falling edge */
    void set_sp(bool high) { sp_input_ = high; }

    /* Current output levels, after direction and the PB6/PB7 timer modes */
    data_t port_a(void) const US_RAM_ATTR;
    data_t port_b(void) const US_RAM_ATTR;
    /* Keyboard matrix (CIA1 only), rows on port A, columns on port B, both active low */
    void set_key(uint8_t row, uint8_t col, bool pressed);
    void clear_keys(void);
    bool any_key_pressed(void) const { return keys_pressed_; }

    /* TOD is clocked from the mains, needs the cycle length, set by the video model */
    void set_clock_hz(uint32_t hz) { clock_hz_ = hz; }

    /* State, catches the chip up first (it runs behind the bus) */
    uint16_t counter_a(void) { catch_up_now(); return ta_.counter; }
    uint16_t counter_b(void) { catch_up_now(); return tb_.counter; }
    uint16_t latch_a(void) const { return ta_.latch; }
    uint16_t latch_b(void) const { return tb_.latch; }
    data_t icr(void) { catch_up_now(); return icr_; }
    data_t imr(void) const { return imr_; }
    bool int_asserted(void) { catch_up_now(); return int_low_; }

  public:
    /**
     * @brief One timer as a state machine
     *
     * Bit names are from VICE, start, one shot and clock source come from the control register
     * Force load and step are strobes (survive one transition)
     */
    /* Nothing happens for a long time */
    static constexpr uint32_t kForever = 0x00ffffffu;

    struct Timer {
      enum : uint16_t {
        kCrStart   = 0x0001, /* control register bit 0 */
        kCount2    = 0x0002,
        kStep      = 0x0004, /* one count from CNT or from timer A */
        kCrOneShot = 0x0008, /* control register bit 3 */
        kCrFLoad   = 0x0010, /* control register bit 4, a strobe */
        kPhi2In    = 0x0020, /* Counting the system clock */
        kCount3    = 0x0040,
        kLoad1     = 0x0080,
        kOneShot0  = 0x0100,
        kLoad      = 0x0200,
        kOut       = 0x0400, /* Underflow output, drives PB6/PB7 */
        kCount     = 0x0800,
        kOneShot   = 0x1000,
        kCrMask    = 0x0039, /* start, one shot, force load, clock source */
      };

      uint16_t state = 0;
      uint16_t latch = 0xffff;
      uint16_t counter = 0xffff;

      /* State transition, 8 bit tests (a 32 KB lookup table is slower on the RP2350) */
      static US_ALWAYS_INLINE uint16_t next(uint16_t s)
      {
        uint16_t t = static_cast<uint16_t>(s & (kCrStart | kCrOneShot | kPhi2In));
        if ((s & kCrStart) && (s & kPhi2In)) t |= kCount2;
        if ((s & kCount2) || ((s & kStep) && (s & kCrStart))) t |= kCount3;
        if (s & kCount3)    t |= kCount;
        if (s & kCrFLoad)   t |= kLoad1;
        if (s & kLoad1)     t |= kLoad;
        if (s & kCrOneShot) t |= kOneShot0;
        if (s & kOneShot0)  t |= kOneShot;
        return t;
      }

      /* One clock, returns true on underflow */
      US_ALWAYS_INLINE bool tick(void)
      {
        bool underflow = false;
        if (counter != 0 && (state & kCount3) != 0) counter--;
        state = next(state);
        if (counter == 0 && (state & kCount3) != 0) {
          state = static_cast<uint16_t>(state | kLoad | kOut);
          underflow = true;
        }
        if (state & kLoad) {
          counter = latch;
          state = static_cast<uint16_t>(state & ~kCount3);
        }
        if ((state & kOut) && (state & (kOneShot | kOneShot0))) {
          state = static_cast<uint16_t>(state & ~(kCrStart | kCount2));
        }
        return underflow;
      }

      /* Control register write, bit 5 = count CNT, internal phi2 bit is its inverse */
      US_ALWAYS_INLINE void set_control(data_t value)
      {
        state = static_cast<uint16_t>(
          (state & ~kCrMask) | ((value & kCrMask) ^ kPhi2In));
      }
      US_ALWAYS_INLINE void set_latch_lo(data_t value)
      {
        latch = static_cast<uint16_t>((latch & 0xff00) | value);
        if (state & kLoad) counter = static_cast<uint16_t>((counter & 0xff00) | value);
      }
      US_ALWAYS_INLINE void set_latch_hi(data_t value)
      {
        latch = static_cast<uint16_t>((latch & 0x00ff) | (value << 8));
        /* A stopped timer takes the new latch immediately */
        if ((state & kLoad) || !(state & kCrStart)) counter = latch;
      }
      /* One count from CNT or from a timer A underflow */
      US_ALWAYS_INLINE void step_once(void)
      {
        if (state & kCrStart) state |= kStep;
      }
      /**
       * @brief Clocks this timer can be left alone for
       *
       * 0 = something is in the pipeline, walk every clock
       * Else clocks until the timer does something visible, the clocks before are a subtraction from the counter
       * Counting state (start, phi2 in, three count stages) is a fixed point of next() until the counter reaches 0
       */
      US_ALWAYS_INLINE uint32_t quiet_clocks(void) const
      {
        /* Settled state = unchanged by next(), a stopped timer stays settled (one shot bits apply to the following underflow) */
        if (next(state) != state) return 0;
        if ((state & kCount3) == 0) return kForever;

        /* Counting, stages that change something part way must be walked, a plain phi2 countdown does not
         * Underflow and one shot stop land when the counter reaches 0 */
        if ((state & (kStep | kCrFLoad | kLoad1 | kLoad | kOut)) != 0) return 0;
        if ((state & (kCrStart | kPhi2In)) != (kCrStart | kPhi2In)) return 0;
        return (counter == 0) ? 1u : counter;
      }

      /** @brief Take n clocks off a timer that quiet_clocks() allowed */
      US_ALWAYS_INLINE void skip(uint32_t n)
      {
        if ((state & kCount3) != 0) {
          counter = static_cast<uint16_t>(counter - n);
        }
      }

      US_ALWAYS_INLINE bool running(void) const { return (state & kCrStart) != 0; }
      US_ALWAYS_INLINE bool output(void) const { return (state & kOut) != 0; }
      US_ALWAYS_INLINE void reset(void)
      {
        state = 0;
        latch = counter = 0xffff;
      }
    };

  private:

    /* Interrupt sources in ICR / IMR */
    enum : data_t {
      kIntTimerA = 0x01,
      kIntTimerB = 0x02,
      kIntAlarm  = 0x04,
      kIntSerial = 0x08,
      kIntFlag   = 0x10,
      kIntAny    = 0x80,
    };

    void set_int_line(bool asserted) US_RAM_ATTR;
    void update_ports(void) US_RAM_ATTR;
    /** @brief Catch up to the bus (one tick ahead of its cycle counter mid-cycle) */
    void catch_up_now(void) US_RAM_ATTR;
    /** @brief Tell the bus the schedule changed */
    void rescheduled(void) US_RAM_ATTR;
    void skip(uint32_t n) US_RAM_ATTR;
    void update_keys_pressed(void) US_RAM_ATTR;
    void trigger_interrupt(data_t source) US_RAM_ATTR;
    /* TOD */
    void tod_tick(void) US_RAM_ATTR;
    static data_t bcd_increment(data_t value, data_t limit, bool & carry) US_RAM_ATTR;
    Bus & bus_;
    CiaLine line_;
    CiaPortObserver * observer_ = nullptr;

    /* ports */
    data_t pra_ = 0, prb_ = 0;    /* output latches */
    data_t ddra_ = 0, ddrb_ = 0;
    data_t pa_input_ = 0xff;      /* Driven by the outside world */
    data_t pb_input_ = 0xff;
    data_t pa_out_ = 0xff, pb_out_ = 0xff; /* Last reported outputs */

    /* timers */
    Timer ta_;
    Timer tb_;
    /* Interrupt line follows one clock behind the event */
    uint8_t int_delay_ = 0;
    /* PB6 and PB7 pulse high for one clock */
    uint8_t pb6_pulse_ = 0;
    uint8_t pb7_pulse_ = 0;
    data_t cra_ = 0, crb_ = 0;
    data_t icr_ = 0, imr_ = 0;
    data_t pb67_timer_mode_ = 0;
    data_t pb67_timer_out_ = 0;
    data_t pb67_toggle_ = 0;
    bool int_low_ = false;

    /* lines */
    bool cnt_ = true;
    bool cnt_prev_ = true;
    bool flag_ = true;
    bool sp_input_ = true;

    /* serial shift register */
    data_t sdr_ = 0;
    data_t sdr_shift_ = 0;
    uint8_t sdr_bits_ = 0;
    bool sdr_pending_ = false;

    /* Time of day in BCD: 10ths, seconds, minutes, hours */
    data_t tod_[4] = { 0, 0, 0, 0x01 };
    data_t tod_latched_[4] = { 0, 0, 0, 0x01 };
    data_t tod_alarm_[4] = { 0, 0, 0, 0 };
    bool tod_halted_ = true;   /* Stopped until the hours are written */
    bool tod_latch_ = false;   /* Reading the hours freezes the time */
    bool tod_alarm_write_ = false; /* CRB bit 7 selects alarm */
    uint32_t tod_cycles_ = 0;
    uint32_t clock_hz_ = 985248; /* PAL, set by the video model */

    /* Keyboard matrix, active low, one bit per column and row */
    data_t keyboard_[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    /* No key held is the common case (port reads are hot) */
    bool keys_pressed_ = false;

    /* Clocks actually run (behind the bus when left alone) */
    cycle_t ticks_ = 0;
    cycle_t real_ticks_ = 0;
};

} /* namespace usbsid */

#endif /* _US_CIA_MOS6526_H_ */
