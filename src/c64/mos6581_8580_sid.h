/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * mos6581_8580_sid.h
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

#pragma once
#ifndef _US_SID_H_
#define _US_SID_H_

#include <cstdint>

#include <types.h>

class mmu;
class mos6510;

/**
 * @brief C64 Sound Interface Device
 */
class mos6581_8580
{
  public:
    mos6581_8580(void);
    ~mos6581_8580(void);

    /* SID related defaults */
    uint8_t sidcount  = 1;
    uint8_t sidno     = 0;
    uint16_t sidone   = 0xd400;
    uint16_t sidtwo   = 0x0000; /* 0xd420 */
    uint16_t sidthree = 0x0000; /* 0xd440 */
    uint16_t sidfour  = 0x0000; /* 0xd460 */
    /* USBSID related variables and defaults */
    uint8_t fmoplsidno = 0;
    uint8_t sidssockone = 1;
    uint8_t sidssocktwo = 0;
    uint8_t sockonesidone = 0;
    uint8_t sockonesidtwo = 0;
    uint8_t socktwosidone = 0;
    uint8_t socktwosidtwo = 0;
    bool forcesockettwo = false;

    bool log_sidrw = false;

  private:
    /* Glue */
    mmu * mmu_;
    mos6510 * cpu;

    /* Clocks */
    uint32_t sid_main_clk   = 0;
    uint32_t flush_main_clk = 0;
    /* Cycles */
    CPUCLOCK s_cyclecount = 0;
    CPUCLOCK w_cyclecount = 0;
    CPUCLOCK r_cyclecount = 0;
    /* IO Banking */
    uint8_t bsc = 0;
    uint8_t crg = 0;
    uint8_t krn = 0;

  public:
    void glue_c64(mmu * _mmu, mos6510 * _cpu);

    bool custom_sidaddr_check(uint16_t addr);
    inline uint8_t sidaddr_translation(uint16_t addr);
    void sid_flush(void);
    unsigned int sid_delay(void);
    uint8_t read_sid(uint16_t addr);
    void write_sid(uint16_t addr, uint8_t data);

    void print_settings(void);
};

#endif /* _US_SID_H_ */
