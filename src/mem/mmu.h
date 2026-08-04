/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mmu.h
 * Address decoding: the processor port at $00/$01, the bank map the PLA
 * produces, the ROMs, the colour RAM and the IO block.
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
#ifndef _US_MEM_MMU_H_
#define _US_MEM_MMU_H_

#include "constants.h"
#include "io_device.h"
#include "memory.h"
#include "mos906114_pla.h"
#include "ram.h"
#include "rom.h"
#include "types.h"

namespace usbsid {

class Mos6526;
class Mos6569;
class Mos6581_8580;

/**
 * @brief C64 memory management.
 *
 * Everything the CPU does goes through read() and write(). The VIC has its
 * own path, vic_read(), because it sees a different memory map: RAM plus the
 * character generator, never the IO block and never the other ROMs.
 */
class Mmu final : public Memory
{
  public:
    Mmu(Ram & ram, Mos906114Pla & pla);
    ~Mmu(void) override = default;

    void reset(void);

    data_t read(addr_t addr) override US_RAM_ATTR;
    void write(addr_t addr, data_t value) override US_RAM_ATTR;

    /* IO block wiring, filled in as the chips arrive.
     *
     * By their real types, not as IoDevice: these four are on the per access
     * path, and reaching them through a vtable costs a load and an indirect
     * branch, plus a thunk that the compiler puts in flash where every call
     * to it is an XIP stall on the device. The expansion pages stay generic,
     * nothing time critical lives there. */
    void attach_vic(Mos6569 * dev) { vic_ = dev; }
    void attach_sid(Mos6581_8580 * dev) { sid_ = dev; }
    void attach_cia1(Mos6526 * dev) { cia1_ = dev; }
    void attach_cia2(Mos6526 * dev) { cia2_ = dev; }
    void attach_io1(IoDevice * dev) { io1_ = dev; }
    void attach_io2(Mos6581_8580 * dev) { io2_ = dev; }

    /**
     * @brief The VIC side of memory.
     *
     * The VIC addresses 16 KB at a time (the bank comes from CIA2 port A) and
     * sees the character generator at $1000-$1fff of banks 0 and 2 instead of
     * the RAM underneath.
     */
    data_t vic_read(addr_t addr) const US_RAM_ATTR;
    void set_vic_bank(uint8_t bank) { vic_bank_ = static_cast<uint8_t>(bank & 3); }
    uint8_t vic_bank(void) const { return vic_bank_; }

    /* Loaders and the SID register mirror bypass all decoding */
    US_ALWAYS_INLINE data_t dma_read(addr_t addr) const { return ram_.dma_read(addr); }
    US_ALWAYS_INLINE void dma_write(addr_t addr, data_t value) { ram_.dma_write(addr, value); }

    /* Processor port */
    US_ALWAYS_INLINE data_t port_dir(void) const { return port_dir_; }
    US_ALWAYS_INLINE data_t port_data(void) const { return port_data_; }
    data_t port_read(void) const US_RAM_ATTR;
    Roms roms;

  private:
    void update_banks(void) US_RAM_ATTR;
    data_t read_io(addr_t addr) US_RAM_ATTR;
    void write_io(addr_t addr, data_t value) US_RAM_ATTR;
    data_t read_rom(Bank bank, addr_t addr) const US_RAM_ATTR;
    Ram & ram_;
    Mos906114Pla & pla_;

    Mos6569 * vic_       = nullptr;
    Mos6581_8580 * sid_  = nullptr;
    Mos6526 * cia1_      = nullptr;
    Mos6526 * cia2_      = nullptr;
    IoDevice * io1_      = nullptr;
    Mos6581_8580 * io2_  = nullptr;

    /* Processor port at $0000 (direction) and $0001 (data).
     * Bits that are inputs read the pull ups, not the latch: bits 0 to 2 have
     * them, bit 4 is the cassette sense which reads high with no tape, and
     * bits 6 and 7 are not bonded out on a 6510. */
    static constexpr data_t kPortInputHigh = 0x17;
    data_t port_dir_  = 0x00;
    data_t port_data_ = 0x00;

    uint8_t vic_bank_ = 0;
};

} /* namespace usbsid */

#endif /* _US_MEM_MMU_H_ */
