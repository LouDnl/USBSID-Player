/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * cynthcart_embedded.h
 * Older Cynthcart entry points, forwarded to c64app_embedded.h
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
#ifndef _US_CART_CYNTHCART_EMBEDDED_H_
#define _US_CART_CYNTHCART_EMBEDDED_H_

#include "c64app_embedded.h"

/* Older firmware entry points, Cynthcart only */
static inline void start_cynthcart(void) { start_c64app(C64APP_CYNTHCART); }
static inline void stop_cynthcart(void) { stop_c64app(); }
static inline unsigned int run_cynthcart(void) { return run_c64app(); }

#endif /* _US_CART_CYNTHCART_EMBEDDED_H_ */
