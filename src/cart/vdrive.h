/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * vdrive.h
 * Read-only virtual disk drive on device 8 at KERNAL level, files from a table
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
#ifndef _US_CART_VDRIVE_H_
#define _US_CART_VDRIVE_H_

#include <stddef.h>
#include <stdint.h>

#include "core/types.h"
#include "mem/io_device.h"

namespace usbsid {

class Ram;

/* PRG file, data starts with its two byte load address */
struct VFile {
  const char * name;   /* PETSCII, upper case as typed on a C64 */
  const uint8_t * data;
  size_t size;
};

/**
 * Owns IO1 ($de00-$deff): $de00-$de7f go to `lower` (the MIDI ACIA), $de80-$deef
 * hold a 6502 stub the KERNAL file vectors point at, $def0-$deff are its registers.
 * Device 8 calls are served from the file table, every other device falls through
 * to the original KERNAL routine. Writes are refused (26, WRITE PROTECT ON).
 */
class VirtualDrive : public IoDevice
{
  public:
    VirtualDrive(Ram & ram, IoDevice * lower, const VFile * files, size_t file_count,
                 const char * disk_name, const char * disk_id);

    data_t io_read(addr_t addr) override;
    void io_write(addr_t addr, data_t value) override;

    /* After the KERNAL boot (it sets $031a-$0333), points the file vectors at the stub */
    void install(void);
    void reset(void);

  private:
    enum Fn : uint8_t { OPEN, CLOSE, CHKIN, CHKOUT, CLRCHN, CHRIN, CHROUT, GETIN, CLALL, LOAD, SAVE, FN_COUNT };

    struct Channel {
      bool used;
      uint8_t la;
      uint8_t sa;
      const uint8_t * data;
      size_t size;
      size_t pos;
    };

    static constexpr size_t kChannels = 4;
    static constexpr size_t kNameMax = 40;
    static constexpr size_t kDirMax = 32 * 20;

    void execute(uint8_t ret_lo);
    bool op_open(void);
    bool op_close(void);
    bool op_chkin(bool out);
    void op_chrin(void);
    bool op_load(void);
    void op_save(void);
    void close_all(void);

    size_t read_name(char * out);
    const VFile * find(const char * pattern);
    void build_directory(const char * pattern);
    void set_status(uint8_t code, const char * text);
    void run_command(const char * cmd);
    Channel * channel_for(uint8_t la);
    void finish(bool carry);

    Ram & ram_;
    IoDevice * lower_;
    const VFile * files_;
    size_t file_count_;
    const char * disk_name_;
    const char * disk_id_;

    data_t regs_[16] = {};
    addr_t rom_[FN_COUNT] = {};
    bool installed_ = false;

    Channel channels_[kChannels] = {};
    uint8_t in_la_ = 0;
    char status_[48] = {};
    size_t status_len_ = 0;
    uint8_t dir_[kDirMax] = {};
    size_t dir_len_ = 0;
};

} /* namespace usbsid */

#endif /* _US_CART_VDRIVE_H_ */
