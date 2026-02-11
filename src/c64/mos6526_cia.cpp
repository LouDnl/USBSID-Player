/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * Parts if this emulator are based on other great emulators and players
 * like Vice, SidplayFp, Websid and emudore/adorable
 *
 * mos6526_cia.cpp
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
#include <mos6526_cia.h>
#include <c64util.h>

extern bool log_cia1rw, log_cia2rw;


/**
 * @brief Construct a new mos6526::mos6526 object
 *
 * @param base_address
 */
mos6526::mos6526(uint_least16_t base_address) :
  cia_address(base_address)
{
  is_cia2 = _is_cia2();

  MOSDBG("[CIA] %d Init\n",(is_cia2+1));

  if (is_cia2 && log_cia2rw) {
    log_rw = true;
    MOSDBG("[CIA 2] Read/Write logging enabled\n");
  }
  if (!is_cia2 && log_cia1rw) {
    log_rw = true;
    MOSDBG("[CIA 1] Read/Write logging enabled\n");
  }

  MOSDBG("[CIA] %d created @ $%04x\n",(is_cia2?2:1),cia_address);
  reset();
  return;
}

/**
 * @brief Destroy the mos6526::mos6526 object
 *
 */
mos6526::~mos6526(void)
{
  MOSDBG("[CIA] %d Deinit\n",(is_cia2+1));
  return;
}

/**
 * @brief Glue required C64 parts
 *
 */
void __us_not_in_flash_func(glue_c64) mos6526::glue_c64(mos6510 * _cpu)
{
  cpu = _cpu;

  cia_cpu_clock = prev_cia_cpu_clock = cpu->cycles();

  return;
}

/**
 * @brief Set the cycle read callback function (unused!)
 */
void mos6526::set_cycle_fn(ReadCycles c)
{
  cpu_cycles = (ReadCycles)c;
  return;
}

/**
 * @brief Reset the CIA register values to default settings
 *
 */
void __us_not_in_flash_func(reset) mos6526::reset()
{
  /* Base */
  cia_cpu_clock = prev_cia_cpu_clock = 0;
  prev_timer_a_counter = prev_timer_b_counter = 0;

  /* TOD to zero */
  tod_counter = 0x00000000;
  tod_tenths = tod_seconds = tod_minutes = tod_hours = 0;
  /* Enable Time of day */
  tod_running = true;
  /* Disable TOD latched */
  tod_latched = false;

  /* No IRQ's on boot */
  irq_enabled = irq_triggered = false;
  alarm_irq_enabled = flag_irq_enabled = false;
  alarm_irq_triggered = flag_irq_triggered = false;

  /* Init all timers to zero */
  timer_a_prescaler = timer_a_counter = 0x4025; /* Default at PAL */
  timer_b_prescaler = timer_b_counter = 0xFFFF; /* Default disabled */
  /* Disable all timers and IRQ's */
  timer_a_enabled = pStartTimer; /* Enabled */
  timer_b_enabled = pStopTimer;  /* Disabled */
  // timer_a_irq_enabled = timer_b_irq_enabled = false;
  timer_a_irq_enabled = true;
  timer_b_irq_enabled = false;
  /* Set IRQ triggers to false */
  timer_a_irq_triggered = timer_b_irq_triggered = false;
  /* Timers default input mode is PHI2 */
  timer_a_input_mode = timer_b_input_mode = pModePHI2;
  /* Timers default run mode is continuous */
  timer_a_run_mode = timer_b_run_mode = pModeContinuous;
  /* Set default timer PortB modes */
  timer_a_portb_out = timer_b_portb_out = pNormalOperation;
  /* Set default timer output modes */
  timer_a_output_mode = timer_b_output_mode = pPulse;
  /* Set force load and underflow disabled */
  timer_a_force_load = timer_b_force_load = false;
  timer_a_underflow = timer_b_underflow = false;
  /* Set CRA Serialport mode */
  timer_a_sp_mode = pInput;
  /* Set CRA TOD Hertz */
  timer_a_is_50hz = p50Hz;
  /* Set CRB Time of Day write registers mode */
  timer_b_wrtod_mode = pWriteTOD;

  _sdr = 0;

  /* Keyboard ~ PRA and PRB are 0xFF at boot, bus is inverted */
  _pra = 0xff;
  _prb = 0xff;
  _ddra = _ddrb = 0xff;
  /* CIA 2 */
  vic_base_addr = 0x0000;
}

/**
 * @brief Cia1/Cia2 register read acccess
 *
 * @param reg
 * @return uint8_t
 */
uint8_t __us_not_in_flash_func(read_register) mos6526::read_register(uint8_t reg)
{
  uint8_t data = 0;
  switch(reg) {
    /* data port a (0x0), keyboard matrix cols and joystick #2 */
    case PRA:
      if (is_cia2) {
        data = _pra | ~(_ddra & 0x3f);
        break;
      }
    /* data port b (0x1), keyboard matrix rows and joystick #1 */
    case PRB:
      if (is_cia2) {
        data = _prb | ~(_ddrb & 0x3f);
      } else {
        if (_pra == 0xff) data = 0xff;
        else if(_pra /* && _ddra == 0xff */) { /* Written to by */
          int col = 0;
          uint8_t v = ~_pra;
          while (v >>= 1)col++;
          data = _kb_matrix[col];
        } else {
          data =  0xff;
        }
      }
      break;
    /* data direction port a (0x2) */
    case DDRA:
      data = _ddra;
      break;
    /* data direction port b (0x3) */
    case DDRB:
      data = _ddrb;
      break;
    /* timer a low byte (0x4) */
    case TAL:
      data = (uint8_t)(timer_a_counter & 0xff);
      break;
    /* timer a high byte (0x5) */
    case TAH:
      data = (uint8_t)(timer_a_counter & 0xff00) >> 8;
      break;
    /* timer b low byte (0x6) */
    case TBL:
      data = (uint8_t)(timer_b_counter & 0xff);
      break;
    /* timer b high byte (0x7) */
    case TBH:
      data = (uint8_t)(timer_b_counter & 0xff00) >> 8;
      break;
    /* RTC 1/10s (0x8) */
    case TODTEN:
      data = (tod_latched ? (tod_counter & 0xff) : tod_tenths);
      tod_latched = (tod_latched ? false : tod_latched);
      break;
    /* RTC seconds (0x9) */
    case TODSEC:
      data = (tod_latched ? ((uint8_t)(tod_counter & 0xff00) >> 8) : tod_seconds);
      break;
    /* RTC minutes (0xA) */
    case TODMIN:
      data = (tod_latched ? ((uint8_t)(tod_counter & 0xff0000) >> 16) : tod_minutes);
      break;
    /* RTC hours (0xB) */
    case TODHR:
      tod_latched = true;
      tod_counter = ((tod_hours<<24)|(tod_minutes<<16)|(tod_seconds<<8)|tod_tenths);
      data = (uint8_t)(tod_counter & 0xff000000) >> 24;
      break;
    /* shift serial (0xC) */
    case SDR:
      data = _sdr;
      break;
    /* interrupt control and status (0xD) */
    case ICR:
      if(irq_triggered) data |= (1 << 7); // IRQ occured
      if(timer_a_irq_triggered) data |= (1 << 0);
      if(timer_b_irq_triggered) data |= (1 << 1);
      if(alarm_irq_triggered)   data |= (1 << 2);
      /* sdr alarm ~ not used */
      if(flag_irq_triggered)    data |= (1 << 4);

      if(log_rw) MOSDBG("[R CIA%d] [ICR] $%02x:%02x [RESULT] 0b%d%d%d%d%d%d%d%d\n",
        (is_cia2 ? 2 : 1),
        ICR,data,
        irq_triggered,0,0,
        flag_irq_triggered,
        0,
        alarm_irq_triggered,
        timer_b_irq_triggered,
        timer_a_irq_triggered);
      /* Read disables all IRQ's */
      irq_triggered = false;
      alarm_irq_triggered = flag_irq_triggered = false;
      timer_a_irq_triggered = timer_b_irq_triggered = false;
      break;
    /* control timer a (0xE) */
    case CRA:
      /* 0b00000001 */
      if (timer_a_enabled) data = SET_BIT(data,ENABLE_TIMER);
      /* 0b00000010 */
      if (timer_a_portb_out) data = SET_BIT(data,PORTBx_TIMER);
      /* 0b00000100 */
      if (timer_a_output_mode) data = SET_BIT(data,TOGGLED_PORTBx);
      /* 0b00001000 */
      if (timer_a_run_mode) data = SET_BIT(data,ONESHOT_TIMER);
      /* 0b00010000 ~ force load strobe always reads zero */
      /* 0b00100000 */
      if (timer_a_input_mode) data = SET_BIT(data,TIMER_FROM_CNT);
      /* 0b01000000 */
      if (timer_a_sp_mode) data = SET_BIT(data,SERIALPORT_IS_OUTPUT);
      /* 0b10000000 */
      if (timer_a_is_50hz) data = SET_BIT(data,TIMEOFDAY_50Hz);

      if(log_rw) MOSDBG("[R CIA%d] [CRA] $%02x:%02x [RESULT] 0b%d%d%d%d%d%d%d%d\n",
        (is_cia2 ? 2 : 1),
        CRA,data,
        timer_a_is_50hz,
        timer_a_sp_mode,
        timer_a_input_mode,
        timer_a_force_load,
        timer_a_run_mode,
        timer_a_output_mode,
        timer_a_portb_out,
        timer_a_enabled);
      break;
    /* control timer b (0xF) */
    case CRB:
      /* 0b00000001 */
      if (timer_b_enabled) data = SET_BIT(data,ENABLE_TIMER);
      /* 0b00000010 */
      if (timer_b_portb_out) data = SET_BIT(data,PORTBx_TIMER);
      /* 0b00000100 */
      if (timer_b_output_mode) data = SET_BIT(data,TOGGLED_PORTBx);
      /* 0b00001000 */
      if (timer_b_run_mode) data = SET_BIT(data,ONESHOT_TIMER);
      /* 0b00010000 ~ force load strobe always reads zero */
      /* 0b01100000 */
      if (timer_b_input_mode&TIMERB_MASK) data &= (~(TIMERB_MASK) | (timer_b_input_mode << 5));
      /* 0b10000000 */
      if (timer_b_wrtod_mode) data = SET_BIT(data,TOD_BIT_SETS_ALARM);

      if(log_rw) MOSDBG("[R CIA%d] [CRB] $%02x:%02x [RESULT] 0b%d%d%d%d%d%d%d%d\n",
        (is_cia2 ? 2 : 1),
        CRB,data,
        timer_b_wrtod_mode,
        ((timer_b_input_mode&0b10)>>1),
        (timer_b_input_mode&0b01),
        timer_b_force_load,
        timer_b_run_mode,
        timer_b_output_mode,
        timer_b_portb_out,
        timer_b_enabled);
      break;
  }
  r_shadow[reg] = data;
  return data;
}

/**
 * @brief Cia1/Cia2 register write acccess
 *
 * @param reg
 * @param value
 */
void __us_not_in_flash_func(write_register) mos6526::write_register(uint8_t reg, uint8_t value)
{
  w_shadow[reg] = value;
  switch(reg) {
    /* data port a (0x0), keyboard matrix cols and joystick #2 */
    case PRA:
      _pra = value;
      if _MOS_UNLIKELY (is_cia2) {
        /**
         *  Cia2 PRA bit 0 and 1 (0b00000011) hold the VIC base address
         *  Bits are read inverted
         *
         *  0b00, 0: (bank 3) $c000/$ffff, 49152->65535
         *  0b01, 1: (bank 2) $8000/$bfff, 32768->49151
         *  0b10, 2: (bank 1) $4000/$7fff, 16384->32767
         *  0b11, 3: (bank 1) $0000/$3fff, 0->16383 (the default)
         */
        vic_base_addr = ((((~_pra)&0x3) << 0xeu) & 0xffff);
      }
      break;
    /* data port b (0x1), keyboard matrix rows and joystick #1 */
    case PRB:
      _prb = value;
      break;
    /* data direction port a (0x2) */
    case DDRA:
      _ddra = value;
      break;
    /* data direction port b (0x3) */
    case DDRB:
      _ddrb = value;
      break;
    /* timer a low byte (0x4) */
    case TAL: /* latch */
      timer_a_prescaler &= 0xff00;
      timer_a_prescaler |= value;
      break;
    /* timer a high byte (0x5) */
    case TAH: /* latch */
      timer_a_prescaler &= 0x00ff;
      timer_a_prescaler |= value << 8;
      if(!timer_a_enabled) { /* If timer stopped */
        /* Force load timer latch into counter */
        timer_a_counter = timer_a_prescaler;
      }

      if(log_rw) MOSDBG("[W CIA%d] [TAH] [TA]%d [P]%04x(%d) [C]%04x(%d)\n",
        (is_cia2 ? 2 : 1),
        timer_a_enabled,
        timer_a_prescaler,
        timer_a_prescaler,
        timer_a_counter,
        timer_a_counter);
      break;
    /* timer b low byte (0x6) */
    case TBL: /* latch */
      timer_b_prescaler &= 0xff00;
      timer_b_prescaler |= value;
      break;
    /* timer b high byte (0x7) */
    case TBH: /* latch */
      timer_b_prescaler &= 0x00ff;
      timer_b_prescaler |= value << 8;
      if(!timer_b_enabled) { /* If timer stopped */
        /* Force load timer latch into counter */
        timer_b_counter = timer_b_prescaler;
      }

      if(log_rw) MOSDBG("[W CIA%d] [TBH] [TB]%d [P]%04x(%d) [C]%04x(%d)\n",
        (is_cia2 ? 2 : 1),
        timer_b_enabled,
        timer_b_prescaler,
        timer_b_prescaler,
        timer_b_counter,
        timer_b_counter);
      break;
    /* RTC 1/10s (0x8) */
    case TODTEN:
      tod_tenths = (value & 0xfu);
      tod_running = (!tod_running ? true : tod_running);
      break;
    /* RTC seconds (0x9) */
    case TODSEC:
      tod_seconds = (value & 0x7fu);
      break;
    /* RTC minutes (0xA) */
    case TODMIN:
      tod_minutes = (value & 0x7fu);
      break;
    /* RTC hours (0xB) */
    case TODHR:
      value &= 0x9fu;
      if (((value & 0x1fu) == 0x12u) && !timer_a_is_50hz) {
        value ^= 0x80u; /* Flip AM/PM on hour 12 if timer is 60Hz */
      }
      tod_hours = value;
      tod_running = false;
      break;
    /* shift serial (0xC) */
    case SDR:
      _sdr = value;
      break;
    /* interrupt control and status (0xD) */
    case IMR:
      /**
      * if bit 7 is set, enable selected mask of
      * interrupts, else disable them
      */
      if (ISSET_BIT(value,pSetClearIRQ)) {
        // irq_enabled = true;
        if (ISSET_BIT(value,pTimerA)) timer_a_irq_enabled = true; /* 0x01 */
        if (ISSET_BIT(value,pTimerB)) timer_b_irq_enabled = true; /* 0x02 */
        if (ISSET_BIT(value,pAlarm))  alarm_irq_enabled = true;    /* 0x04 */
        if (ISSET_BIT(value,pFlag))   flag_irq_enabled = true;     /* 0x10 */
      } else {
        // irq_enabled = false;
        if (ISSET_BIT(value,pTimerA)) timer_a_irq_enabled = false; /* 0x01 */
        if (ISSET_BIT(value,pTimerB)) timer_b_irq_enabled = false; /* 0x02 */
        if (ISSET_BIT(value,pAlarm))  alarm_irq_enabled = false;    /* 0x04 */
        if (ISSET_BIT(value,pFlag))   flag_irq_enabled = false;     /* 0x10 */
      }

      if(log_rw) MOSDBG("[W CIA%d] [IMR] $%02x:%02x [RESULT] 0b%d%d%d%d%d%d%d%d\n",
        (is_cia2 ? 2 : 1),
        IMR,value,
        ISSET_BIT(value,pSetClearIRQ),0,0,
        flag_irq_enabled,
        0,
        alarm_irq_enabled,
        timer_b_irq_enabled,
        timer_a_irq_enabled);
      break;
    /* control timer a (0xE) */
    case CRA:
      /* 0b00010000 */
      if (!(value & FORCELOAD_STROBE) /* forceload is 0 in value */
        && timer_a_force_load) { /* but already set */
        value |= FORCELOAD_STROBE; /* set in value */
      }
      /* 0b00000001 */
      if (ISSET_BIT(value,ENABLE_TIMER)) timer_a_enabled = pStartTimer; else timer_a_enabled = pStopTimer;
      /* 0b00000010 */
      if (ISSET_BIT(value,PORTBx_TIMER)) timer_a_portb_out = pTimerOnPBx; else timer_a_portb_out = pNormalOperation;
      /* 0b00000100 */
      if (ISSET_BIT(value,TOGGLED_PORTBx)) timer_a_output_mode = pToggle; else timer_a_output_mode = pPulse;
      /* 0b00001000 */
      if (ISSET_BIT(value,ONESHOT_TIMER)) timer_a_run_mode = pModeOneShot; else timer_a_run_mode = pModeContinuous;
      /* 0b00010000 */
      if (ISSET_BIT(value,FORCELOAD_STROBE)) timer_a_force_load = true; else timer_a_force_load = false;
      /* 0b00100000 */
      if (ISSET_BIT(value,TIMER_FROM_CNT)) timer_a_input_mode = pModeCNT; else timer_a_input_mode = pModePHI2;
      /* 0b01000000 */
      if (ISSET_BIT(value,SERIALPORT_IS_OUTPUT)) timer_a_sp_mode = pOutput; else timer_a_sp_mode = pInput;
      /* 0b10000000 */
      if (ISSET_BIT(value,TIMEOFDAY_50Hz)) timer_a_is_50hz = p50Hz; else timer_a_is_50hz = p60Hz;

      if(log_rw) MOSDBG("[W CIA%d] [CRA] $%02x:%02x [RESULT] 0b%d%d%d%d%d%d%d%d\n",
        (is_cia2 ? 2 : 1),
        CRA,value,
        timer_a_is_50hz,
        timer_a_sp_mode,
        timer_a_input_mode,
        timer_a_force_load,
        timer_a_run_mode,
        timer_a_output_mode,
        timer_a_portb_out,
        timer_a_enabled);
      break;
    /* control timer b (0xF) */
    case CRB:
      /* 0b00010000 */
      if (!(value & FORCELOAD_STROBE) /* forceload is 0 in value */
        && timer_b_force_load) { /* but already set */
        value |= FORCELOAD_STROBE; /* set in value */
      }
      /* 0b00000001 */
      if (ISSET_BIT(value,ENABLE_TIMER)) timer_b_enabled = pStartTimer; else timer_b_enabled = pStopTimer;
      /* 0b00000010 */
      if (ISSET_BIT(value,PORTBx_TIMER)) timer_b_portb_out = pTimerOnPBx; else timer_b_portb_out = pNormalOperation;
      /* 0b00000100 */
      if (ISSET_BIT(value,TOGGLED_PORTBx)) timer_b_output_mode = pToggle; else timer_b_output_mode = pPulse;
      /* 0b00001000 */
      if (ISSET_BIT(value,ONESHOT_TIMER)) timer_b_run_mode = pModeOneShot; else timer_b_run_mode = pModeContinuous;
      /* 0b00010000 */
      if (ISSET_BIT(value,FORCELOAD_STROBE)) timer_b_force_load = true; else timer_b_force_load = false;

      /* 0b00100000 */
      if ((value&TIMERB_MASK) == TIMERB_FROM_CNT) timer_b_input_mode = pModeCNT; /* 0x20 */
      /* 0b01000000 */
      else if ((value&TIMERB_MASK) == TIMERB_FROM_TIMERA) timer_b_input_mode = pModeTimerA; /* 0x40 */
      /* 0b01100000 */
      else if ((value&TIMERB_MASK) == TIMERB_FROM_TIMERA_AND_CNT) timer_b_input_mode = pModeTimerACNT; /* 0x60 */
      /* 0b00000000 */
      else if ((value&TIMERB_MASK) == TIMERB_FROM_CPUCLK) timer_b_input_mode = pModePHI2; /* 0x00 */

      /* 0b10000000 */
      if (ISSET_BIT(value,TOD_BIT_SETS_ALARM)) timer_b_wrtod_mode = pWriteAlarm; else timer_b_wrtod_mode = pWriteTOD;

      if(log_rw) MOSDBG("[W CIA%d] [CRB] $%02x:%02x [RESULT] 0b%d%d%d%d%d%d%d%d\n",
        (is_cia2 ? 2 : 1),
        CRB,value,
        timer_b_wrtod_mode,
        ((timer_b_input_mode&0b10)>>1),
        (timer_b_input_mode&0b01),
        timer_b_force_load,
        timer_b_run_mode,
        timer_b_output_mode,
        timer_b_portb_out,
        timer_b_enabled);
      break;
  }

}

/**
 * @brief register keyboard press/depress input
 * @param a col
 * @param b row
 *
 * TODO: Make sure that only enabled lines(bits) are set according to DDRA/DDRB
 */
void __us_not_in_flash_func(write_prab_bits) mos6526::write_prab_bits(uint8_t a, uint8_t b, bool state)
{
  uint8_t mask;
  if (state) { /* ON */
    /* Set bus */
    _pra &= ~(1<<a);       /* PRA ~ COL */
    _prb &= ~(1<<b);       /* PRB ~ ROW */
    /* Set keyboard matrix */
    mask = ~(1 << b);      /* PRB is inverted mask */
    _kb_matrix[a] &= mask; /* PRA */
  } else { /* OFF */
    /* Set bus */
    _pra |= (1<<a);        /* PRA ~ COL */
    _prb |= (1<<b);        /* PRB ~ ROW */
    /* Set keyboard matrix */
    mask = (1 << b);       /* PRB is inverted mask */
    _kb_matrix[a] |= mask; /* PRA */
  }
  /* MOSDBG("[CIA1] Keypress %s row %d col %d, pra %2x prb %2x mask %2x\n",
    (state?"On":"Off"),a,b,_pra,_prb,mask
  ); */
  return;
}

/**
 * @brief read vic_base_address from PRA
 * Used from MMU->vic_read_byte()
 *
 * @return uint_least16_t
 */
uint_least16_t __us_not_in_flash_func(vic_base_address) mos6526::vic_base_address(void)
{
  return vic_base_addr;
}

/**
 * @brief Cia1/Cia2 Timer A emulation
 *
 */
void __us_not_in_flash_func(timer_a) mos6526::timer_a(void)
{
  /* Check for force load latch requested */
  if _MOS_UNLIKELY (timer_a_force_load) {
    timer_a_counter = timer_a_prescaler; /* reload timer */
  }
  /* Check for timer A enabled*/
  if _MOS_LIKELY (timer_a_enabled) {

    /* Check for count Phi2 enabled */
    if ((timer_a_input_mode == pModePHI2)
        /* Checp for CNT enabled */
        || (timer_a_input_mode == pModeCNT)) { /* Used by So-Phisticated_III_loader.sid */

      timer_a_counter -= (cia_cpu_clock - prev_cia_cpu_clock); /* Update counter */
      /* Check for counter underflow */
      if _MOS_UNLIKELY (timer_a_counter >= 0xffff) {
        timer_a_underflow = true;
        timer_a_counter = timer_a_prescaler; /* reload timer */

        // if (timer_a_portb_out) {
        //   if (_prb & 0x40) {
        //     _prb &= ~(1<<6); /* Unset PB7 in portB */
        //   } else {
        //     _prb |= (1<<6); /* Set PB7 in portB */
        //   }
        // }

        /* Generate interrupt if write mask allows */
        if (timer_a_irq_enabled) {
          timer_a_irq_triggered = irq_triggered = true; /* Set interrupt bits in read ICR */

          if(is_cia2) {
            cpu->nmi(is_cia2); /* Trigger interrupt */
          } else {
            cpu->irq(is_cia2); /* Trigger interrupt */
          }

        }
        /* If one-shot is enabled */
        if (timer_a_run_mode == pModeOneShot) {
          timer_a_enabled = pStopTimer; /* Disable timer A in write register*/
        }

      }
    }
  }
  timer_a_force_load = false;   /* Reset timer A strobe bit */
  timer_a_underflow = false; /* Reset underflow */
}

/**
 * @brief Cia1/Cia2 Timer B emulation
 *
 */
void __us_not_in_flash_func(timer_b) mos6526::timer_b(void)
{
  /* Check for force load latch requested */
  if _MOS_UNLIKELY (timer_b_force_load) {
    timer_b_counter = timer_b_prescaler; /* reload timer */
  }
  /* Check for timer B enabled */
  if _MOS_LIKELY (timer_b_enabled) {
    /* Check for count Phi2 enabled */
    if _MOS_LIKELY ((timer_b_input_mode == pModePHI2)
                    || (timer_b_input_mode == pModeCNT)) {

      timer_b_counter -= (cia_cpu_clock - prev_cia_cpu_clock); /* Update counter */
      /* Check for counter underflow */
      if _MOS_UNLIKELY (timer_b_counter >= 0xffff) {
        timer_b_underflow = true;
        timer_b_counter = timer_b_prescaler; /* reload timer */

        // if (timer_b_portb_out) {
        //   if (_prb & 0x40) {
        //     _prb &= ~(1<<6); /* Unset PB7 in portB */
        //   } else {
        //     _prb |= (1<<6); /* Set PB7 in portB */
        //   }
        // }

        /* Generate interrupt if write mask allows */
        if (timer_b_irq_enabled) {
          timer_b_irq_triggered = irq_triggered = true; /* Set interrupt bits in read ICR */
          if(is_cia2) {
            cpu->nmi(is_cia2); /* Trigger interrupt */
          } else {
            cpu->irq(is_cia2); /* Trigger interrupt */
          }
        }

        if (timer_b_run_mode == pModeOneShot) { /* If one-shot is enabled */
          timer_b_enabled = pStopTimer; /* Disable timer B in write register*/
        }
      }
    }
    /* Check for count TimerA enabled */
    if _MOS_UNLIKELY ((timer_b_input_mode == pModeTimerA) /* Used by Graphixmania_2_part_6.sid / Demi-Demo_4.sid */
                      || (timer_b_input_mode == pModeTimerACNT)) {

      timer_b_counter = timer_a_counter; /* Update counter */

      /* Check for counter underflow */
      if (timer_a_counter > prev_timer_a_counter) { timer_b_underflow = true; }
      /* counter underflow */
      if _MOS_UNLIKELY (timer_b_underflow) {
        timer_b_counter = timer_a_counter;
        /* Generate interrupt if write mask allows */
        if (timer_b_irq_enabled) {
          timer_b_irq_triggered = irq_triggered = true; /* Set interrupt bit in read ICR */
          if(is_cia2) {
            cpu->nmi(is_cia2); /* Trigger interrupt */
          } else {
            cpu->irq(is_cia2); /* Trigger interrupt */
          }
        }
        /* If one-shot is enabled */
        if (timer_b_run_mode == pModeOneShot) {
          timer_b_enabled = pStopTimer; /* Disable timer B in write register*/
        }
      }
      prev_timer_a_counter = timer_a_counter;
    }
  }
  timer_b_force_load = false; /* Reset timer B strobe bit */
  timer_b_underflow = false; /* Reset underflow */
}

/**
 * @brief Fake time of day timer
 * test sidtunes
 * /MUSICIANS/K/Kawasaki_Ryo/Kawasaki_Synthesizer_Demo.sid
 * /MUSICIANS/M/Merman/Traffic.sid
 */
void __us_not_in_flash_func(tod) mos6526::tod(void)
{
  if (tod_running) {

    tod_tenths++ & 0x0fu;
    if(tod_tenths == 0x0au) {
      tod_tenths = 0;

      tod_seconds++;
      if((tod_seconds & 0x0F) == 0x0A) {
        tod_seconds += 6;
        if(tod_seconds == 0x60) {
          tod_seconds = 0;

        /* Time of Day minutes and hours are not used in any SID tunes */

        //   tod_minutes++;
        //   if((tod_minutes & 0x0F) == 0x0A) {
        //     tod_minutes += 6;
        //     if(tod_minutes == 0x60) {
        //       tod_minutes = 0;

        //       tod_hours++;
        //       if((tod_hours & 0x1F) == 0x0A) {
        //         tod_hours += 6;
        //       } else {
        //         if((tod_hours & 0x1F) == 0x13) {
        //           tod_hours &= 0x8F;
        //           tod_hours ^=0x80;
        //         }
        //       }
        //     }
        //  }
        }
      }
    }
  }
  /* MOSDBG("[CIA%d] TOD(%d|%d): $%02x%02x%02x%02x\n",
    (is_cia2 ? 2 : 1),tod_latched,tod_running,tod_hours,tod_minutes,tod_seconds,tod_tenths); */
}

/**
 * @brief: emulate a single CIA timer run
 * NOTE: not single cycle exact
 */
bool __us_not_in_flash_func(emulate) mos6526::emulate(void)
{
  // if _MOS_UNLIKELY (!check_cyclefn()) {
  //   D("[CIA] ERROR! Cannot emulate without CPU cycles!\n");
  //   return false;
  // }

  cia_cpu_clock = cpu->cycles();

  timer_a();
  timer_b();

  tod();

  prev_cia_cpu_clock = cia_cpu_clock;
  return true;
}

/**
 * @brief debug logging of registers PRA and PRB
 *
 */
void mos6526::dump_prab(void)
{
  MOSDBG("#%6u [CIA%d][PRA:%02x|DDRA:%02x|PRB:%02x|DDRB:%02x]",
    cpu->cycles(),
    (is_cia2 ? 2 : 1),
    _pra,_ddra,_prb,_ddrb);
}

/**
 * @brief debug logging of registers ICR/IMR, CRA and CRB
 *
 */
void mos6526::dump_irqs(void)
{
  MOSDBG("CIA%d:%02x%02x|%02x%02x|%02x%02x",
    (is_cia2 ? 2 : 1),
    r_shadow[ICR],w_shadow[IMR],
    r_shadow[CRA],w_shadow[CRA],
    r_shadow[CRB],w_shadow[CRB]);
  return;
}

/**
 * @brief debug logging of the CIA timers
 *
 */
void mos6526::dump_timers(void)
{
  MOSDBG("[CIA%d][TOD%2x%2x%2x%2x][A|P:%04x:%5uC:%04x:%5d][BP:%04x:%5uC:%04x:%5d][IMR%02x][ICR%02x][CRA%02x][CRB%02x] ",
    (is_cia2 ? 2 : 1),
    tod_hours,tod_minutes,tod_seconds,tod_tenths,
    timer_a_prescaler,
    timer_a_prescaler,
    timer_a_counter,
    timer_a_counter,
    timer_b_prescaler,
    timer_b_prescaler,
    timer_b_counter,
    timer_b_counter,
    ((flag_irq_enabled<<3)|(alarm_irq_enabled<<2)|(timer_b_irq_enabled<<1)|timer_a_irq_enabled),
    ((flag_irq_triggered<<3)|(alarm_irq_triggered<<2)|(timer_b_irq_triggered<<1)|timer_a_irq_triggered),
    ((timer_a_is_50hz<<7)
     |(timer_a_sp_mode<<6)
     |(timer_a_input_mode<<5)
     |(timer_a_force_load<<4)
     |(timer_a_run_mode<<3)
     |(timer_a_output_mode<<2)
     |(timer_a_portb_out<<1)
     |timer_a_enabled),
    ((timer_b_wrtod_mode<<7)
     |(timer_b_input_mode<<5)
     |(timer_b_force_load<<4)
     |(timer_b_run_mode<<3)
     |(timer_b_output_mode<<2)
     |(timer_b_portb_out<<1)
     |timer_b_enabled)
    );
  return;
}
