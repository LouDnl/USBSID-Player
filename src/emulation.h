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
 * emulation.h
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
#ifndef _US_EMULATION_H
#define _US_EMULATION_H


#include <constants.h>
#include <sidfile.h>
#include <c64util.h>
#include <wrappers.h>

#include <mos6510_cpu.h>
#include <mos6526_cia.h>
#include <mos6581_8580_sid.h>
#include <mos6560_6561_vic.h>
#include <mos906114_pla.h>
#include <mmu.h>

/* C64 Variables */
mos6510 *Cpu;
mos6526 *Cia1;
mos6526 *Cia2;
mos6560_6561 *Vic;
mos906114 *Pla;
mos6581_8580 *SID;
mmu *MMU;

/* Initialize variables */
int pcbversion = -1;
int fmoplsidno = -1;
uint16_t sidone = 0;
uint16_t sidtwo = 0;
uint16_t sidthree = 0;
uint16_t sidfour = 0;
int sidssockone = 0, sidssocktwo = 0;
int sockonesidone = 0, sockonesidtwo = 0;
int socktwosidone = 0, socktwosidtwo = 0;
bool forcesockettwo = false; /* force play on socket two */
int sidcount = 1;
int sidno = 0;

#define CIA1_ADDRESS 0xDC00
#define CIA2_ADDRESS 0xDD00
/* Emulation variables */
#if DESKTOP
volatile sig_atomic_t stop = false;
volatile sig_atomic_t playing = false;
volatile sig_atomic_t paused = false;
volatile sig_atomic_t vsidpsid = false;
#elif EMBEDDED
volatile bool stop = false;
volatile bool playing = false;
volatile bool paused = false;
volatile bool vsidpsid = false;
#endif

/* Emulation variables */
bool enable_r2 = false;
bool log_instructions = false;
bool log_timers = false;
bool log_pla = false;
bool log_readwrites = false;
bool log_romrw = false;
bool log_vicrw = false;
bool log_vicrrw = false;
bool log_cia1rw = false;
bool log_cia2rw = false;
bool log_sidrw = false;


#endif /* _US_EMULATION_H */
