/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * mmu.cpp
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

#include "mmu.h"

#include "mos6526.h"
#include "mos6569.h"
#include "mos6581_8580.h"

#include "util/logging.h"

namespace usbsid {

namespace {
/* Cartridge zones with no cartridge in the port read as open bus */
constexpr data_t kOpenBus = 0xff;
} /* namespace */

Mmu::Mmu(Ram & ram, Mos906114Pla & pla)
  : ram_(ram), pla_(pla)
{
  reset();
}

void Mmu::reset(void)
{
  /* Everything is an input after reset, so the three latches read as the
   * pull ups: LORAM, HIRAM and CHAREN all high, which is mode 31. */
  port_dir_  = 0x00;
  port_data_ = 0x00;
  vic_bank_  = 0;
  update_banks();
}

data_t Mmu::port_read(void) const
{
  const data_t driven = static_cast<data_t>(port_data_ & port_dir_);
  const data_t floating =
    static_cast<data_t>(kPortInputHigh & static_cast<data_t>(~port_dir_));
  return static_cast<data_t>(driven | floating);
}

void Mmu::update_banks(void)
{
  /* The PLA sees the effective level on the pins, which for an input bit is
   * the pull up and not the latch. */
  pla_.update(port_read(), pla_.game(), pla_.exrom());
}

data_t Mmu::read_rom(Bank bank, addr_t addr) const
{
  switch (bank) {
    case Bank::Basic:
      return roms.basic[addr - kAddrBasicFirstPage];
    case Bank::Kernal:
      return roms.kernal[addr - kAddrKernalFirstPage];
    case Bank::CharRom:
      return roms.chargen[addr - kAddrCharsFirstPage];
    default:
      return kOpenBus;
  }
}

/**
 * @brief The IO block.
 *
 * A chip only answers if it is actually fitted. A socket with nothing in it
 * does not drive the bus, so the access falls through to the RAM underneath,
 * which is also how the existing player mirrors SID writes into RAM. That
 * matters in practice: a machine with no second SID still reads and writes
 * $d420 sensibly, and test programs that use a spare IO address as a
 * scratchpad behave the same as on the real thing.
 */
data_t Mmu::read_io(addr_t addr)
{
  switch (addr & 0x0f00) {
    case 0x0000: case 0x0100: case 0x0200: case 0x0300: /* $d000-$d3ff VIC */
      return (vic_ != nullptr) ? vic_->io_read(addr) : ram_.read(addr);
    case 0x0400: case 0x0500: case 0x0600: case 0x0700: /* $d400-$d7ff SID */
      return (sid_ != nullptr) ? sid_->io_read(addr) : ram_.read(addr);
    case 0x0800: case 0x0900: case 0x0a00: case 0x0b00: /* $d800-$dbff colour */
      /* Colour RAM is four bits wide. The upper nybble is whatever the VIC
       * last put on the bus; returning the nybble alone keeps it predictable
       * and nothing in a SID player depends on the floating half. */
      return ram_.read_color(addr);
    case 0x0c00:                                        /* $dc00-$dcff CIA1 */
      return (cia1_ != nullptr) ? cia1_->io_read(addr) : ram_.read(addr);
    case 0x0d00:                                        /* $dd00-$ddff CIA2 */
      return (cia2_ != nullptr) ? cia2_->io_read(addr) : ram_.read(addr);
    case 0x0e00:                                        /* $de00-$deff IO1 */
      return (io1_ != nullptr) ? io1_->io_read(addr) : ram_.read(addr);
    default:                                            /* $df00-$dfff IO2 */
      return (io2_ != nullptr) ? io2_->io_read(addr) : ram_.read(addr);
  }
}

void Mmu::write_io(addr_t addr, data_t value)
{
  switch (addr & 0x0f00) {
    case 0x0000: case 0x0100: case 0x0200: case 0x0300:
      if (vic_ != nullptr) vic_->io_write(addr, value);
      else ram_.write(addr, value);
      break;
    case 0x0400: case 0x0500: case 0x0600: case 0x0700:
      if (sid_ != nullptr) sid_->io_write(addr, value);
      else ram_.write(addr, value);
      break;
    case 0x0800: case 0x0900: case 0x0a00: case 0x0b00:
      ram_.write_color(addr, value);
      break;
    case 0x0c00:
      if (cia1_ != nullptr) cia1_->io_write(addr, value);
      else ram_.write(addr, value);
      break;
    case 0x0d00:
      if (cia2_ != nullptr) cia2_->io_write(addr, value);
      else ram_.write(addr, value);
      break;
    case 0x0e00:
      if (io1_ != nullptr) io1_->io_write(addr, value);
      else ram_.write(addr, value);
      break;
    default:
      if (io2_ != nullptr) io2_->io_write(addr, value);
      else ram_.write(addr, value);
      break;
  }
}

data_t Mmu::read(addr_t addr)
{
  /* The 6510 intercepts its own port before anything else sees the address */
  if (US_UNLIKELY(addr <= kAddrMemoryLayout)) {
    return (addr == kAddrDataDirection) ? port_dir_ : port_read();
  }

  const Bank bank = pla_.bank_for_page(static_cast<uint8_t>(addr >> 12));
  switch (bank) {
    case Bank::Ram: {
      const data_t value = ram_.read(addr);
      US_LOG_IF(read_writes, "[R] $%04x:%02x ram\n", addr, value);
      return value;
    }
    case Bank::Io:      return read_io(addr);
    case Bank::Basic:
    case Bank::Kernal:
    case Bank::CharRom: {
      const data_t value = read_rom(bank, addr);
      US_LOG_IF(rom_rw, "[R] $%04x:%02x %s\n", addr, value,
                (bank == Bank::Basic) ? "basic"
                : (bank == Bank::Kernal) ? "kernal" : "chargen");
      US_LOG_IF(read_writes, "[R] $%04x:%02x rom\n", addr, value);
      return value;
    }
    case Bank::CartLo:
    case Bank::CartHi:
    case Bank::Open:
    default:
      /* No cartridge support: those zones read as open bus */
      return kOpenBus;
  }
}

void Mmu::write(addr_t addr, data_t value)
{
  if (US_UNLIKELY(addr <= kAddrMemoryLayout)) {
    if (addr == kAddrDataDirection) {
      port_dir_ = value;
    } else {
      port_data_ = value;
    }
    US_LOG_IF(pla, "[PLA] $%04x:%02x port, mode now %02x\n", addr, value,
              port_read());
    /* The RAM underneath keeps a copy: the CPU cannot see it, but the VIC
     * reads that address as ordinary RAM. */
    ram_.dma_write(addr, value);
    update_banks();
    return;
  }

  const Bank bank = pla_.bank_for_page(static_cast<uint8_t>(addr >> 12));
  if (bank == Bank::Io) {
    write_io(addr, value);
    return;
  }

  /* Writes always land in the RAM underneath, whatever is mapped over it.
   * That is what makes writing through the KERNAL ROM work. */
  US_LOG_IF(read_writes, "[W] $%04x:%02x ram\n", addr, value);
  ram_.write(addr, value);
}

data_t Mmu::vic_read(addr_t addr) const
{
  const addr_t base = static_cast<addr_t>(vic_bank_ << 14);
  const addr_t full = static_cast<addr_t>(base | (addr & 0x3fff));

  /* The character generator appears at $1000-$1fff of the two even banks */
  if ((vic_bank_ & 1) == 0 && (addr & 0x3fff) >= 0x1000 &&
      (addr & 0x3fff) < 0x2000) {
    return roms.chargen[(addr & 0x0fff)];
  }
  return ram_.dma_read(full);
}

} /* namespace usbsid */
