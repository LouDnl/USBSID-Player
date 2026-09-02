/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * cynthcart_embedded.h
 * The three entry points the firmware calls to run Cynthcart through the
 * player instead of the standalone emulator (ONBOARD_EMULATOR). Same names
 * and signatures as repo/lib/emulator/emudore_emulator.h on purpose, so
 * usbsid.c and midi.c need only pick which header to include.
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
 */

#pragma once
#ifndef _US_CART_CYNTHCART_EMBEDDED_H_
#define _US_CART_CYNTHCART_EMBEDDED_H_

#ifdef __cplusplus
extern "C" {
#endif

void start_cynthcart(void);
void stop_cynthcart(void);
unsigned int run_cynthcart(void);

#ifdef __cplusplus
}
#endif

#endif /* _US_CART_CYNTHCART_EMBEDDED_H_ */
