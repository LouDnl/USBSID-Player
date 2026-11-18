/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * c64app_embedded.h
 * Entry points the firmware calls to run an embedded C64 MIDI application
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
 */

#pragma once
#ifndef _US_CART_C64APP_EMBEDDED_H_
#define _US_CART_C64APP_EMBEDDED_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Values are persisted in the firmware config, append only */
typedef enum {
  C64APP_CYNTHCART = 0,
  C64APP_SIDSYNTH  = 1,
  C64APP_COUNT
} c64app_t;

/* Out of range ids start Cynthcart */
void start_c64app(int app);
void stop_c64app(void);
unsigned int run_c64app(void);
/* App started last, C64APP_COUNT when none runs */
int running_c64app(void);

#ifdef __cplusplus
}
#endif

#endif /* _US_CART_C64APP_EMBEDDED_H_ */
