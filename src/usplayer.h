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
 * usplayer.h
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

#ifndef _USPLAYER_H_
#define _USPLAYER_H_
#pragma once

#ifdef __cplusplus
  extern "C" {
#endif


/* externs from usplayer.cpp */
extern void load_prg(uint8_t * binary_, size_t binsize_, bool loop);
extern void load_sidtune(uint8_t * sidfile, int sidfilesize, char subt);
extern void init_sidplayer(void);
extern void start_sidplayer(bool loop);
extern void loop_sidplayer(void);
extern bool stop_sidplayer(void);
extern void next_subtune(void);
extern void previous_subtune(void);


#ifdef __cplusplus
  }
#endif

#endif /* _USPLAYER_H_ */
