/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_asid.h
 * The backend that talks to the hardware.
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
#ifndef _US_SID_SID_ASID_H_
#define _US_SID_SID_ASID_H_


#include <string>
#include <vector>

#include "types.h"

#include "sid_backend.h"
#include "sidfile.h"

#include "RtMidi.h"

namespace usbsid {

#define ASID_MAXSID 4
#define ASID_FM_MAX_PAIRS 16

/**
 * @brief Play calls per frame for the ASID environment message
 *
 * Raster driven tunes play once per frame, a CIA driven tune every latch + 1 cycles
 * The multiplier is the frame length over that period, rounded and clamped to 1..16
 */
inline int asid_speed_multiplier(bool uses_cia, uint16_t cia_latch,
                                 uint32_t cycles_per_frame)
{
  if (!uses_cia) return 1;
  const uint32_t period = static_cast<uint32_t>(cia_latch) + 1;
  uint32_t m = (2 * cycles_per_frame + period) / (2 * period);
  if (m < 1) m = 1;
  if (m > 16) m = 16;
  return static_cast<int>(m);
}

/**
 * @brief Send register writes to any ASID supporting Midi endpoint
 */
class ASIDBackend final : public SidBackend
{
  private:
    RtMidiOut *midiout = nullptr;
    int nports, asidport = 0;

    enum C64Cycles {
      EMULATION_CYCLES_PER_SECOND_DFLT = 1000000,
      EMULATION_CYCLES_PER_SECOND_PAL  =  985248,
      EMULATION_CYCLES_PER_SECOND_NTSC = 1022727,
    };
    struct ASIDChip {
      unsigned char sidRegister[32];
      unsigned char sidModified[32];
    };

    struct ASIDChip sid_chip[4] = {};

    int nosids = 1;
    const unsigned char sidaddr[4] = { 0x4E, 0x50, 0x51, 0x52 };

    std::vector<unsigned char> message;
    std::vector<unsigned char> basic_message;

    /* FM/OPL index/data pairs waiting for an ASID 0x60 message */
    std::vector<unsigned char> fm_pairs;
    unsigned char fm_addr = 0;

    bool port_is_open = false;

  public:
    ASIDBackend(void);
    ~ASIDBackend(void) override;

    void list_ports(void);
    bool asid_init(int port, int no_sids);
    void asid_close(void);

    int curr_port(void) { return asidport; };
    std::string curr_device(void) { return midiout->getPortName(asidport); }; /* Use .c_str() for logging */

    void write(addr_t reg, data_t value, uint16_t cycles) override;
    void wait(uint16_t cycles) override;
    void flush(void) override;
    void reset(void) override;

    /* multiplier: play calls per frame, clamped to 1..16 */
    void sendSIDEnvironment(bool isPAL, int multiplier = 1);
    void sendSIDType(bool is6581);

    int asid_flush(void);
    void asid_flush_fm(void);
    void asid_clear(void);
    int asid_dump(uint16_t addr, uint8_t byte, int sidno);

};

} /* namespace usbsid */

#endif /* _US_SID_SID_ASID_H_ */
