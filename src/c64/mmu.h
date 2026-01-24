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
 * mmu.h
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
#ifndef _US_MMU_H_
#define _US_MMU_H_

#include <cstdint>

class mos6510;
class mos6526;
class mos6560_6561;
class mos6581_8580;
class mos906114;

/**
 * @brief C64 Memory Management Unit
 */
class mmu
{
  public:
    mmu(void);
    ~mmu(void);

    /* Debugging variables */
    bool log_instructions = false;
    bool log_pla = false;
    bool log_readwrites = false;
    bool log_romrw = false;
    bool log_vicrw = false;
    bool log_vicrrw = false;
    bool log_cia1rw = false;
    bool log_cia2rw = false;

  private:
    /* Glue */
    mos6510 * cpu;
    mos906114 * pla;
    mos6560_6561 * vic;
    mos6526 * cia1;
    mos6526 * cia2;
    mos6581_8580 * sid;

    const unsigned char * basic;
    const unsigned char * chargen;
    const unsigned char * kernal;

    uint_fast8_t bsc, crg, krn;

    inline uint8_t read_sid(uint16_t addr);
    inline void write_sid(uint16_t addr, uint8_t data);
    inline uint8_t read_cia(uint_least16_t addr);
    inline void write_cia(uint_least16_t addr, uint8_t data);
    inline uint8_t read_vic(uint_least16_t addr);
    inline void write_vic(uint_least16_t addr, uint8_t data);
    inline uint8_t rom_read_byte(uint16_t addr, char rom);

  public:
    void glue_c64(mos6510 *_cpu, mos906114 *_pla, mos6560_6561 *_vic, mos6526 *_cia1, mos6526 *_cia2, mos6581_8580 *_sid);

    uint8_t vic_read_byte(uint16_t addr);
    uint8_t read_byte(uint16_t addr);
    void write_byte(uint16_t addr, uint8_t data);

    uint8_t dma_read_ram(uint16_t addr);
    void dma_write_ram(uint16_t addr, uint8_t data);

};

#endif /* _US_MMU_H_ */
