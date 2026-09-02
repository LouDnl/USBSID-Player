/*
 * Motorola 68B50 ACIA emulation
 * (for Midi and or Uart communication)
 * Copyright (c) 2025, LouD <emudore@mail.loudai.nl>
 *
 * MC68B50.cpp
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
 */

#include "MC68B50.h"

#if defined(EMBEDDED) && EMBEDDED
extern "C" {
  #include "globals.h"
  #include "pico/util/queue.h"  /* Inter core queue */
  #include "midi.h"
  extern queue_t cynthcart_queue;
}
#endif

namespace usbsid {

MC68B50::MC68B50(Bus & bus)
  : bus_(bus)
{
  reset();
}

void MC68B50::reset(void)
{
  regs_[STATUS] = TDRE; /* Set TDRE default to empty */
  bus_.set_irq(IrqSource::Expansion, false);
}

/**
 * @brief reads a byte from MC6850 readable registers
 * Configured for Datel/Kerberos; registers for other brands would sit at
 * different offsets in this same page, see ACIARegisters_all_unused.
 */
data_t MC68B50::io_read(addr_t addr)
{
  const uint8_t r = static_cast<uint8_t>(addr & 0x0f);
  data_t retval = 0;
  switch (r) {
    case CONTROL: /* $de04 ~ control register ~ write only */
      break;
    case STATUS:  /* $de06 ~ status register  ~ read only */
      retval = regs_[STATUS];
      break;
    case TXDR:    /* $de05 ~ TX register      ~ write only */
      break;
    case RXDR:    /* $de07 ~ RX register      ~ read only */
      regs_[STATUS] &= ~(IRQ | RDRF); /* Clear IRQ and RDRF on read */
      bus_.set_irq(IrqSource::Expansion, false);
      retval = regs_[RXDR];
      break;
    default:      /* default always returns whatever was last written */
      retval = regs_[r];
      break;
  }
  return retval;
}

/**
 * @brief writes a byte to MC6850 writeable registers
 * Configured for Datel/Kerberos, same caveat as io_read.
 */
void MC68B50::io_write(addr_t addr, data_t value)
{
  const uint8_t r = static_cast<uint8_t>(addr & 0x0f);
  switch (r) {
    case CONTROL: /* $de04 ~ control register ~ write only */
      regs_[CONTROL] = value;
      /* Only the master reset (both counter divide bits set) changes
       * anything CynthCart's fixed 8N1, no-interrupt-on-TX setup needs. */
      if ((value & CR0CR1SEL) == RES) reset();
      return;
    case STATUS:  /* $de06 ~ status register  ~ read only */
      return;
    case TXDR:    /* $de05 ~ TX register      ~ write only */
      regs_[TXDR] = value;
      return;
    case RXDR:    /* $de07 ~ RX register      ~ read only */
      return;
    default:      /* default always writes to the scratch register */
      regs_[r] = value;
      return;
  }
}

inline void MC68B50::process_midi(void)
{
  if (regs_[STATUS] & RDRF) return; /* data already waiting to be read */

  #if defined(EMBEDDED) && EMBEDDED
  if (!queue_is_empty(&cynthcart_queue)) {
    cynthcart_queue_entry_t cq_entry;
    if (queue_try_remove(&cynthcart_queue, &cq_entry)) {
      regs_[RXDR] = cq_entry.data;       /* Add data to the RXDR */
      regs_[STATUS] |= (IRQ | RDRF);     /* Set IRQ and RDRF for data available */
      bus_.set_irq(IrqSource::Expansion, true);
    }
  }
  #endif
}

/**
 * @brief drain one queued MIDI byte if there is room, called once per frame
 */
void MC68B50::emulate(void)
{
  process_midi();
}

} /* namespace usbsid */
