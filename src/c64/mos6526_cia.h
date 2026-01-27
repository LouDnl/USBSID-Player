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
 * mos6526_cia.h
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

#ifndef _MOS6526_H
#define _MOS6526_H

#include <cstdint>
#include <types.h>


class mos6510;


/* MOS 6526 CIA */
class mos6526
{
  private:
    /* Glue */
    mos6510 * cpu;
    /* Init */
    uint_least16_t cia_address;

    /* Debugging */
    uint_least8_t w_shadow[0xf];
    uint_least8_t r_shadow[0xf];

    /* Variables */
    CPUCLOCK cia_cpu_clock;
    CPUCLOCK prev_cia_cpu_clock;
    bool log_rw = false;

    /* Private constants */
    enum pTimerMode /* off/on */
    {
      pStopTimer,  /* 0 */
      pStartTimer  /* 1 */
    };

    enum pPortBMode /* off/on */
    {
      pNormalOperation,  /* 0 */
      pTimerOnPBx        /* 1: TimerA = PB6, TimerB = PB7 */
    };

    enum pOutputMode /* off/on */
    {
      pPulse,  /* 0 */
      pToggle  /* 1 */
    };

    enum pRunMode /* off/on */
    {
      pModeContinuous,  /* 0 */
      pModeOneShot      /* 1 */
    };

    enum pSerialPortMode    /* Input/Output */
    {
      pInput,   /* 0 */
      pOutput   /* 1 */
    };

    enum pHzMode    /* 60Hz/50Hz */
    {
      p60Hz,  /* 0 */
      p50Hz   /* 1 */
    };

    enum pTODWrRegs /* TOD/ALARM */
    {
      pWriteTOD,    /* 0 */
      pWriteAlarm   /* 1 */
    };

    enum pInputMode /* bit mask */
    {
      pModePHI2,      /* 0b0 TA / 0b00 TB */
      pModeCNT,       /* 0b1 TA / 0b01 TB */
      pModeTimerA,    /* 0b10 TB */
      pModeTimerACNT  /* 0b11 TB */
    };

    enum pIRQMode /* bit location */
    {
      pTimerA,      /* 0 */
      pTimerB,      /* 1 */
      pAlarm,       /* 2 */
      pSerialPort,  /* 3 */
      pFlag,        /* 4 */
      pNo1_,        /* not used */
      pNo2_,        /* not used */
      pSetClearIRQ  /* 7 */
    };

    enum ControlBitValues
    {
      ENABLE_TIMER               = 0,     /* bit location */
      PORTBx_TIMER               = 1,     /* bit location */
      TOGGLED_PORTBx             = 2,     /* bit location */
      ONESHOT_TIMER              = 3,     /* bit location */
      FORCELOAD_STROBE           = 4,     /* bit location */
      TIMER_FROM_CNT             = 5,     /* bit location */
      SERIALPORT_IS_OUTPUT       = 6,     /* bit location */
      TOD_BIT_SETS_ALARM         = 7,     /* bit location */
      TIMEOFDAY_50Hz             = 7,     /* bit location */

      TIMERA_MASK                = 0x20,  /* bit mask */
      TIMERB_MASK                = 0x60,  /* bit mask */
      TIMERB_FROM_CPUCLK         = 0x00,  /* bit comparison */
      TIMERB_FROM_CNT            = 0x20,  /* bit comparison */
      TIMERB_FROM_TIMERA         = 0x40,  /* bit comparison */
      TIMERB_FROM_TIMERA_AND_CNT = 0x60,  /* bit comparison */

    };

    /* CIA 1 keyboard matrix */
    uint8_t _kb_matrix[8] = { // Active low
      // cols ->
      0b11111111, // rows
      0b11111111, // ↓
      0b11111111,
      0b11111111,
      0b11111111,
      0b11111111,
      0b11111111,
      0b11111111,
    };

    /* Register related */

    /* CIA (bus)PRA & (bus)PRB state inverted */
    uint8_t _pra, _prb;          /* 0x00,0x01 read/write */
    uint8_t _ddra, _ddrb;        /* 0x02,0x03 read/write */
    uint_least16_t vic_base_addr;  /* Cia2 only */

    uint8_t _sdr;                /* 0x0c read/write */

    /* TIMER(uint32_t) to account for 16bit underflow */
    TIMER timer_a_counter;   /* 0x04,0x05 read ~ counter */
    TIMER timer_b_counter;   /* 0x06,0x07 read ~ counter */
    TIMER prev_timer_a_counter;
    TIMER prev_timer_b_counter;
    uint16_t timer_a_prescaler; /* 0x04,0x05 write ~ latch */
    uint16_t timer_b_prescaler; /* 0x06,0x07 write ~ latch */

    bool irq_enabled;           /* _imr & 0b10000000 ~ write */
    bool irq_triggered;         /* _icr & 0b10000000 ~ read */

    bool flag_irq_enabled;      /* _imr & 0b00010000 */
    bool alarm_irq_enabled;     /* _imr & 0b00000100 */
    bool flag_irq_triggered;    /* _icr & 0b00010000 */
    bool alarm_irq_triggered;   /* _icr & 0b00000100 */

    bool timer_a_irq_enabled;   /* _imr & 0b00000001 ~ pIRQMode */
    bool timer_a_irq_triggered; /* _icr & 0b00000001 ~ pIRQMode */
    bool timer_a_enabled;       /* _cra & 0b00000001 ~ resets on underflow in oneshot mode */
    bool timer_a_portb_out;     /* _cra & 0b00000010 ~ pPortBMode */
    bool timer_a_output_mode;   /* _cra & 0b00000100 ~ pOutputMode */
    bool timer_a_run_mode;      /* _cra & 0b00001000 ~ pRunMode */
    bool timer_a_force_load;    /* _cra & 0b00010000 */
    bool timer_a_input_mode;    /* _cra & 0b00100000 ~ pInputMode */
    bool timer_a_sp_mode;       /* _cra & 0b01000000 */
    bool timer_a_is_50hz;       /* _cra & 0b10000000 */
    bool timer_a_underflow;

    bool timer_b_irq_enabled;   /* _imr & 0b00000010 ~ pIRQMode */
    bool timer_b_irq_triggered; /* _icr & 0b00000010 ~ pIRQMode */
    bool timer_b_enabled;       /* _crb & 0b00000001 ~ resets on underflow in oneshot mode */
    bool timer_b_portb_out;     /* _crb & 0b00000010 ~ pPortBMode */
    bool timer_b_output_mode;   /* _crb & 0b00000100 ~ pOutputMode */
    bool timer_b_run_mode;      /* _crb & 0b00001000 ~ pRunMode */
    bool timer_b_force_load;    /* _crb & 0b00010000 */
    uint8_t timer_b_input_mode; /* _crb & 0b01100000 ~ pInputMode (4 modes, so not boolean) */
    bool timer_b_wrtod_mode;    /* _crb & 0b10000000 */
    bool timer_b_underflow;

    bool tod_running; /* Time of day */
    bool tod_latched; /* Time of day latch */
    uint32_t tod_counter;
    uint8_t tod_tenths;
    uint8_t tod_seconds;
    uint8_t tod_minutes;
    uint8_t tod_hours;

    typedef unsigned int (*ReadCycles)(void); /* External cycles reading */

    inline bool _is_cia2(void) { return (cia_address == 0xDD00); };
    bool is_cia2;

  public:
    mos6526(uint_least16_t base_address);
    ~mos6526(void);

    void glue_c64(mos6510 * _cpu);

    void set_cycle_fn(ReadCycles c);
    ReadCycles cpu_cycles = nullptr;
    bool check_cyclefn(void) { return (cpu_cycles != nullptr); };

    void reset(void);

    uint8_t read_register(uint8_t r);
    void write_register(uint8_t r, uint8_t v);
    void write_prab_bits(uint8_t a, uint8_t b, bool state);

    uint16_t ta_prescaler(void) { return timer_a_prescaler; };
    uint_least16_t vic_base_address(void);

    void timer_a(void);
    void timer_b(void);
    void tod(void);
    bool emulate(void);

    void dump_prab(void);
    void dump_irqs(void);
    void dump_timers(void);

    enum CIA1Registers
    {
      PRA     = 0x0, /* Keyboard (R/W) COL, Joystick, Lightpen, Paddles */
      PRB     = 0x1, /* Keyboard (R/W) ROW, Joystick, Timer A, Timer B */
      DDRA    = 0x2, /* Datadirection Port A */
      DDRB    = 0x3, /* Datadirection Port B */
      TAL     = 0x4, /* Timer A Low Byte */
      TAH     = 0x5, /* Timer A High Byte */
      TBL     = 0x6, /* Timer B Low Byte */
      TBH     = 0x7, /* Timer A High Byte */
      TODTEN  = 0x8, /* RTC 1/10s */
      TODSEC  = 0x9, /* RTC sec */
      TODMIN  = 0xA, /* RTC min */
      TODHR   = 0xB, /* RTC hr */
      SDR     = 0xC, /* Serial shift register */
      IMR     = 0xD, /* Interrupt mask register ~ write */
      ICR     = 0xD, /* Interrupt control register ~ read */
      CRA     = 0xE, /* Control Timer A */
      CRB     = 0xF, /* Control Timer B */
    };

    enum InterruptBitVal
    { /* (Read or Write operation determines which one:) */
      INTERRUPT_HAPPENED = 0x80,
      SET_OR_CLEAR_FLAGS = 0x80,
      /* flags/masks of interrupt-sources */
      FLAGn      = 0x10,
      SERIALPORT = 0x08,
      ALARM      = 0x04,
      TIMERB     = 0x02,
      TIMERA     = 0x01
    };

};


#endif /* _MOS6526_H */
