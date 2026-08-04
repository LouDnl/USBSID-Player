/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6526.h
 * Cycle exact MOS 6526 CIA.
 *
 * The timer is the state machine used by VICE (src/core/ciatimer.h, GPLv2,
 * Andre Fachat), which is a refinement of the delay pipeline in
 * assets/cia6526_example that PROJECT.md names as the reference. Both work by
 * propagating events through delay stages once per clock; the difference is
 * that the load and count stages have their own propagation rules instead of
 * sharing one shift register, which is what lets a timer reload from an
 * underflow that lands on the same clock as the stop. The shift register
 * cannot express that case, and the Lorenz cia1ta/cia1tb/cia2ta/cia2tb tests
 * fail on it.
 *
 * Everything else, the interrupt delay, the PB6/PB7 output modes, TOD and the
 * serial register, follows the assets/cia6526_example model.
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
#ifndef _US_CIA_MOS6526_H_
#define _US_CIA_MOS6526_H_

#include "core/bus.h"
#include "io_device.h"
#include "types.h"

namespace usbsid {

/* Which line this CIA pulls when it raises an interrupt. CIA1 is wired to
 * IRQ, CIA2 to NMI. */
enum class CiaLine : uint8_t { Irq, Nmi };

/**
 * @brief Told when a CIA port output changes.
 *
 * CIA2 port A carries the VIC bank, so somebody has to hear about it. This is
 * a callback and not a poll because port writes are rare and the per cycle
 * path must stay clean.
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
     * @brief Run up to the given tick count, in as few steps as it can.
     *
     * The bus does not clock a CIA every cycle. It asks how long the chip can
     * be left alone and then leaves it alone, which is most of the time: a
     * timer counting down from a thousand does nothing but subtract for nine
     * hundred and ninety nine of them. Anything that looks at the chip in the
     * meantime, a read, a write or an input changing, catches it up first, so
     * what it sees is what it would have seen if every cycle had been walked.
     */
    void catch_up(cycle_t target_ticks) US_RAM_ATTR;

    /** @brief Clocks the chip can be left alone for, at least one. */
    uint32_t cycles_to_event(void) const US_RAM_ATTR;

    /**
     * @brief Apply a run of timer A underflows in one go, or do nothing.
     *
     * The one shape `cycles_to_event()` cannot help with: timer A free running
     * on phi2 with a tiny latch as a prescaler, and timer B counting its
     * underflows. The chip genuinely does something every few cycles, so
     * refusing to skip is correct, and `prg/Musik_Run_Stop.prg` (latch of two,
     * an underflow every three cycles) walked 97% of its cycles for it.
     *
     * Nothing in that run is visible from outside until timer B underflows:
     * timer A's own interrupt is masked, it drives no port and no shift
     * register, and timer B only counts. So the whole run can be applied as
     * arithmetic and the chip left standing one underflow short of timer B's,
     * which is where the walking has to start again.
     *
     * Returns the number of clocks applied, or zero when the shape does not
     * hold, which is every other tune.
     */
    uint32_t cascade_skip(cycle_t behind) US_RAM_ATTR;

  private:
    /** @brief Whether timer A is prescaling timer B and nothing else. */
    bool cascade_shape(void) const US_RAM_ATTR;
    /** @brief Clocks the cascade can be left alone for, 0 if not in it. */
    uint32_t cascade_quiet(void) const US_RAM_ATTR;
    /** @brief Clocks the plain counter skip is good for, 0 if it must walk. */
    uint32_t generic_quiet(void) const US_RAM_ATTR;
    /** @brief Trim a skip so it does not step over a time of day tick. */
    uint32_t tod_bound(uint32_t quiet) const US_RAM_ATTR;

  public:

    /** @brief How many ticks it has actually run. */
    cycle_t ticks(void) const { return ticks_; }

    /* how many cycles were walked rather than skipped, for tuning */
    cycle_t real_ticks(void) const { return real_ticks_; }

    void reset(void) override;

    data_t io_read(addr_t addr) override US_RAM_ATTR;
    void io_write(addr_t addr, data_t value) override US_RAM_ATTR;

    /* Direct register access that does not touch the side effects a real read
     * would have. For debugging and for tests. */
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
    /* The keyboard matrix, only meaningful on CIA1. Rows are driven from
     * port A, columns are read back on port B, both active low. */
    void set_key(uint8_t row, uint8_t col, bool pressed);
    void clear_keys(void);
    bool any_key_pressed(void) const { return keys_pressed_; }

    /* TOD is clocked from the mains, so it needs to know how long a cycle is.
     * The video model sets this at step 2.6. */
    void set_clock_hz(uint32_t hz) { clock_hz_ = hz; }

    /* State, for tests and for the status logging. These catch the chip up
     * first: it is behind by design, and a stale counter is a wrong answer. */
    uint16_t counter_a(void) { catch_up_now(); return ta_.counter; }
    uint16_t counter_b(void) { catch_up_now(); return tb_.counter; }
    uint16_t latch_a(void) const { return ta_.latch; }
    uint16_t latch_b(void) const { return tb_.latch; }
    data_t icr(void) { catch_up_now(); return icr_; }
    data_t imr(void) const { return imr_; }
    bool int_asserted(void) { catch_up_now(); return int_low_; }

  public:
    /**
     * @brief One timer, as a state machine.
     *
     * The bit names are VICE's. Start, one shot and the clock source come
     * from the control register; the rest are propagation stages. Force load
     * and step are strobes: they survive exactly one transition.
     */
    /* "nothing is going to happen for a very long time" */
    static constexpr uint32_t kForever = 0x00ffffffu;

    struct Timer {
      enum : uint16_t {
        kCrStart   = 0x0001, /* control register bit 0 */
        kCount2    = 0x0002,
        kStep      = 0x0004, /* one count from CNT or from timer A */
        kCrOneShot = 0x0008, /* control register bit 3 */
        kCrFLoad   = 0x0010, /* control register bit 4, a strobe */
        kPhi2In    = 0x0020, /* counting the system clock */
        kCount3    = 0x0040,
        kLoad1     = 0x0080,
        kOneShot0  = 0x0100,
        kLoad      = 0x0200,
        kOut       = 0x0400, /* the underflow output, drives PB6/PB7 */
        kCount     = 0x0800,
        kOneShot   = 0x1000,
        kCrMask    = 0x0039, /* start, one shot, force load, clock source */
      };

      uint16_t state = 0;
      uint16_t latch = 0xffff;
      uint16_t counter = 0xffff;

      /* The state transition. Written out rather than tabulated: it is eight
       * bit tests, which beats a 32 KB lookup table on the RP2350. */
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

      /* One clock. Returns true when the timer underflowed. */
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

      /* Control register write. Bit 5 of the register means "count CNT", so
       * the internal phi2 bit is its inverse. */
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
        /* a stopped timer takes the new latch straight away */
        if ((state & kLoad) || !(state & kCrStart)) counter = latch;
      }
      /* One count from CNT or from a timer A underflow */
      US_ALWAYS_INLINE void step_once(void)
      {
        if (state & kCrStart) state |= kStep;
      }
      /**
       * @brief How many clocks this timer can be left alone for.
       *
       * Zero means "not a moment": something is in flight in the pipeline and
       * every clock has to be walked. Otherwise it is the number of clocks
       * until the timer does something visible, and the clocks before that one
       * are nothing but a subtraction from the counter.
       *
       * The counting state is a fixed point of next(): start, phi2 in, and the
       * three count stages, and it stays that way until the counter reaches
       * zero. That is what makes the skip exact rather than approximate.
       */
      US_ALWAYS_INLINE uint32_t quiet_clocks(void) const
      {
        /* A settled state is one the transition leaves alone. A stopped timer
         * settles into one and stays there for ever, one shot bits and all:
         * those say what happens at the *next* underflow, and a timer that is
         * not counting will not have one. */
        if (next(state) != state) return 0;
        if ((state & kCount3) == 0) return kForever;

        /* Counting. The stages that change something part way through have to
         * be walked; a plain phi2 countdown does not. The underflow, and with
         * it any one shot stop, lands when the counter reaches zero. */
        if ((state & (kStep | kCrFLoad | kLoad1 | kLoad | kOut)) != 0) return 0;
        if ((state & (kCrStart | kPhi2In)) != (kCrStart | kPhi2In)) return 0;
        return (counter == 0) ? 1u : counter;
      }

      /** @brief Take `n` clocks off a timer that quiet_clocks() allowed. */
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

    /* interrupt sources in ICR / IMR */
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
    /** @brief Catch up to where the bus is, which is one tick ahead of its
     *  cycle counter while a cycle is being executed. */
    void catch_up_now(void) US_RAM_ATTR;
    /** @brief Tell the bus the schedule changed under it. */
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
    data_t pa_input_ = 0xff;      /* what the outside world drives */
    data_t pb_input_ = 0xff;
    data_t pa_out_ = 0xff, pb_out_ = 0xff; /* last reported outputs */

    /* timers */
    Timer ta_;
    Timer tb_;
    /* the interrupt line follows one clock behind the event */
    uint8_t int_delay_ = 0;
    /* PB6 and PB7 pulse high for exactly one clock */
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

    /* time of day, in BCD: 10ths, seconds, minutes, hours */
    data_t tod_[4] = { 0, 0, 0, 0x01 };
    data_t tod_latched_[4] = { 0, 0, 0, 0x01 };
    data_t tod_alarm_[4] = { 0, 0, 0, 0 };
    bool tod_halted_ = true;   /* stopped until the hours are written */
    bool tod_latch_ = false;   /* reading the hours freezes the time */
    bool tod_alarm_write_ = false; /* CRB bit 7 selects alarm instead of clock */
    uint32_t tod_cycles_ = 0;
    uint32_t clock_hz_ = 985248; /* PAL, changed by the video model */

    /* keyboard matrix, active low, one bit per column and row */
    data_t keyboard_[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    /* nothing held is the usual case, and the port reads are on the hot path */
    bool keys_pressed_ = false;

    /* how many clocks this chip has actually run, which is behind the bus
     * whenever it has been left alone */
    cycle_t ticks_ = 0;
    cycle_t real_ticks_ = 0;
};

} /* namespace usbsid */

#endif /* _US_CIA_MOS6526_H_ */
