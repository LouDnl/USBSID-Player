/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * psiddrv_install.h
 * Install the PSID/RSID driver into the machine memory
 * The driver (psiddrv.a65 > psiddrv.o65 > psiddrv.h) and the o65
 * relocator (reloc65.c) come from VICE and are written by
 * Dag Lem  <resid@nimrod.no>
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
 *
 */

#pragma once
#ifndef _US_DRIVER_PSIDDRV_INSTALL_H_
#define _US_DRIVER_PSIDDRV_INSTALL_H_

#include "ram.h"
#include "sidfile.h"
#include "types.h"

namespace usbsid {

/* Driver image starts with nine bytes of workspace, then the entry jump.
 * The CBM80 vector it installs points to this address */
constexpr addr_t kPsidDrvEntryOffset = 9;

/* Rest of the layout:
 *   +0..8    workspace
 *   +9..11   entry jump (jmp_addr)
 *   +12..20  CBM80 block copied to $8000 (bck_addr)
 *   +21..    parameter block (drv_addr), first byte is the song
 *   +$89     "load another song" entry (nxt_addr)
 */
constexpr addr_t kPsidDrvCbm80Offset = 12;
constexpr addr_t kPsidDrvParamOffset = 21;
constexpr addr_t kPsidDrvNextSongOffset = 0x89;

/**
 * @brief Put the tune and the driver into memory, return false if the driver could not be relocated
 *
 * @param ram Machine RAM, written directly
 * @param tune Parsed SID file
 * @param song Subtune to start, 1 based, 0 is the file default
 * @param is_pal Video standard of the machine
 * @param reloc_addr_out Where the driver ended up
 */
bool psiddrv_install(Ram & ram, const SidFile & tune, uint16_t song,
                     bool is_pal, addr_t & reloc_addr_out);

/** @brief Point the driver at a different subtune without reloading */
void psiddrv_set_song(Ram & ram, addr_t reloc_addr, uint16_t song, bool is_pal);

} /* namespace usbsid */

#endif /* _US_DRIVER_PSIDDRV_INSTALL_H_ */
