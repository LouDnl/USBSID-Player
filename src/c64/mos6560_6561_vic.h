/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * Parts if this emulator are based on other great emulators and players
 * like Vice, SidplayFp, Websid, SidBerry and emudore/adorable
 *
 * mos6560_6561_vic.h
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025-2026 LouD
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

#ifndef _MOS6560_6561_H
#define _MOS6560_6561_H

#include <cstdint>
#include <thread>
#include <chrono>

#include <types.h>


class mos6510;
class mos6581_8580;


/* MOS6560/6561 VIC-II */
class mos6560_6561
{
  private:
    /* Glue */
    mos6510 *cpu;
    mos6581_8580 *sid;

    CPUCLOCK vic_cpu_clock;
    CPUCLOCK prev_vic_cpu_clock;

    /* Debugging only */
    uint8_t shadow_regs[0x40] = {0};

    static const addr_t kVicStart = 0xd000;

    enum pVICregisters {
      SPR_X_COORD_MSB  = 0x10,
      CONTROLA         = 0x11,
      RASTERROWL       = 0x12,
      LIGHTPEN_X_COORD = 0x13,
      LIGHTPEN_Y_COORD = 0x14,
      SPRITE_ENABLE    = 0x15,
      CONTROLB         = 0x16,
      MEMORY_PTRS      = 0x18,
      INTERRUPT        = 0x19,
      INTERRUPT_ENABLE = 0x1A,
      BORDER_COLOR     = 0x20,
      BG_COLORS_START  = 0x21,
      BG_COLORS_END    = 0x24
    };

    enum pControlBitVal {
      RASTERROWMSB   = 0x80,
      DISPLAY_ENABLE = 0x10,
      ROWS           = 0x08,
      YSCROLL_MASK   = 0x07
    };

    enum pInterruptBitVal {
      VIC_IRQ = 0x80,
      RASTERROW_MATCH_IRQ = 0x01
    };

    /* graphic modes */
    enum pGraphicMode
    {
      pCharMode,
      pMCCharMode,
      pBitmapMode,
      pMCBitmapMode,
      pExtBgMode,
      pIllegalMode,
    };
    pGraphicMode graphic_mode_;

    uint_fast8_t r_sprite_x[8]  = {0};
    uint_fast8_t r_sprite_y[8]  = {0};
    uint_fast8_t r_sprite_msbs  = 0;
    uint_fast8_t r_border_color = 0;
    uint_fast8_t r_bg_colors[4] = {0};
    uint_fast8_t r_lightpen_x   = 0;
    uint_fast8_t r_lightpen_y   = 0;

    uint_fast8_t control_register_one;
    uint_fast8_t control_register_one_read;
    uint_fast8_t control_register_two;
    uint_fast8_t raster_row_lines;
    uint_fast8_t sprite_enabled;
    uint_fast8_t irq_status;
    uint_fast8_t irq_enabled;

    uint_fast8_t memory_ptrs;

    counter_t row_cycle_count;
    counter_t raster_irq;

    typedef val_t (*VicReadDMA)(addr_t);

    std::chrono::steady_clock::time_point prev_frame_was_at_;
    uint_fast16_t raster_row(void);
    bool stun(void);
    void vsync_do_end_of_line(void);

  public:
    mos6560_6561(void);
    ~mos6560_6561(void);

    void reset(void);
    void glue_c64(VicReadDMA rdma, mos6510 *_cpu, mos6581_8580 *_sid);

    reg_t read_register(reg_t reg);
    void write_register(reg_t reg, val_t value);

    void emulate(void);
    int set_timer_speed(int speed);

    /* VIC-II DMA read callback function */
    VicReadDMA vic_dma_read;

     /* Set in set_timer_speed() start */
    double ticks_per_frame;
    double emulated_clk_per_second;

    /* Set to 0 in reset() */
    tick_t last_sync_emulated_tick;
    tick_t last_sync_tick;
    CPUCLOCK last_sync_clk;
    tick_t sync_target_tick;
    tick_t   start_sync_tick;
    CPUCLOCK start_sync_clk;

    int timer_speed = 0; /* Percentage */
    bool sync_reset = true;
    bool metrics_reset = false;

    /* Number of clock cycles per seconds on the real machine. */
    long cycles_per_sec;
    /* Number of frames per second on the real machine. */
    double refresh_frequency;
    double refresh_rate;
    /* Set on start */
    counter_t raster_lines;
    cycle_t raster_row_cycles;

    cycle_t prev_raster_line; /* Set to 0 in reset() */

    void graphic_mode(void);
    void dump_regs(void);
    void dump_irqs(void);
    void dump_timers(void);


};

#endif /* _MOS6560_6561_H */
