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
 * mos6560_6561_vic.cpp
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

#include <mos6510_cpu.h>
#include <mos6581_8580_sid.h>
#include <mos6560_6561_vic.h>
#include <timer.h>
#include <c64util.h>

#if EMBEDDED
#include <pico/types.h>
#include <pico/time.h>
extern "C" uint32_t clockcycles(void);
extern "C" void clockcycle_delay(uint32_t n_cycles);
#endif


/**
 * @brief Construct a new mos6560_6561::mos6560_6561 object
 *
 */
mos6560_6561::mos6560_6561(void)
{
  MOSDBG("[VIC] Init\n");

  /* Default to PAL until changed */
  cycles_per_sec = 958248;
  refresh_rate = 19950.0;
  refresh_frequency = 50.125; /* (double)(cycles_per_sec / refresh_rate) */
  raster_lines = 312;
  raster_row_cycles = 63;

  reset();

  set_timer_speed(100);
  return;
}

/**
 * @brief Destroy the mos6560 6561::mos6560 6561 object
 *
 */
mos6560_6561::~mos6560_6561(void)
{
  MOSDBG("[VIC] Deinit\n");
  return;
}

/**
 * @brief Glue required C64 parts
 *
 */
void mos6560_6561::glue_c64(VicReadDMA rdma, mos6510 *_cpu, mos6581_8580 *_sid)
{
  cpu = _cpu;
  sid = _sid;
  vic_dma_read = (VicReadDMA)rdma;
}

/**
 * @brief Reset the VIC-II register values to default settings
 *
 */
void mos6560_6561::reset(void)
{
  row_cycle_count = 0;

  control_register_one = 0;
  control_register_one_read = 0;
  control_register_two = 0;
  raster_row_lines = 0;
  sprite_enabled = 0;
  raster_irq = 0;
  irq_status = 0;
  irq_enabled = 0;
  prev_raster_line = 0;

  r_sprite_x[8]   = {0};
  r_sprite_y[8]   = {0};
  r_sprite_msbs   = 0;
  r_border_color  = 0;
  r_bg_colors[4]  = {0};
  r_lightpen_x   = 0;
  r_lightpen_y   = 0;

  memory_ptrs = 0b1u; /* Bit 0 is not used and always set to 1 */

  return;
}

/**
 * @brief VIC-II register write access
 *
 * @param reg
 * @return reg_t
 */
reg_t mos6560_6561::read_register(reg_t reg)
{
  /* DMA read return value from shadow RAM first */
  val_t data = shadow_regs[reg];

  switch (reg) {
    case SPR_X_COORD_MSB: /* 0x10 */
      data = r_sprite_msbs;
      return data;
    case CONTROLA: /* 0x11 */
      data = control_register_one;
      return data;
    case RASTERROWL: /* 0x12 */
      data = raster_row_lines;
      return data;
    case LIGHTPEN_X_COORD: /* 0x13 */
      data = r_lightpen_x;
      return data;
    case LIGHTPEN_Y_COORD: /* 0x14 */
      data = r_lightpen_y;
      return data;
    case SPRITE_ENABLE: /* 0x15 */
      data = sprite_enabled;
      return data;
    case CONTROLB: /* 0x16 */
      data = control_register_two;
      return data;
    case MEMORY_PTRS: /* 0x18 */
      data = memory_ptrs;
      return data;
    /**
     * interrupt status register
     * IRQ|  - |  - |  - | ILP|IMMC|IMBC|IRST|
     */
    case INTERRUPT: /* 0x19 */
      data = (0x0fu & irq_status);
      data |= (((data != 0) << 7) | 0x70u);
      return data;
    /**
     * interrupt enable register
     *   - |  - |  - |  - | ELP|EMMC|EMBC|ERST|
     */
    case INTERRUPT_ENABLE: /* 0x1A */
      data = (irq_enabled & 0x0fu);
      return data;
    case BORDER_COLOR: /* 0x20 */
      data = r_border_color;
      return data;
    case BG_COLORS_START ... BG_COLORS_END: /* 0x21 ~ 0x24 */
      data = r_bg_colors[(reg-0x21u)];
      return data;
    default:
      break;
  }

  /* Sprites 0x00 -> 0x0f */
  if _MOS_UNLIKELY (reg <= 0xfu) {
    switch (reg % 2u) {
      case 1: /* Uneven ~ store Y coord of sprite n*/
        data = r_sprite_x[reg >> 1u];
        break;
      case 0:  /* Even ~ store X coord of sprite n */
        data = r_sprite_x[reg >> 1u];
        break;
    }
  }

  return data;
}

/**
 * @brief VIC-II register read access
 *
 * @param reg
 * @param value
 */
void mos6560_6561::write_register(reg_t reg, val_t value)
{

  switch (reg) {
    case SPR_X_COORD_MSB: /* 0x10 */
      r_sprite_msbs = value;
      return;
    case CONTROLA: /* 0x11 */
      control_register_one = (value&0x7fu);
      raster_irq = ((value&RASTERROWMSB) << 1u);
      return;
    case RASTERROWL: /* 0x12 */
      raster_irq = value | (raster_irq & 0x100u);
      return;
    case LIGHTPEN_X_COORD: /* 0x13 */
      r_lightpen_x = value;
      return;
    case LIGHTPEN_Y_COORD: /* 0x14 */
      r_lightpen_y = value;
      return;
    case SPRITE_ENABLE: /* 0x15 */
      sprite_enabled = value;
      return;
    case CONTROLB: /* 0x16 */
      control_register_two = value;
      return;
    case MEMORY_PTRS: /* 0x18 */
      memory_ptrs = (value | 0b1u);
      return;
    case INTERRUPT: /* 0x19 */
      irq_status &= ~(value&0xfu);
      return;
    case INTERRUPT_ENABLE: /* 0x1A */
      irq_enabled = (value&0xfu);
      return;
    case BORDER_COLOR: /* 0x20 */
      r_border_color = value;
      return;
    case BG_COLORS_START ... BG_COLORS_END: /* 0x21 ~ 0x24 */
      r_bg_colors[(reg-0x21u)] = value;
      return;
    default:
      break;
  }

  /* Sprites 0x00 -> 0x0f */
  if _MOS_UNLIKELY (reg <= 0xfu) {
    switch (reg % 2u) {
      case 1: /* Uneven ~ store Y coord of sprite n*/
        r_sprite_x[reg >> 1u] = value;
        #if DESKTOP
        // detect_sprite_sprite_collision(reg>>1u);
        #endif
        return;
      case 0:  /* Even ~ store X coord of sprite n */
        r_sprite_x[reg >> 1u] = value;
        #if DESKTOP
        // detect_sprite_sprite_collision(reg>>1u);
        #endif
        return;
    }
  }

  /* if we arrive here, that means the register written isn't implemented */
  /* store write in shadow registers */
  shadow_regs[reg] = value;

  return;
}

/**
 * @brief: emulate a single VIC-II raster cycle run
 * NOTE: not single cycle exact
 *
 */
void mos6560_6561::emulate(void)
{
  vic_cpu_clock = cpu->cycles();
  static tick_t cycles;

  cycles = (vic_cpu_clock - prev_vic_cpu_clock);
  static tick_t vic_cycles;
  vic_cycles+=cycles;

  row_cycle_count += cycles;

  // if _MOS_UNLIKELY ((row_cycle_count >= 55) && (row_cycle_count <= 62)) {
  //   int sprite_id = (row_cycle_count - 55)/*  / 2 */;

  //   /* Check if sprite is enabled ($D015) */
  //   if _MOS_UNLIKELY (sprite_enabled & (1 << sprite_id)) {
  //       cpu->tickle_me(1); /* Stall CPU by simulating to pull the Bus Available line low */
  //       // cpu->stall_cpu(true); /* Stall CPU by simulating to pull the Bus Available line low */
  //       // MOSDBG("[VIC] row_cycle_count: %u sprite_id: %d\n",row_cycle_count,sprite_id);
  //       /* Perform DMA Reads */
  //       /* Fetch Pointer from end of current Screen Matrix ($07F8-$07FF) */
  //       // uint16_t ptr_addr = 0x07F8 + sprite_id;
  //       // uint8_t pointer = s->ram[ptr_addr];
  //       // uint8_t sprite_pointer = vic_dma_read(ptr_addr);

  //       // Fetch 3 bytes of sprite data for this line
  //       // uint16_t data_addr = pointer * 64 + (s->raster_line % 21) * 3;

  //       // In a real emulator, these reads happen across specific phases
  //       // s->sprite_buffers[sprite_id][0] = s->ram[data_addr];
  //   } else {
  //       cpu->stall_cpu(false);
  //   }
  // } else {
  //   cpu->stall_cpu(false);
  // }

  if _MOS_UNLIKELY (row_cycle_count >= raster_row_cycles) {
    row_cycle_count -= raster_row_cycles;

    if (stun()) {
      cpu->cycles(cpu->cycles()+23);
    }

    // uint_fast16_t raster_row = (((control_register_one & RASTERROWMSB) << 1u) + raster_row_lines);
    uint_fast16_t current_raster_row_ = raster_row();

    // ++raster_row;

    if _MOS_UNLIKELY ((irq_enabled & RASTERROW_MATCH_IRQ) &&
      (current_raster_row_ == raster_irq)) {
      irq_status |= (VIC_IRQ | RASTERROW_MATCH_IRQ);
      cpu->irq(2);
    }

    ++current_raster_row_;

    if _MOS_UNLIKELY (current_raster_row_ >= raster_lines) {
      current_raster_row_ = 0;
      vic_cycles = 0;
      sid->sid_flush();
      vsync_do_end_of_line();
    }

    control_register_one = ((control_register_one & ~RASTERROWMSB) | ((current_raster_row_ & 0x100) >> 1));
    raster_row_lines = (current_raster_row_ & 0xFF);

    // MOSDBG("[VIC] RL:%u RRC:%u RR:%u RRL:%u RCYC:%u IRQ@%u IRQE:%x IRQST:%x\n",
    //   raster_lines,
    //   raster_row_cycles,
    //   raster_row_lines,
    //   raster_row,
    //   row_cycle_count,
    //   raster_irq,
    //   irq_enabled,
    //   irq_status);
  }

  // MOSDBG("[VIC] RL:%u RRC:%u RRL:%u RCYC:%u IRQ:%x ST:%x\n",
  //   raster_lines,
  //   raster_row_cycles,
  //   raster_row_lines,
  //   // raster_row,
  //   row_cycle_count,
  //   raster_irq,
  //   irq_status);

  prev_vic_cpu_clock = vic_cpu_clock;
  return;
}

/**
 * @brief Return the current raster row based on registers
 * raster_row_lines and control_register_one
 *
 * @return uint_fast16_t
 */
uint_fast16_t mos6560_6561::raster_row(void)
{
  return (((control_register_one & RASTERROWMSB) << 1u) + raster_row_lines);
}

/**
 * @brief Returns true if we need to stun the cpu right now
 *
 * @return true
 * @return false
 */
bool mos6560_6561::stun(void)
{
  uint_fast16_t current_raster_row_ = raster_row();
  bool _stun = (
    (current_raster_row_ >= 0x30) /* 48 */
    && (current_raster_row_ <= 0xf7 /* 247 */
      && (current_raster_row_ & 0x7 == (control_register_one & 0x7))));
  return _stun;
}

/**
 * @brief Initialize vsync timers and set relative speed of emulation in percent
 *
 * @param speed
 * @return int
 */
int mos6560_6561::set_timer_speed(int speed)
{
  double cpu_percent;

  timer_speed = speed;

  if (refresh_frequency <= 0) {
    /* Happens during init */
    return -1;
  }

  if (speed < 0) {
    /* negative speeds are fps targets */
    cpu_percent = 100.0 * ((0 - speed) / refresh_frequency);
  } else {
    /* positive speeds are cpu percent targets */
    cpu_percent = speed;
  }

  ticks_per_frame = tick_per_second() * 100.0 / cpu_percent / refresh_frequency;
  emulated_clk_per_second = cycles_per_sec * cpu_percent / 100.0;

  MOSDBG("[VIC] RATE:%f FREQ:%.3f CYC/S:%u RASTERLINES:%u ROWCYCLES:%u TIMER:%u TICKS/FR:%g EMUCLK/S:%g\n",
    refresh_rate,
    refresh_frequency,
    cycles_per_sec,
    raster_lines,
    raster_row_cycles,
    timer_speed,
    ticks_per_frame,
    emulated_clk_per_second
  );

  return 0;
}

/**
 * @brief Vice end of raster line VSYNC
 *
 */
void mos6560_6561::vsync_do_end_of_line(void)
{
  const int microseconds_between_sync = 2 * 1000;

  tick_t tick_between_sync = tick_per_second() / (1000000 / microseconds_between_sync);
  tick_t tick_now;
  tick_t tick_delta;
  tick_t ticks_until_target;

  bool tick_based_sync_timing = true; /* Fixed value */

  CPUCLOCK main_cpu_clock = cpu->cycles();
  CPUCLOCK sync_clk_delta;
  double sync_emulated_ticks;

  /* used to preserve the fractional ticks betwen calls */
  static double sync_emulated_ticks_offset;

  tick_now = tick_now_after(last_sync_tick);

  if (sync_reset) {
    MOSDBG("[VIC] Sync reset @ tick: %u CPU @ %u cycles\n",
      tick_now, main_cpu_clock);
    sync_reset = false;
    metrics_reset = true;

    last_sync_emulated_tick = tick_now;
    last_sync_tick = tick_now;
    last_sync_clk = main_cpu_clock;
    sync_target_tick = tick_now;

    return;
  }

  /* how many host ticks since last sync. */
  tick_delta = tick_now - last_sync_tick;
  /* is it time to consider keyboard, joystick ? */
  if (tick_delta >= tick_between_sync) {

    /*
     * Compare the emulated time vs host time.
     *
     * First, add the emulated clock cycles since last sync.
     */

    sync_clk_delta = main_cpu_clock - last_sync_clk;

    /* amount of host ticks that represents the emulated duration */
    sync_emulated_ticks = (double)tick_per_second() * sync_clk_delta / emulated_clk_per_second;

    /* combine with leftover offset from last sync */
    sync_emulated_ticks += sync_emulated_ticks_offset;

    /* calculate the ideal host tick representing the emulated tick */
    sync_target_tick += sync_emulated_ticks;

    /* Preserve the fractional component for next time */
    sync_emulated_ticks_offset = sync_emulated_ticks - (tick_t)sync_emulated_ticks;

    /*
      * How many host ticks to catch up with emulator?
      *
      * If we are behind the emulator, this will be a giant number
      */

    // BUG: with PSID tunes ticks_until_target is WAY too high!!!
    ticks_until_target = sync_target_tick - tick_now;

    if (ticks_until_target < tick_per_second()) {
      /* Emulation timing / sync is OK. */

      // /* If we can't rely on the audio device for timing, slow down here. */
      if (tick_based_sync_timing) {
        // mainlock_yield_and_sleep(ticks_until_target);
        tick_sleep(ticks_until_target);
        // MOSDBG("[VIC] SLEPT FOR %u\n",ticks_until_target);

      }
    } else if ((tick_t)0 - ticks_until_target > tick_per_second()) {
      /* We are more than a second behind, reset sync and accept that we're not running at full speed. */

      MOSDBG("Sync is %.3f ms behind (%.3f %u %u %u) (%u %u)\n", (double)TICK_TO_MICRO((tick_t)0 - ticks_until_target) / 1000,
        (double)TICK_TO_MICRO(0 - ticks_until_target), tick_between_sync, ticks_until_target, tick_per_second(),
        cpu->cycles(), sync_clk_delta);
      sync_reset = true;
    } else {
      // tick_sleep(ticks_until_target*ticks_until_target);
      // MOSDBG("[VIC] SLEPT FOR %u\n",ticks_until_target*ticks_until_target);
      // sync_reset = true;
      /* We are running slow - make sure we still yield to the UI thread if it's waiting */

      // mainlock_yield();
    }

    last_sync_tick = tick_now;
    last_sync_clk = main_cpu_clock;
  }
}

/**
 * @brief Sets the VIC graphics mode, for debug and logging purposes only!
 *
 */
void mos6560_6561::graphic_mode(void)
{
  uint ecm = (control_register_one & 0x40u);
  uint bmm = (control_register_one & 0x20u);
  uint mcm = (control_register_two & 0x10u);

  if(!ecm && !bmm && !mcm)
    graphic_mode_ = pCharMode;
  else if(!ecm && !bmm && mcm)
    graphic_mode_ = pMCCharMode;
  else if(!ecm && bmm && !mcm)
    graphic_mode_ = pBitmapMode;
  else if(!ecm && bmm && mcm)
    graphic_mode_ = pMCBitmapMode;
  else if(ecm && !bmm && !mcm)
    graphic_mode_ = pExtBgMode;
  else /* TODO: YEAH, WHAT EXACTLY */
    graphic_mode_ = pMCCharMode/* kIllegalMode */;
    // graphic_mode_ = kIllegalMode;
}

/**
 * @brief debug logging of IRQ status and enabled registers
 *
 */
void mos6560_6561::dump_irqs(void)
{
  MOSDBG("VIC:%02x%02x",irq_status,irq_enabled);
  return;
}

/**
 * @brief debug logging of registers CRA, RRL, SPR, CRB, IRQ and IQE
 *
 */
void mos6560_6561::dump_regs(void)
{
  MOSDBG("[CRA]%02x[RRL]%02x[SPR]%02x[CRB]%02x[IRQ]%02x[IQE]%02x",
    control_register_one,raster_row_lines,sprite_enabled,control_register_two,
    irq_status,irq_enabled
  );

  return;
}

/**
 * @brief debug logging of the VIC-II timers
 *
 */
void mos6560_6561::dump_timers(void)
{
  MOSDBG("[VIC][RR I:%3u/L%3u] ",
    raster_irq,raster_row_lines
  );
  return;
}
