/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * prgfile.h
 * A C64 program file: two bytes of load address and then the bytes.
 *
 * Also the PC64 container the .p00 extension means, which is the same thing
 * behind a 26 byte header that carries the file's original C64 name. A .p00
 * is what a file transferred off a disk with a PC64 style tool looks like,
 * and there are enough of them about that reading them is worth twenty lines.
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
#ifndef _US_FILE_PRGFILE_H_
#define _US_FILE_PRGFILE_H_

#include "types.h"

namespace usbsid {

/* Where BASIC programs load, and where the KERNAL expects them */
constexpr addr_t kBasicStart = 0x0801;

/**
 * @brief A parsed program file.
 *
 * The payload is not copied, this points into the caller's bytes, the same way
 * SidFile does.
 */
struct PrgFile {
  bool valid = false;
  bool is_p00 = false;          /* it arrived in a PC64 container */

  addr_t load_addr = 0;
  addr_t end_addr = 0;          /* the last byte, inclusive */

  const data_t * data = nullptr;
  size_t data_size = 0;

  /* If the program starts with a BASIC line that SYSes somewhere, this is
   * where. Nearly every machine code program on a disk does, because that is
   * how you start one by typing RUN. */
  bool has_sys_stub = false;
  addr_t sys_addr = 0;

  char name[17] = { 0 };        /* from the container, when there is one */

  /** @brief True when this loads where BASIC programs go. */
  bool is_basic(void) const { return load_addr == kBasicStart; }
};

/**
 * @brief Parse a PRG or a P00.
 *
 * @param bytes  the whole file
 * @param len    its length
 * @param out    filled in on success, out.valid says which
 */
bool prgfile_parse(const data_t * bytes, size_t len, PrgFile & out);

} /* namespace usbsid */

#endif /* _US_FILE_PRGFILE_H_ */
