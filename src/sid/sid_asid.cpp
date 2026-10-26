/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_asid.cpp
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
 * WITHOUT ANY WARRANTY {} without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "sid_asid.h"
#include "util/logging.h"


namespace usbsid {

ASIDBackend::ASIDBackend(void)
{

}

ASIDBackend::~ASIDBackend(void)
{
  asid_close();
}

void ASIDBackend::list_ports(void)
{
  RtMidiOut *l_midiout = new RtMidiOut();
  unsigned int nports = l_midiout->getPortCount();
  std::fprintf(stdout, "Available Midi ports: %u\n", nports);
  std::string port_name;
  for(unsigned int i = 0; i < nports; i++) {
    port_name = l_midiout->getPortName(i);
    std::fprintf(stdout, "  Port %u : %s\n", i, port_name.c_str());
  }
  delete l_midiout;
}

bool ASIDBackend::asid_init(int midi_port, int no_sids)
{
  nosids = no_sids;
  int nports, asidport = 0;
  // Uncomment for debug prints
  // printf("Opening Midi port %d for ASID\n", midi_port);

  asidport = midi_port;
  if (midiout) { delete midiout; midiout = nullptr; }
  port_is_open = false;
  midiout = new RtMidiOut();
  nports = midiout->getPortCount();
  // Uncomment for debug prints
  // std::fprintf(stdout, "Available Midi ports: %u\n", nports);
  // for(uint i = 0; i < nports; i++) {
  //   std::fprintf(stdout, "  Port %d : %s\n", i, midiout->getPortName(i).c_str());
  // }
  if (asidport >= nports) {
    printf("Requested Midi port %d is not available\n", asidport);
    delete midiout;
    midiout = nullptr;
    return false;
  }

  printf("  midi     : using ASID port %d: %s\n", asidport, midiout->getPortName(asidport).c_str());

  midiout->openPort(asidport);
  port_is_open = true;

  basic_message.clear();
  basic_message.push_back(0xf0);
  basic_message.push_back(0x2d);
  basic_message.push_back(0x4c);
  basic_message.push_back(0xf7);
  midiout->sendMessage(&basic_message); //start sid play mode

  return true;
}

void ASIDBackend::asid_close(void)
{
    if (port_is_open && midiout) {
      basic_message.clear();
      basic_message.push_back(0xf0);
      basic_message.push_back(0x2d);
      basic_message.push_back(0x4d);
      basic_message.push_back(0xf7);
      asid_clear();
      midiout->sendMessage(&basic_message);
    }
    /* Free the handle on every path, an unopened port owns one too */
    delete midiout;
    midiout = nullptr;
    port_is_open = false;
}

void ASIDBackend::sendSIDEnvironment(bool isPAL, int multiplier)
{
  // Physical out buffer, including protocol overhead
  unsigned char ASidOutBuffer[8];
  int index = 0;

  if (!midiout || !port_is_open) return;

  // No UI exist to request buffering or change speed multiplier
  const bool isBufferingRequested = false;
  // Speed field is the multiplier minus 1, 1x to 16x
  if (multiplier < 1) multiplier = 1;
  if (multiplier > 16) multiplier = 16;
  const int speedMultiplier = multiplier - 1;

  // Time between two frames
  /* int64_t, long is 32 bit on Windows (LLP64) and 1000000*63*312 overflows it */
  const int frameDeltaUs = isPAL
    ? (int)((int64_t)1000000*63*312/EMULATION_CYCLES_PER_SECOND_PAL)
    : (int)((int64_t)1000000*65*263/EMULATION_CYCLES_PER_SECOND_NTSC);

  // Sysex start data for an ASID message
  ASidOutBuffer[index++] = 0xf0;
  ASidOutBuffer[index++] = 0x2d;
  ASidOutBuffer[index++] = 0x31; // SID environment

  // Payload

  /* data0: settings
    bit0    : 0 = PAL, 1 = NTSC
    bits4-1 : speed, 1x to 16x (value 0-15)
    bit5    : 1 = custom speed (only framedelta valid)
    bit6    : 1 = buffering requested by user
    */
  ASidOutBuffer[index++] =
    ((isBufferingRequested ? 1:0)   << 6) |
    ((speedMultiplier & 0x0f)       << 1) |
    ((isPAL? 0:1)                   << 0);

  /* data1: framedelta uS, total 7+7+2=16 bits, slowest time = 65535us = 15Hz
     bits6-0: framedelta uS (LSB)

     data2:
     bits6-0: framedelta uS

     data3:
     bits1-0: framedelta uS (MSB)
     bits6-2: 5 bits (reserved)
  */
  ASidOutBuffer[index++] = (frameDeltaUs >> 7*0) & 0x7f;
  ASidOutBuffer[index++] = (frameDeltaUs >> 7*1) & 0x7f;
  ASidOutBuffer[index++] = (frameDeltaUs >> 7*2) & 0x03;

  // Sysex end marker
  ASidOutBuffer[index++] = 0xf7;

  // Send to physical MIDI port
  midiout->sendMessage(ASidOutBuffer, index);
}

void ASIDBackend::sendSIDType(bool is6581)
{
  if (!midiout || !port_is_open) return;

  // Physical out buffer, including protocol overhead
  unsigned char ASidOutBuffer[6];
  int index = 0;

  // Sysex start data for an ASID message
  ASidOutBuffer[index++] = 0xf0;
  ASidOutBuffer[index++] = 0x2d;
  ASidOutBuffer[index++] = 0x32; // SID type

  // Payload
  ASidOutBuffer[index++] = 0; // Chip index, only one chip
  ASidOutBuffer[index++] = is6581? 0x00 : 0x01; // bits 7-1 reserved

  // Sysex end marker
  ASidOutBuffer[index++] = 0xf7;

  // Send to physical MIDI port
  midiout->sendMessage(ASidOutBuffer, index);
}

/**
 * @brief Send the pending FM/OPL pairs as one ASID 0x60 message
 *
 * Layout: F0 2D 60 <pairs-1> <msb mask bytes> <7 bit data bytes> F7
 * One mask byte per seven data bytes holds their high bits
 */
void ASIDBackend::asid_flush_fm(void)
{
  const size_t n = fm_pairs.size();
  if (n == 0) return;
  const size_t mask_bytes = (n - 1) / 7 + 1;
  message.clear();
  message.push_back(0xf0);
  message.push_back(0x2d);
  message.push_back(0x60);
  message.push_back((unsigned char)((n / 2) - 1));
  for (size_t i = 0; i < mask_bytes; i++) message.push_back(0);
  for (size_t i = 0; i < n; i++) {
    if (fm_pairs[i] & 0x80) message[4 + i / 7] |= (unsigned char)(1 << (i % 7));
    message.push_back(fm_pairs[i] & 0x7f);
  }
  message.push_back(0xf7);
  if (port_is_open) midiout->sendMessage(&message);
  fm_pairs.clear();
}

/**
 * @brief Drop all pending SID and FM/OPL state without sending it
 */
void ASIDBackend::asid_clear(void)
{
  for (int sid = 0; sid < ASID_MAXSID; sid++) {
    for (int i = 0; i < 32; i++) sid_chip[sid].sidModified[i] = 0;
  }
  fm_pairs.clear();
}

/**
 * @brief Send one snapshot per SID with changed registers, FM/OPL first, return 0
 *
 * Every chip with a modified register is sent (all chips up to ASID_MAXSID)
 */
int ASIDBackend::asid_flush(void)
{
  static const int regmap[28] = {0,1,2,3,5,6,7,8,9,10,12,13,14,15,16,17,19,20,21,22,23,24,4,11,18,25,26,27};
  if (!port_is_open) return 0;
  /* FM/OPL first keeps OPL and SID writes in tune order */
  asid_flush_fm();
  for (int sid = 0; sid < ASID_MAXSID; sid++) {
    int i = 0, j = 0;
    unsigned int mask = 0;
    unsigned int msb = 0;
    /* Set a mask bit per written register and its bit 7 into msb */
    for (i = 0; i < 28; i++) {
      j = regmap[i];
      if (sid_chip[sid].sidModified[j] != 0) mask |= (1u << i);
      if (sid_chip[sid].sidRegister[j] > 0x7f) msb |= (1u << i);
    }
    if (mask == 0) continue; /* nothing changed for this chip */
    message.clear();
    message.push_back(0xf0);
    message.push_back(0x2d);
    message.push_back(sidaddr[sid]);
    message.push_back(mask & 0x7f);
    message.push_back((mask >> 7) & 0x7f);
    message.push_back((mask >> 14) & 0x7f);
    message.push_back((mask >> 21) & 0x7f);
    message.push_back(msb & 0x7f);
    message.push_back((msb >> 7) & 0x7f);
    message.push_back((msb >> 14) & 0x7f);
    message.push_back((msb >> 21) & 0x7f);
    for (i = 0; i < 28; i++) {
      j = regmap[i];
      if (sid_chip[sid].sidModified[j] != 0) {
        message.push_back(sid_chip[sid].sidRegister[j] & 0x7f);
      }
    }
    message.push_back(0xf7);
    midiout->sendMessage(&message);
    for (i = 0; i < 32; i++) sid_chip[sid].sidModified[i] = 0;
  }
  return 0;
}

/**
 * @brief Fold one SID register write into the frame snapshot, return 0
 *
 * @param addr   register, 0x00-0x1f
 * @param byte   value written
 * @param sidno  chip, 1 based
 *
 * Registers above 0x18 are ignored, ASID uses 0x19-0x1b as shadows for a
 * second write to the voice control registers
 */
int ASIDBackend::asid_dump(uint16_t addr, uint8_t byte, int sidno)
{
  int reg, data;
  sidno -= 1;
  if (sidno < 0 || sidno >= ASID_MAXSID) return 0;

  reg = addr & 0x1f;
  data = byte;
  if (reg > 0x18) return 0;

  struct ASIDChip & c = sid_chip[sidno];
  if (c.sidModified[reg] == 0) {
    c.sidRegister[reg] = data & 0xff;
    c.sidModified[reg] = 1;
    return 0;
  }
  /* Written twice in one frame */
  switch (reg) {
    case 0x04: case 0x0b: case 0x12: {
      /* Control registers are 7 apart, their shadows 1 apart */
      const int shadow = (reg == 0x04) ? 0x19 : (reg == 0x0b) ? 0x1a : 0x1b;
      if (c.sidModified[shadow] != 0) {
        /* Third write, cut the frame and start a new one */
        asid_flush();
        c.sidRegister[reg] = data & 0xff;
        c.sidModified[reg] = 1;
      } else {
        c.sidRegister[shadow] = data & 0xff;
        c.sidModified[shadow] = 1;
      }
      break;
    }
    case 0x16: case 0x17: case 0x18:
      /* Filter and volume have no shadow and their order is audible */
      asid_flush();
      c.sidRegister[reg] = data & 0xff;
      c.sidModified[reg] = 1;
      break;
    default:
      c.sidRegister[reg] = data & 0xff; /* last value of the frame wins */
  }
  return 0;
}

/**
 * @brief Route one write: FM/OPL ports or a SID register of chips 1-4
 *
 * FM/OPL arrives as parked registers at kMaxSids * 0x20 and up,
 * port 0x00 is the index and 0x10 the data port. Writes for chip 5 and up are dropped
 */
void ASIDBackend::write(addr_t reg, data_t value, uint16_t cycles)
{
  (void)cycles;
  if (!port_is_open) return;
  if (reg >= static_cast<addr_t>(kMaxSids * 0x20)) {
    if ((reg & 0x1f) == 0x10) {
      fm_pairs.push_back(fm_addr);
      fm_pairs.push_back((unsigned char)value);
      if (fm_pairs.size() >= (size_t)(ASID_FM_MAX_PAIRS * 2)) asid_flush_fm();
    } else {
      fm_addr = (unsigned char)value;
    }
    return;
  }
  const unsigned chip = (unsigned)(reg >> 5);
  if (chip >= ASID_MAXSID) return;
  asid_dump((unsigned short)(reg & 0x1f), (unsigned char)value, (int)chip + 1);
}

void ASIDBackend::wait(uint16_t cycles)
{
  (void)cycles;
}

void ASIDBackend::flush(void)
{
  asid_flush();
}

void ASIDBackend::reset(void)
{
  asid_clear();
}

} /* namespace usbsid */
