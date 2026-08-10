/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * psiddrv_install.h
 * Installing the PSID/RSID driver into the machine's memory.
 *
 * The driver itself (psiddrv.a65, assembled to psiddrv.o65 and turned into
 * psiddrv.h) and the o65 relocator (reloc65.c) come from old player, which
 * took them from VICE. They are known good and were not worth rewriting; what
 * is written here is the install sequence around them.
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
 *
 */

#pragma once
#ifndef _US_DRIVER_PSIDDRV_INSTALL_H_
#define _US_DRIVER_PSIDDRV_INSTALL_H_

#include "ram.h"
#include "sidfile.h"
#include "types.h"

namespace usbsid {

/* The driver image starts with nine bytes of workspace, then its entry jump.
 * The CBM80 vector the driver installs for itself points here, so this is
 * also where a machine autostarting from the cartridge vector would land. */
constexpr addr_t kPsidDrvEntryOffset = 9;

/* The rest of the layout, verified against old player ~ src/vsidpsid.cpp:
 *   +0..8    workspace
 *   +9..11   entry jump          (its next_prev_tune calls this jmp_addr)
 *   +12..20  the CBM80 block the driver copies to $8000  (bck_addr)
 *   +21..    the parameter block (drv_addr), whose first byte is the song
 *   +$89     the driver's "load another song" entry       (nxt_addr)
 */
constexpr addr_t kPsidDrvCbm80Offset = 12;
constexpr addr_t kPsidDrvParamOffset = 21;
constexpr addr_t kPsidDrvNextSongOffset = 0x89;

/**
 * @brief Put the tune and the driver into memory.
 *
 * @param ram      the machine's RAM, written directly (this is a load, not
 *                 something the CPU does)
 * @param tune     a parsed SID file
 * @param song     the subtune to start, 1 based, 0 means the file's default
 * @param is_pal   which video standard the machine is running
 * @param reloc_addr_out  where the driver ended up
 * @return false when the driver could not be relocated
 */
bool psiddrv_install(Ram & ram, const SidFile & tune, uint16_t song,
                     bool is_pal, addr_t & reloc_addr_out);

/**
 * @brief Point the driver at a different subtune without reloading anything.
 */
void psiddrv_set_song(Ram & ram, addr_t reloc_addr, uint16_t song, bool is_pal);

} /* namespace usbsid */

#endif /* _US_DRIVER_PSIDDRV_INSTALL_H_ */
