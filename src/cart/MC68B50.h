/*
 * Motorola 68B50 ACIA emulation
 * (for Midi and or Uart communication)
 * Copyright (c) 2025, LouD <emudore@mail.loudai.nl>
 *
 * MC68B50.h
 * An IoDevice on IO1 ($de00-$deff), Datel/Kerberos register layout
 * (CONTROL $de04, TXDR $de05, STATUS $de06, RXDR $de07). Ported from
 * emulator-repo's emudore-based version to run inside USBSID-Player: no more
 * C64/Memory dependency, and the CPU IRQ is a level on the bus instead of a
 * direct, repeatedly-poked call.
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

#pragma once
#ifndef _US_CART_MC68B50_H_
#define _US_CART_MC68B50_H_

#include "core/bus.h"
#include "core/types.h"
#include "mem/io_device.h"

namespace usbsid {

/**
 * @brief Parent class containing register addresses,
 * bitmasks and bits
 */
class MC6850BitMasks
{
  protected:

    enum ACIARegisters
    { /* DATEL/SIEL/JMS/C-LAB (Kerberos) */
      CONTROL  = 0x04, /* control register       ~ write only */
      STATUS   = 0x06, /* status register        ~ read only */
      TXDR     = 0x05, /* transmit data register ~ write only */
      RXDR     = 0x07, /* receive data register  ~ read  only */
    };

    enum ControlBitMasks
    {
      CR0CR1SEL = 0b00000011, /* Counter divide select bits */
      WORDSEL   = 0b00011100, /* Word select bits */
      TCCTR     = 0b01100000, /* Transmit control bits */
      INTEN     = 0b10000000, /* Receive interrupt enable bit */
    };

    enum CounterDivideBits
    {
      R1  = 0b00, /* set divide ratio +1 */
      R16 = 0b01, /* set divide ratio +16 */
      R64 = 0b10, /* set divide ratio +64 */
      RES = 0b11, /* reset midi device */
    };

    enum WordSelectBits
    {
      w7e2 = 0b000, /* 7 Bits + Even Parity + 2 Stop Bits */
      w7o2 = 0b001, /* 7 Bits + Odd Parity + 2 Stop Bits */
      w7e1 = 0b010, /* 7 Bits + Even Parity + 1 Stop Bit */
      w7o1 = 0b011, /* 7 Bits + Odd Parity + 1 Stop Bit */
      w8n2 = 0b100, /* 8 Bits + 2 Stop Bits */
      w8n1 = 0b101, /* 8 Bits + 1 Stop Bit */
      w8e1 = 0b110, /* 8 Bits + Even Parity + 1 Stop Bit */
      w8o1 = 0b111, /* 8 Bits + Odd Parity + 1 Stop Bit */
    };

    enum TransmitControlBits
    {
      RTSloTID = 0b00, /* RTS=low, Transmitting Interrupt Disabled. */
      RTSloTIE = 0b01, /* RTS=low, Transmitting Interrupt Enabled. */
      RTShiTID = 0b10, /* RTS=high, Transmitting Interrupt Disabled */
      RTSloTRB = 0b11, /* RTS=low, Transmits a Break level on the Transmit Data Output. Transmitting Interrupt Disabled. */
    };

    enum StatusRegisterBits
    {
      RDRF = (1<<0), /* Receive Data Register Full */
      TDRE = (1<<1), /* Transmit Data Register Empty */
      DCD  = (1<<2), /* Data Carrier Detect */
      CTS  = (1<<3), /* Clear-to-Send */
      FE   = (1<<4), /* Framing Error */
      RO   = (1<<5), /* Receiver Overrun */
      PE   = (1<<6), /* Parity Error */
      IRQ  = (1<<7), /* Interrupt Request */
    };

    /* Interface registers per Midi device type */
    enum ACIARegisters_all_unused
    {
      /* SEQUENTIAL CIRCUITS INC. */
      CONTROL_1  = 0x00, /* control register       ~ write only */
      STATUS_1   = 0x02, /* status register        ~ read only */
      TXDR_1     = 0x01, /* transmit data register ~ write only */
      RXDR_1     = 0x03, /* receive data register  ~ read  only */
      /* PASSPORT & SENTECH */
      CONTROL_2  = 0x08, /* control register       ~ write only */
      STATUS_2   = 0x08, /* status register        ~ read only */
      TXDR_2     = 0x09, /* transmit data register ~ write only */
      RXDR_2     = 0x09, /* receive data register  ~ read  only */
      /* DATEL/SIEL/JMS/C-LAB (Kerberos) */
      CONTROL_3  = 0x04, /* control register       ~ write only */
      STATUS_3   = 0x06, /* status register        ~ read only */
      TXDR_3     = 0x05, /* transmit data register ~ write only */
      RXDR_3     = 0x07, /* receive data register  ~ read  only */
      /* NAMESOFT ~ SAME REGISTERS AS SEQUENTIAL SO NOT USED! */
      CONTROL_4  = 0x00, /* control register       ~ write only */
      STATUS_4   = 0x02, /* status register        ~ read only */
      TXDR_4     = 0x01, /* transmit data register ~ write only */
      RXDR_4     = 0x03, /* receive data register  ~ read  only */
    };

  public:
    MC6850BitMasks(){};
    ~MC6850BitMasks(){};
};

/**
 * @brief Motorola 68B50 Asynchronous Communications Interface Adapter
 *
 * Emulates the Motorola 68B50 ACIA (for Midi and or Uart communication) as
 * an IoDevice sitting on IO1 ($de00-$deff). Its register file is its own,
 * 16 bytes, addressed by the low byte of the address the MMU hands in: real
 * hardware only decodes $de04-$de07, everything else here is a harmless
 * read-your-last-write scratchpad rather than open bus, which matches the
 * original emudore port closely enough and needs no RAM to shadow.
 */
class MC68B50 : protected MC6850BitMasks, public IoDevice
{
  private:

    /* The bus this ACIA raises its IRQ line on */
    Bus & bus_;

    /* Register file, addressed by addr & 0x0f */
    data_t regs_[16] = {};

    /* Process Midi data */
    void process_midi(void);

  public:
    MC68B50(Bus & bus);
    ~MC68B50(void) override = default;

    data_t io_read(addr_t addr) override;
    void io_write(addr_t addr, data_t value) override;

    void reset(void);
    /* Drain one queued MIDI byte if room, called once per frame */
    void emulate(void);

};

} /* namespace usbsid */

#endif /* _US_CART_MC68B50_H_ */
