/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mos6569.h
 * MOS 6569 / 6567 video interface controller, timing only.
 *
 * There is no pixel pipeline and there is no display. What this chip does
 * here is the part a SID player actually depends on:
 *
 *   - the raster counter and the raster interrupt, at the right cycle,
 *   - the bad line condition and the 40 cycles it steals from the CPU,
 *   - sprite DMA and the cycles it steals,
 *   - the frame boundary, which is where the SID buffer is flushed.
 *
 * Cycles stolen from the CPU are not cosmetic: every one of them shifts every
 * following SID write in the frame, which is exactly the error this player
 * exists to remove.
 *
 * Not emulated, because both need a rendered picture: sprite to sprite and
 * sprite to background collisions. Their registers and interrupt bits exist
 * and behave, but nothing ever sets a collision. A renderer, if one is ever
 * added, plugs into the same cycle table.
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
#ifndef _US_VIC_MOS6569_H_
#define _US_VIC_MOS6569_H_

#include "core/bus.h"
#include "io_device.h"
#include "types.h"
#include "vic_timing.h"

namespace usbsid {

/**
 * @brief Told when the VIC finishes a frame.
 *
 * The SID layer flushes its buffer here, which is what the current player
 * does at the end of vsync.
 */
class VicFrameObserver
{
  public:
    virtual ~VicFrameObserver(void) = default;
    virtual void vic_frame_ended(void) = 0;
};

class Mos6569 final : public ClockedDevice, public IoDevice
{
  public:
    Mos6569(Bus & bus, VideoModel model = VideoModel::Pal6569);
    ~Mos6569(void) override = default;

    void tick(void) override US_RAM_ATTR;
    void reset(void) override;

    /**
     * @brief Run up to the given tick count, skipping what it can.
     *
     * A raster line is sixty three cycles and about four of them are
     * interesting: where BA goes low for a bad line, where it comes back, the
     * raster compare at the top, and the line boundary. In between the chip
     * counts, and counting can be done in one step. Anything that looks at it
     * catches it up first, the same as the CIAs.
     *
     * With sprites fetching, every cycle is walked: the fetch windows overlap
     * and wrap into the next line, and a SID player almost never has sprites
     * on, so the simple thing is the right thing.
     */
    void catch_up(cycle_t target_ticks) US_RAM_ATTR;

    /** @brief Ticks it can be left alone for, at least one. */
    uint32_t cycles_to_event(void) const US_RAM_ATTR;

    /**
     * @brief Cycles left until the frame ends, counting the wrap itself.
     *
     * Running exactly this many lands on the frame boundary. It exists so the
     * player does not have to poll the frame counter every cycle: polling it
     * catches the chip up every cycle, which is the whole saving thrown away.
     */
    uint32_t cycles_to_frame_end(void) US_RAM_ATTR;

    /** @brief Cycles actually walked rather than skipped, for tuning. */
    cycle_t real_ticks(void) const { return real_ticks_; }

    data_t io_read(addr_t addr) override US_RAM_ATTR;
    void io_write(addr_t addr, data_t value) override US_RAM_ATTR;

    void set_frame_observer(VicFrameObserver * obs) { observer_ = obs; }

    void set_model(VideoModel model);
    VideoModel model(void) const { return model_; }
    const VicTiming & timing(void) const { return *timing_; }

    /* Where in the frame we are. These catch the chip up: it runs behind the
     * bus by design, and a stale raster line is a wrong answer. */
    uint16_t raster(void) { catch_up_now(); return raster_; }
    uint16_t cycle_in_line(void) { catch_up_now(); return cycle_; }
    bool badline(void) { catch_up_now(); return badline_; }
    uint64_t frames(void) { catch_up_now(); return frames_; }

    /* register state, for tests and for the player */
    data_t peek(uint8_t reg) const US_RAM_ATTR;
    data_t irq_flags(void) const { return irq_flags_; }
    data_t irq_enable(void) const { return irq_enable_; }
    bool sprite_dma(uint8_t n) const { return sprite_dma_[n & 7]; }

  private:
    enum : data_t {
      kIrqRaster    = 0x01,
      kIrqSpriteBg  = 0x02, /* needs a rendered picture, never set here */
      kIrqSprite    = 0x04, /* idem */
      kIrqLightpen  = 0x08,
      kIrqAny       = 0x80,
    };

    /* Registers, by their offset in the $d000 page */
    enum : uint8_t {
      kRegControl1  = 0x11,
      kRegRaster    = 0x12,
      kRegSprEnable = 0x15,
      kRegControl2  = 0x16,
      kRegSprYExp   = 0x17,
      kRegMemPtr    = 0x18,
      kRegIrqFlags  = 0x19,
      kRegIrqEnable = 0x1a,
      kRegSprSprCol = 0x1e,
      kRegSprBgCol  = 0x1f,
    };

    void update_irq_line(void) US_RAM_ATTR;
    void raise_irq(data_t source) US_RAM_ATTR;
    void check_badline(void) US_RAM_ATTR;
    void update_ba(void) US_RAM_ATTR;
    void catch_up_now(void) US_RAM_ATTR;
    void skip(uint32_t n) US_RAM_ATTR;
    void start_of_line(void) US_RAM_ATTR;
    void end_of_line(void) US_RAM_ATTR;
    bool sprite_steals_cycle(uint16_t cycle) const US_RAM_ATTR;
    Bus & bus_;
    VideoModel model_ = VideoModel::Pal6569;
    const VicTiming * timing_ = &kVicTiming[0];
    VicFrameObserver * observer_ = nullptr;

    uint16_t cycle_ = 1;       /* 1 .. cycles_per_line */
    cycle_t ticks_ = 0;       /* how far it has been advanced */
    cycle_t real_ticks_ = 0;  /* how much of that was walked */
    uint16_t raster_ = 0;
    uint16_t raster_irq_ = 0;  /* nine bits, from $d012 and $d011 bit 7 */
    uint64_t frames_ = 0;

    bool badline_ = false;
    bool den_seen_ = false;    /* DEN was set somewhere in raster $30 */
    bool ba_ = true;

    data_t regs_[0x40] = { 0 };
    data_t irq_flags_ = 0;
    data_t irq_enable_ = 0;

    /* sprite DMA, enough of it to steal the right cycles */
    bool sprite_dma_[8] = { false };
    uint8_t sprite_line_[8] = { 0 };
    bool sprite_expand_toggle_[8] = { false };

    /* Which cycles of a line the sprites currently in DMA pull BA low on, and
     * from each cycle where the next change to that is.
     *
     * Both were worked out from the eight windows every time they were needed,
     * which was on every tick (`update_ba()`) and on every scheduling decision
     * (`cycles_to_event()`), with a wrap loop inside. They only change when a
     * sprite's DMA does, which is twice a line at most, so they are built
     * there instead.
     *
     * The edge list is what lets the scheduler skip properly. Stopping at
     * every individual window edge meant a stop every two cycles once several
     * sprites were active, because the windows are five cycles wide and two
     * apart, so they merge and most of those edges are inside a merged run and
     * change nothing. These are the edges of the union and nothing else: every
     * cycle whose BA differs from the cycle before it, the previous line's
     * last cycle counting as the one before cycle one, which is what makes the
     * windows that wrap come out right. */
    static constexpr uint16_t kMaxLineCycles = 72; /* PAL 63, NTSC 65 */
    bool sprite_ba_low_[kMaxLineCycles] = { false };
    uint8_t sprite_ba_edges_[16] = { 0 };
    uint8_t sprite_ba_nedges_ = 0;
    bool sprite_ba_any_ = false;

    /** @brief Rebuild the two tables above from `sprite_dma_`. */
    void rebuild_sprite_ba(void) US_RAM_ATTR;
};

} /* namespace usbsid */

#endif /* _US_VIC_MOS6569_H_ */
