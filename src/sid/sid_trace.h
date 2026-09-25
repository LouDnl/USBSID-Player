/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_trace.h
 * A backend that records what was written and when, so a run can be compared
 * against another player or against a previous run of this one. This is the
 * evidence that the emulation is right, so it lives in the source tree and
 * not in the tests.
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
#ifndef _US_SID_SID_TRACE_H_
#define _US_SID_SID_TRACE_H_

#include <cstdio>

#include "mos6581_8580.h"
#include "sid_backend.h"
#include "sidfile.h"
#include "types.h"

namespace usbsid {

/**
 * @brief Records every SID event with the cycle it happened on.
 *
 * The buffer is fixed and supplied by the caller, so this works on the
 * embedded target as well. When it fills up it stops recording and counts
 * what it dropped rather than overwriting history.
 */
class TraceSidBackend final : public SidBackend
{
  public:
    struct Event {
      uint64_t cycle;   /* running total, so two traces line up */
      uint64_t play;    /* bus cycles since the tune started, for play time */
      uint16_t delta;   /* cycles since the previous event */
      addr_t reg;
      addr_t addr;      /* C64 address, 0 when the source did not know it */
      data_t value;
      char kind;        /* 'w' write, 'r' read, 'f' flush, 'i' wait */
      uint8_t chip;     /* 1 based, past kMaxSids for FM/OPL */
    };

    TraceSidBackend(Event * buffer, size_t capacity)
      : events_(buffer), capacity_(capacity) {}

    /**
     * @brief Pass everything on to another backend after recording it.
     *
     * Without this, recording and playing are exclusive: the machine has one
     * backend and the trace was it, so a trace could only ever be taken of a
     * silent run. Chained, the trace sits in front of the board or of the
     * software SID and writes down what goes past on its way there, which is
     * the only way to see what was actually sent while a tune was playing.
     *
     * Recording happens first so that the file is complete even if the thing
     * behind it fails, and the cycle deltas handed on are the ones that came
     * in: nothing here changes what the backend below sees.
     */
    void set_next(SidBackend * next) { next_ = next; }
    SidBackend * next(void) const { return next_; }

    /**
     * @brief Label events with the source's C64 address, chip and play time.
     *
     * Without a source, chip comes from the register block and play time
     * from the summed deltas.
     */
    void set_source(const Mos6581_8580 * source) { source_ = source; }

    /** @brief C64 clock in Hz, converts play cycles to time in dump(). */
    void set_clock_hz(uint32_t hz) { clock_hz_ = (hz != 0) ? hz : 985248u; }

    void write(addr_t reg, data_t value, uint16_t cycles) override
    {
      cycle_ += cycles;
      record('w', reg, value, cycles);
      if (next_ != nullptr) next_->write(reg, value, cycles);
    }
    data_t read(addr_t reg, uint16_t cycles) override
    {
      cycle_ += cycles;
      /* A chained read records what the backend below answered, which is the
       * value the tune went on to act on. Unchained there is nothing to record
       * but the fact of the read, as before. */
      const data_t v = (next_ != nullptr) ? next_->read(reg, cycles) : 0;
      record('r', reg, v, cycles);
      return v;
    }
    void note_read(addr_t reg, data_t value, uint16_t cycles) override
    {
      /* Positioned at the current cycle without consuming the delta: the next
       * event still carries the whole gap, so the running total stays exact. */
      const uint64_t base = cycle_;
      cycle_ += cycles;
      record('r', reg, value, cycles);
      cycle_ = base;
    }
    void wait(uint16_t cycles) override
    {
      cycle_ += cycles;
      record('i', 0, 0, cycles);
      if (next_ != nullptr) next_->wait(cycles);
    }
    void flush(void) override
    {
      record('f', 0, 0, 0);
      if (next_ != nullptr) next_->flush();
    }
    void reset(void) override
    {
      count_ = 0; dropped_ = 0; cycle_ = 0;
      if (next_ != nullptr) next_->reset();
    }

    size_t count(void) const { return count_; }
    size_t dropped(void) const { return dropped_; }
    const Event & at(size_t i) const { return events_[i]; }

    /**
     * @brief One line per event, laid out like -srw plus cycle and play time.
     *
     * [C] is the delta sent to the backend, @ the running cycle total,
     * [T] the play time as MM:SS.mmm.
     */
    void dump(FILE * out) const
    {
      for (size_t i = 0; i < count_; i++) {
        const Event & e = events_[i];
        const unsigned long long ms = static_cast<unsigned long long>(
          e.play * 1000u / clock_hz_);
        char stamp[40];
        snprintf(stamp, sizeof(stamp), "@%llu [T]%02llu:%02llu.%03llu",
                 static_cast<unsigned long long>(e.cycle), ms / 60000u,
                 (ms / 1000u) % 60u, ms % 1000u);

        char label[16];
        if (e.chip >= 1 && e.chip <= kMaxSids) {
          snprintf(label, sizeof(label), "SID%u", e.chip);
        } else {
          snprintf(label, sizeof(label), "FM");
        }
        char addr[8];
        if (e.addr != 0) {
          snprintf(addr, sizeof(addr), "$%04x", e.addr);
        } else {
          snprintf(addr, sizeof(addr), "$----");
        }

        switch (e.kind) {
          case 'w':
            fprintf(out, "[W %s] %s $%03x:%02x [C]%5u %s\n", label, addr,
                    e.reg, e.value, e.delta, stamp);
            break;
          case 'r':
            fprintf(out, "[R %s] %s $%03x:%02x [C]%5u %s\n", label, addr,
                    e.reg, e.value, e.delta, stamp);
            break;
          case 'i':
            fprintf(out, "[WAIT]                       [C]%5u %s\n", e.delta,
                    stamp);
            break;
          default:
            fprintf(out, "[FLUSH]                             %s\n", stamp);
            break;
        }
      }
    }

  private:
    void record(char kind, addr_t reg, data_t value, uint16_t delta)
    {
      if (count_ >= capacity_) { ++dropped_; return; }
      Event & e = events_[count_++];
      e.cycle = cycle_;
      e.delta = delta;
      e.reg = reg;
      e.value = value;
      e.kind = kind;
      e.play = (source_ != nullptr) ? source_->play_cycles() : cycle_;
      e.addr = 0;
      e.chip = static_cast<uint8_t>((reg >> 5) + 1);
      if (source_ != nullptr && source_->io_chip() != 0) {
        e.addr = source_->io_addr();
        e.chip = source_->io_chip();
      }
    }

    Event * events_;
    SidBackend * next_ = nullptr;
    const Mos6581_8580 * source_ = nullptr;
    uint32_t clock_hz_ = 985248;
    size_t capacity_;
    size_t count_ = 0;
    size_t dropped_ = 0;
    uint64_t cycle_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_SID_TRACE_H_ */
