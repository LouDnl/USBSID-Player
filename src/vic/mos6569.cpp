/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6569.cpp
 *
 * The cycle numbers follow Christian Bauer's "The MOS 6567/6569 video
 * controller (VIC-II) and its application in the Commodore 64", which is the
 * document every C64 emulator's VIC timing comes from.
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

#include "mos6569.h"

#include "util/logging.h"

namespace usbsid {

namespace {

/* The display window, in raster lines. Bad lines only happen inside it. */
constexpr uint16_t kFirstDisplayLine = 0x30;
constexpr uint16_t kLastDisplayLine  = 0xf7;

/* A bad line pulls BA low three cycles before the first character access and
 * holds it until the last one, so cycles 12 to 54 inclusive. The CPU keeps
 * running through the first three of those on its own credit, which is where
 * the well known "40 cycles" comes from. */
constexpr uint16_t kBadlineBaFirst = 12;
constexpr uint16_t kBadlineBaLast  = 54;

/* Sprite n is fetched in cycles 58+2n and 59+2n, and BA goes low three cycles
 * ahead of that. Both wrap into the next line for the higher numbered
 * sprites. */
constexpr uint16_t kSpriteFirstCycle = 58;

/* Where the two pieces of sprite bookkeeping happen in a line: DMA is switched
 * on for a sprite whose Y matches, and the line counter that eventually
 * switches it off again runs well after the fetches. Named because the
 * scheduler has to know to stop on them. */
constexpr uint16_t kSpriteDmaCycle   = 55;
constexpr uint16_t kSpriteCountCycle = 15;

/* Sprites are 21 lines tall, twice that when vertically expanded */
constexpr uint8_t kSpriteHeight = 21;

} /* namespace */

Mos6569::Mos6569(Bus & bus, VideoModel model)
  : bus_(bus)
{
  set_model(model);
  reset();
}

void Mos6569::set_model(VideoModel model)
{
  model_ = model;
  timing_ = &vic_timing(model);
  rebuild_sprite_ba();   /* the windows wrap on the line length */
}

void Mos6569::reset(void)
{
  ticks_ = bus_.cycles();
  cycle_ = 1;
  raster_ = 0;
  raster_irq_ = 0;
  frames_ = 0;
  badline_ = false;
  den_seen_ = false;
  ba_ = true;

  for (uint8_t i = 0; i < 0x40; i++) regs_[i] = 0;
  /* the KERNAL finds these values on a real machine after reset */
  regs_[kRegControl1] = 0x1b;
  regs_[kRegControl2] = 0xc8;
  regs_[kRegMemPtr]   = 0x14;

  irq_flags_ = 0;
  irq_enable_ = 0;

  for (uint8_t i = 0; i < 8; i++) {
    sprite_dma_[i] = false;
    sprite_line_[i] = 0;
    sprite_expand_toggle_[i] = false;
  }
  rebuild_sprite_ba();

  bus_.set_irq(IrqSource::Vic, false);
  bus_.set_ba(true);
}

/* ------------------------------------------------------------------------ *
 * interrupts
 * ------------------------------------------------------------------------ */

void Mos6569::update_irq_line(void)
{
  bus_.set_irq(IrqSource::Vic, (irq_flags_ & irq_enable_ & 0x0f) != 0);
}

void Mos6569::raise_irq(data_t source)
{
  irq_flags_ = static_cast<data_t>(irq_flags_ | source);
  update_irq_line();
}

/* ------------------------------------------------------------------------ *
 * bad lines and BA
 * ------------------------------------------------------------------------ */

void Mos6569::check_badline(void)
{
  /* A bad line needs the display to have been enabled somewhere in the first
   * line of the display window, the raster inside the window, and the low
   * three raster bits to match the vertical scroll. */
  const data_t yscroll = static_cast<data_t>(regs_[kRegControl1] & 0x07);
  badline_ = den_seen_ &&
             raster_ >= kFirstDisplayLine && raster_ <= kLastDisplayLine &&
             (static_cast<data_t>(raster_ & 0x07) == yscroll);
}

/**
 * @brief Work out the BA low cycles, and the next change from each of them.
 *
 * Called when a sprite's DMA changes, which is at cycle 55 and cycle 15, and
 * nowhere else. Everything that used to derive this per tick now reads it.
 */
void Mos6569::rebuild_sprite_ba(void)
{
  const uint16_t line = timing_->cycles_per_line;

  for (uint16_t c = 0; c <= line + 1; c++) sprite_ba_low_[c] = false;
  sprite_ba_any_ = false;

  for (uint8_t n = 0; n < 8; n++) {
    if (!sprite_dma_[n]) continue;
    sprite_ba_any_ = true;

    /* BA low from three cycles before the first fetch until the last one.
     * The higher numbered sprites wrap into the next line. */
    const uint16_t first = static_cast<uint16_t>(kSpriteFirstCycle + 2 * n - 3);
    const uint16_t last  = static_cast<uint16_t>(kSpriteFirstCycle + 2 * n + 1);

    for (uint16_t c = first; c <= last; c++) {
      uint16_t wrapped = c;
      while (wrapped > line) wrapped = static_cast<uint16_t>(wrapped - line);
      sprite_ba_low_[wrapped] = true;
    }
  }

  /* The edges of the union: every cycle whose BA differs from the cycle
   * before it. Cycle one's predecessor is the last cycle of the previous
   * line, which is the same table, and that is what makes a window that
   * wrapped show up as an edge at the start of the line rather than being
   * missed. Getting that wrong was worth five failing lockstep checks. */
  sprite_ba_nedges_ = 0;
  for (uint16_t c = 1; c <= line; c++) {
    const bool before = (c == 1) ? sprite_ba_low_[line] : sprite_ba_low_[c - 1];
    if (sprite_ba_low_[c] != before &&
        sprite_ba_nedges_ < sizeof(sprite_ba_edges_)) {
      sprite_ba_edges_[sprite_ba_nedges_++] = static_cast<uint8_t>(c);
    }
  }
}

bool Mos6569::sprite_steals_cycle(uint16_t cycle) const
{
  return (cycle < kMaxLineCycles) ? sprite_ba_low_[cycle] : false;
}

void Mos6569::update_ba(void)
{
  bool low = false;

  if (badline_ && cycle_ >= kBadlineBaFirst && cycle_ <= kBadlineBaLast) {
    low = true;
  }
  if (!low && sprite_steals_cycle(cycle_)) {
    low = true;
  }

  if (low == ba_) {
    ba_ = !low;
    bus_.set_ba(ba_);
  }
}

/* ------------------------------------------------------------------------ *
 * the clock
 * ------------------------------------------------------------------------ */

void Mos6569::start_of_line(void)
{
  /* DEN is sampled through the whole of the first display line */
  if (raster_ == kFirstDisplayLine) {
    if ((regs_[kRegControl1] & 0x10) != 0) den_seen_ = true;
  } else if (raster_ == 0) {
    den_seen_ = false;
  }

  check_badline();
}

void Mos6569::end_of_line(void)
{
  raster_ = static_cast<uint16_t>(raster_ + 1);
  if (raster_ >= timing_->lines_per_frame) {
    raster_ = 0;
    ++frames_;
    if (observer_ != nullptr) observer_->vic_frame_ended();
  }
  start_of_line();
}

/* ------------------------------------------------------------------------ *
 * scheduling
 * ------------------------------------------------------------------------ */

uint32_t Mos6569::cycles_to_event(void) const
{
  /* The first display line samples DEN on every cycle of it */
  if (raster_ == kFirstDisplayLine && !den_seen_) return 1;

  /* The line boundary always matters: it moves the raster and, at the end of
   * the frame, tells the SID layer to flush. */
  uint16_t next = timing_->cycles_per_line;

  /* Sprites.
   *
   * This used to walk every cycle of every line as soon as one sprite was
   * enabled, on the grounds that the fetch windows overlap and wrap into the
   * next line. They do, but the cost of that shortcut is not small: a single
   * enabled sprite took the VIC from walking 1.8% of cycles to walking all of
   * them, which is two and a half times the emulation cost and the difference
   * between a program playing and a program dragging on the device. Almost
   * every PRG has a sprite somewhere.
   *
   * What actually happens on a line is bounded. Cycle 55 is where DMA is
   * switched on for a sprite whose Y matches, so a line with any sprite
   * *enabled* has to be visited there. Cycle 15 is where the line counter
   * runs, which only matters if some DMA is already *active*. And an active
   * sprite pulls BA low across five cycles of its own, so the cycles where
   * that starts and stops have to be visited too. Everything between is
   * quiet, and on the 291 lines of 312 where a 21 line sprite is not being
   * fetched at all, the whole line is quiet bar cycle 55. */
  if (regs_[kRegSprEnable] != 0 && kSpriteDmaCycle >= cycle_ &&
      kSpriteDmaCycle < next) {
    next = kSpriteDmaCycle;
  }

  if (sprite_ba_any_) {
    /* the line counter, and the end of DMA, at cycle 15 */
    if (kSpriteCountCycle >= cycle_ && kSpriteCountCycle < next) {
      next = kSpriteCountCycle;
    }

    /* Where BA changes. Ticking every individual window edge, which is what
     * this used to do, meant stopping every two cycles once a few sprites were
     * active: the windows are five cycles wide and two apart, so they merge,
     * and most of those edges are inside the merged run and change nothing.
     * `sprite_ba_edges_` is the union's own edges and nothing else, tested the
     * same way the individual edges were. */
    for (uint8_t i = 0; i < sprite_ba_nedges_; i++) {
      const uint16_t at = sprite_ba_edges_[i];
      if (at >= cycle_ && at < next) next = at;
    }
  }

  /* BA drops for a bad line and comes back after it */
  if (badline_) {
    if (kBadlineBaFirst >= cycle_ && kBadlineBaFirst < next) {
      next = kBadlineBaFirst;
    }
    const uint16_t after = kBadlineBaLast + 1;
    if (after >= cycle_ && after < next) next = after;
  }

  /* The raster compare, at the top of the line it matches */
  const bool compare_here = (raster_ != 0) ? (raster_ == raster_irq_)
                                           : (raster_irq_ == 0);
  if (compare_here) {
    const uint16_t at = (raster_ != 0) ? 1 : 2;
    if (at >= cycle_ && at < next) next = at;
  }

  return static_cast<uint32_t>(next - cycle_ + 1);
}

void Mos6569::skip(uint32_t n)
{
  cycle_ = static_cast<uint16_t>(cycle_ + n);
  ticks_ += n;
}

void Mos6569::catch_up(cycle_t target_ticks)
{
  while (ticks_ < target_ticks) {
    const cycle_t behind = target_ticks - ticks_;
    const uint32_t quiet = cycles_to_event();

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

uint32_t Mos6569::cycles_to_frame_end(void)
{
  catch_up_now();

  const uint32_t line = timing_->cycles_per_line;
  const uint32_t lines_left =
    static_cast<uint32_t>(timing_->lines_per_frame - 1 - raster_);
  /* what is left of this line, then the whole lines after it */
  return (line - cycle_ + 1) + lines_left * line;
}

void Mos6569::catch_up_now(void)
{
  catch_up(bus_.catch_up_target());
}

void Mos6569::tick(void)
{
  ++ticks_;
  ++real_ticks_;
  /* The raster interrupt is raised at the start of the line, except for line
   * zero which is compared one cycle later because the counter wraps there. */
  if (cycle_ == 1 && raster_ != 0) {
    if (raster_ == raster_irq_) raise_irq(kIrqRaster);
  } else if (cycle_ == 2 && raster_ == 0) {
    if (raster_irq_ == 0) raise_irq(kIrqRaster);
  }

  /* DEN keeps being sampled while the first display line runs */
  if (raster_ == kFirstDisplayLine && (regs_[kRegControl1] & 0x10) != 0 &&
      !den_seen_) {
    den_seen_ = true;
    check_badline();
  }

  /* Sprite DMA switches on in cycle 55 for every enabled sprite whose Y
   * coordinate matches this line. */
  if (cycle_ == kSpriteDmaCycle) {
    const data_t enabled = regs_[kRegSprEnable];
    const data_t line    = static_cast<data_t>(raster_ & 0xff);

    bool changed = false;
    for (uint8_t n = 0; n < 8; n++) {
      const data_t sprite_y = regs_[static_cast<uint8_t>(0x01 + 2 * n)];
      if ((enabled & (1u << n)) != 0 && sprite_y == line && !sprite_dma_[n]) {
        sprite_dma_[n] = true;
        sprite_line_[n] = 1; /* this line is the first of the sprite's 21 */
        sprite_expand_toggle_[n] = false;
        changed = true;
      }
    }
    if (changed) rebuild_sprite_ba();
  }

  /* Counting the sprite's lines happens well after the fetch windows, which
   * run from cycle 55 into cycle 10 of the following line for sprite 7. Doing
   * it in cycle 55 would end the DMA before the last line was fetched. */
  if (cycle_ == kSpriteCountCycle) {
    const data_t expand = regs_[kRegSprYExp];
    bool changed = false;
    for (uint8_t n = 0; n < 8; n++) {
      if (!sprite_dma_[n]) continue;
      if ((expand & (1u << n)) != 0) {
        sprite_expand_toggle_[n] = !sprite_expand_toggle_[n];
        if (sprite_expand_toggle_[n]) continue;
      }
      if (++sprite_line_[n] > kSpriteHeight) {
        sprite_dma_[n] = false;
        changed = true;
      }
    }
    if (changed) rebuild_sprite_ba();
  }

  update_ba();

  if (++cycle_ > timing_->cycles_per_line) {
    cycle_ = 1;
    end_of_line();
  }
}

/* ------------------------------------------------------------------------ *
 * registers
 * ------------------------------------------------------------------------ */

/**
 * @brief The raster counter as a read of $d011/$d012 sees it.
 *
 * Every line's increment happens in the line's first cycle, with one
 * exception: the wrap from the last line of the frame to line 0 is a cycle
 * later than that. So for the width of one cycle a program reading the raster
 * at the top of the frame is still shown the last line, and only then zero.
 *
 * This is the same fact that puts the raster IRQ for line 0 in cycle 2 while
 * every other line's is in cycle 1 (see `tick()`): one counter, one late
 * transition, two visible consequences. The internal `raster_` is left
 * alone, because the badline test, the sprite windows and the display window
 * all key off the line the chip is actually drawing.
 *
 * Acid800 `cpu_timing` is what this is measurable with. Its `_waitVCount`
 * polls $d011 every seven cycles for the frame to wrap, so where the sync
 * lands decides which line each of the sixteen counted loops ends on. Its
 * assert 13 counts 2533 cycles from the sync to its own read of $d012, which
 * under NTSC is 38 lines and 63 cycles: two cycles of tolerance out of sixty
 * five, and without this rule the sync lands inside them and the assert reads
 * a line early. The same count under PAL is 40 lines and 13 cycles, tolerant
 * from cycle 1 to cycle 50, which is why PAL never showed it.
 */
uint16_t Mos6569::visible_raster(void) const
{
  if (raster_ == 0 && cycle_ == 1) {
    return static_cast<uint16_t>(timing_->lines_per_frame - 1);
  }
  return raster_;
}

data_t Mos6569::peek(uint8_t reg) const
{
  const uint8_t r = static_cast<uint8_t>(reg & 0x3f);
  switch (r) {
    case kRegControl1:
      return static_cast<data_t>((regs_[kRegControl1] & 0x7f) |
                                 ((visible_raster() & 0x100) ? 0x80 : 0x00));
    case kRegRaster:
      return static_cast<data_t>(visible_raster() & 0xff);
    case kRegIrqFlags:
      return static_cast<data_t>(
        irq_flags_ | 0x70 |
        (((irq_flags_ & irq_enable_ & 0x0f) != 0) ? kIrqAny : 0));
    case kRegIrqEnable:
      return static_cast<data_t>(irq_enable_ | 0xf0);
    default:
      if (r >= 0x2f) return 0xff; /* the unconnected registers read as $ff */
      return regs_[r];
  }
}

data_t Mos6569::io_read(addr_t addr)
{
  catch_up_now();
  const uint8_t r = static_cast<uint8_t>(addr & 0x3f);
  US_LOG_IF(vic_reg_reads, "[R VIC] $%04x:%02x raster %u cycle %u\n", addr,
            peek(r), raster_, cycle_);
  switch (r) {
    case kRegSprSprCol:
    case kRegSprBgCol: {
      /* the collision registers clear themselves on read. Nothing sets them
       * without a rendered picture, so they always read as zero for now. */
      const data_t value = regs_[r];
      regs_[r] = 0;
      return value;
    }
    default:
      return peek(r);
  }
}

void Mos6569::io_write(addr_t addr, data_t value)
{
  catch_up_now();
  const uint8_t r = static_cast<uint8_t>(addr & 0x3f);
  US_LOG_IF(vic_rw, "[W VIC] $%04x:%02x raster %u cycle %u\n", addr, value,
            raster_, cycle_);

  switch (r) {
    case kRegControl1:
      regs_[kRegControl1] = value;
      raster_irq_ = static_cast<uint16_t>((raster_irq_ & 0x00ff) |
                                          ((value & 0x80) ? 0x100 : 0));
      /* enabling the display inside the first display line still counts */
      if (raster_ == kFirstDisplayLine && (value & 0x10) != 0) den_seen_ = true;
      check_badline();
      update_ba();
      break;

    case kRegRaster:
      raster_irq_ = static_cast<uint16_t>((raster_irq_ & 0x100) | value);
      break;

    case kRegSprEnable:
    case kRegSprYExp:
      regs_[r] = value;
      break;

    case kRegIrqFlags:
      /* writing a one acknowledges that source */
      irq_flags_ = static_cast<data_t>(irq_flags_ & ~(value & 0x0f));
      update_irq_line();
      break;

    case kRegIrqEnable:
      irq_enable_ = static_cast<data_t>(value & 0x0f);
      update_irq_line();
      break;

    default:
      if (r < 0x2f) regs_[r] = value;
      break;
  }

  /* the schedule the bus is holding may no longer be right */
  bus_.vic_rescheduled();
}

} /* namespace usbsid */
