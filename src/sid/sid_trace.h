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

#include "sid_backend.h"
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
      uint16_t delta;   /* cycles since the previous event */
      data_t reg;
      data_t value;
      char kind;        /* 'w' write, 'r' read, 'f' flush, 'i' wait */
    };

    TraceSidBackend(Event * buffer, size_t capacity)
      : events_(buffer), capacity_(capacity) {}

    void write(data_t reg, data_t value, uint16_t cycles) override
    {
      cycle_ += cycles;
      record('w', reg, value, cycles);
    }
    data_t read(data_t reg, uint16_t cycles) override
    {
      cycle_ += cycles;
      record('r', reg, 0, cycles);
      return 0;
    }
    void wait(uint16_t cycles) override
    {
      cycle_ += cycles;
      record('i', 0, 0, cycles);
    }
    void flush(void) override { record('f', 0, 0, 0); }
    void reset(void) override { count_ = 0; dropped_ = 0; cycle_ = 0; }

    size_t count(void) const { return count_; }
    size_t dropped(void) const { return dropped_; }
    const Event & at(size_t i) const { return events_[i]; }

    /** @brief One line per event, the format the old player logs in. */
    void dump(FILE * out) const
    {
      for (size_t i = 0; i < count_; i++) {
        const Event & e = events_[i];
        switch (e.kind) {
          case 'w':
            fprintf(out, "[W]$%02x:%02x [C]%5u @%llu\n", e.reg, e.value,
                    e.delta, static_cast<unsigned long long>(e.cycle));
            break;
          case 'r':
            fprintf(out, "[R]$%02x    [C]%5u @%llu\n", e.reg, e.delta,
                    static_cast<unsigned long long>(e.cycle));
            break;
          case 'i':
            fprintf(out, "[WAIT]     [C]%5u @%llu\n", e.delta,
                    static_cast<unsigned long long>(e.cycle));
            break;
          default:
            fprintf(out, "[FLUSH]              @%llu\n",
                    static_cast<unsigned long long>(e.cycle));
            break;
        }
      }
    }

  private:
    void record(char kind, data_t reg, data_t value, uint16_t delta)
    {
      if (count_ >= capacity_) { ++dropped_; return; }
      events_[count_++] = Event{ cycle_, delta, reg, value, kind };
    }

    Event * events_;
    size_t capacity_;
    size_t count_ = 0;
    size_t dropped_ = 0;
    uint64_t cycle_ = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_SID_TRACE_H_ */
