/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * prgfile.h
 * C64 program file: 2 bytes load address then the data
 * Also reads the PC64 .p00 container (26 byte header with the C64 name)
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
#ifndef _US_FILE_PRGFILE_H_
#define _US_FILE_PRGFILE_H_

#include "types.h"

namespace usbsid {

/* Where BASIC programs load, and where the KERNAL expects them */
constexpr addr_t kBasicStart = 0x0801;

/** @brief A parsed program file, data points into the caller's bytes */
struct PrgFile {
  bool valid = false;
  bool is_p00 = false;          /* came in a PC64 container */

  addr_t load_addr = 0;
  addr_t end_addr = 0;          /* the last byte, inclusive */

  const data_t * data = nullptr;
  size_t data_size = 0;

  /* Address a BASIC startup line SYSes to */
  bool has_sys_stub = false;
  addr_t sys_addr = 0;

  char name[17] = { 0 };        /* from the container */

  /** @brief True if this loads where BASIC programs go */
  bool is_basic(void) const { return load_addr == kBasicStart; }
};

/**
 * @brief Parse a PRG or P00
 *
 * @param bytes Whole file
 * @param len Length of the file
 * @param out Filled in on success, out.valid tells
 */
bool prgfile_parse(const data_t * bytes, size_t len, PrgFile & out);

} /* namespace usbsid */

#endif /* _US_FILE_PRGFILE_H_ */
