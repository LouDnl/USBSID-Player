/**
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
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 *
 * @file pico_timer.c
 * @author LouD (usbsid-player@loudai.nl)
 * @brief This file contains wrappers around rp2040/rp2350 SDK functions
 * @version 0.1
 * @date 2026-01-24
 *
 * @copyright Copyright (c) 2026
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

#ifdef __cplusplus
 extern "C" {
#endif

#if EMBEDDED

#include <pico.h>
#include <pico/types.h>
#include <pico/time.h>
#include <hardware/structs/sio.h>
#include <hardware/structs/systick.h>

uint64_t pico_us_since_boot(void)
{
  return to_us_since_boot(get_absolute_time());
}

uint64_t pico_ns_since_boot(void)
{
  return (to_us_since_boot(get_absolute_time()) * 1000);
}

void sleep_ms_emu(uint32_t ms)
{
  sleep_ms(ms); /* Allow for player to stop */
  return;
}

#endif /* EMBEDDED */


#ifdef __cplusplus
 }
#endif
