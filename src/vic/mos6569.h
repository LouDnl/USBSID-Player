/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * mos6569.h
 * MOS 6569 / 6567 video interface controller, timing only
 * (no pixel pipeline, no display)
 *
 *   - raster counter and raster interrupt
 *   - bad line condition (steals 40 cycles from the CPU)
 *   - sprite DMA and the cycles it steals
 *   - frame boundary, flushes the SID buffer
 *
 * Sprite collisions are not emulated (requires a rendered picture),
 * registers and interrupt bits exist but are never set
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
#ifndef _US_VIC_MOS6569_H_
#define _US_VIC_MOS6569_H_

#include "core/bus.h"
#include "io_device.h"
#include "types.h"
#include "vic_timing.h"

namespace usbsid {

/**
 * @brief Called when the VIC finishes a frame, the SID layer flushes its buffer
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
     * @brief Run up to the given tick count, skipping uneventful cycles
     *
     * Per line only BA low (bad line), BA back, the raster compare and the line boundary matter
     * With sprites fetching every cycle is walked (fetch windows overlap and wrap into the following line)
     */
    void catch_up(cycle_t target_ticks) US_RAM_ATTR;

    /** @brief Ticks it can be left alone for, at least one */
    uint32_t cycles_to_event(void) const US_RAM_ATTR;

    /**
     * @brief Cycles left until the frame ends (wrap included), running this many lands on the frame boundary
     */
    uint32_t cycles_to_frame_end(void) US_RAM_ATTR;

    /** @brief Cycles actually walked, for tuning */
    cycle_t real_ticks(void) const { return real_ticks_; }

    data_t io_read(addr_t addr) override US_RAM_ATTR;
    void io_write(addr_t addr, data_t value) override US_RAM_ATTR;

    void set_frame_observer(VicFrameObserver * obs) { observer_ = obs; }

    void set_model(VideoModel model);
    VideoModel model(void) const { return model_; }
    const VicTiming & timing(void) const { return *timing_; }

    /* Position in the frame, catches the chip up first (it runs behind the bus) */
    uint16_t raster(void) { catch_up_now(); return raster_; }
    uint16_t cycle_in_line(void) { catch_up_now(); return cycle_; }
    bool badline(void) { catch_up_now(); return badline_; }
    uint64_t frames(void) { catch_up_now(); return frames_; }

    /* Register state */
    data_t peek(uint8_t reg) const US_RAM_ATTR;

    /** @brief Raster counter as $d011/$d012 shows it */
    uint16_t visible_raster(void) const US_RAM_ATTR;
    data_t irq_flags(void) const { return irq_flags_; }
    data_t irq_enable(void) const { return irq_enable_; }
    bool sprite_dma(uint8_t n) const { return sprite_dma_[n & 7]; }

  private:
    enum : data_t {
      kIrqRaster    = 0x01,
      kIrqSpriteBg  = 0x02, /* Needs a rendered picture, never set */
      kIrqSprite    = 0x04, /* Same */
      kIrqLightpen  = 0x08,
      kIrqAny       = 0x80,
    };

    /* Registers by offset in the $d000 page */
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
    cycle_t ticks_ = 0;       /* Ticks advanced */
    cycle_t real_ticks_ = 0;  /* Ticks walked */
    uint16_t raster_ = 0;
    uint16_t raster_irq_ = 0;  /* 9 bits, $d012 and $d011 bit 7 */
    uint64_t frames_ = 0;

    bool badline_ = false;
    bool den_seen_ = false;    /* DEN set somewhere in raster $30 */
    bool ba_ = true;

    data_t regs_[0x40] = { 0 };
    data_t irq_flags_ = 0;
    data_t irq_enable_ = 0;

    /* Sprite DMA, enough to steal the right cycles */
    bool sprite_dma_[8] = { false };
    uint8_t sprite_line_[8] = { 0 };
    bool sprite_expand_toggle_[8] = { false };

    /* Cycles of a line where sprite DMA pulls BA low, rebuilt when sprite DMA changes (max twice a line)
     * Edge list = cycles whose BA differs from the previous cycle (previous line last cycle precedes cycle 1)
     * Sprite windows are 5 cycles wide and 2 apart, they merge, only the union edges count */
    static constexpr uint16_t kMaxLineCycles = 72; /* PAL 63, NTSC 65 */
    bool sprite_ba_low_[kMaxLineCycles] = { false };
    uint8_t sprite_ba_edges_[16] = { 0 };
    uint8_t sprite_ba_nedges_ = 0;
    bool sprite_ba_any_ = false;

    /** @brief Rebuild the two tables above from sprite_dma_ */
    void rebuild_sprite_ba(void) US_RAM_ATTR;
};

} /* namespace usbsid */

#endif /* _US_VIC_MOS6569_H_ */
