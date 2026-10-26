/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * sid_backend.h
 * Where SID register writes go
 *
 * The emulation decides what is written and when (in cycles)
 * A backend sends it to USBSID-Pico, throws it away or records it
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
#ifndef _US_SID_SID_BACKEND_H_
#define _US_SID_SID_BACKEND_H_

#include "types.h"

namespace usbsid {

class SidBackend
{
  public:
    virtual ~SidBackend(void) = default;

    /**
     * @brief Write one register
     *
     * @param reg    physical register with the chip number folded in
     *               $00-$1f is the first SID, $20-$3f the second etc
     *               up to kMaxSids (15, see sidfile.h) chips at $1c0-$1df
     *               addr_t is 16 bit, 15 chips do not fit in an 8 bit register byte
     *               Backends for a real board (max 4 sockets) drop anything at or above $80
     * @param value  byte written
     * @param cycles cycles since the previous event, max $ffff
     */
    virtual void write(addr_t reg, data_t value, uint16_t cycles) = 0;

    /** @brief Read one register, backends without hardware may guess */
    virtual data_t read(addr_t reg, uint16_t cycles) { (void)reg; (void)cycles; return 0; }

    /**
     * @brief Report a read answered by the machine, for recording only
     *
     * No bus access, cycle base untouched, nothing is sent on
     *
     * @param reg    register the access maps to
     * @param value  value the tune read
     * @param cycles cycles since the previous event
     */
    virtual void note_read(addr_t reg, data_t value, uint16_t cycles)
    {
      (void)reg; (void)value; (void)cycles;
    }

    /** @brief More than $ffff cycles passed with nothing to write */
    virtual void wait(uint16_t cycles) { (void)cycles; }

    /** @brief End of frame, push buffered writes */
    virtual void flush(void) {}

    virtual void reset(void) {}

    /** @brief All backend types */
    enum class SidBackendType { None, USB, ReSIDfp, Wav, NetDevice, ASID, Web, Embed };

  private:
    /** @brief Backend type storage, None for backends that never set one (test and null backends) */
    SidBackendType backend_type = SidBackendType::None;

  public:
    /** @brief Returns the type of backend that is used */
    SidBackendType get_backend_type(void) { return backend_type; }

    /** @brief Set the type of backend that is used */
    void set_backend_type(SidBackendType bt) { backend_type = bt; }

    /** @brief Returns true when backend requires reading from Voice 3 in ReSIDfp or a real SID */
    bool read_v3_backend(void) {
      /* This gives the same result as if real reads were enabled */
      switch (backend_type) {
        case SidBackendType::ReSIDfp:
        case SidBackendType::Wav:
        case SidBackendType::Embed: /* Doesn't read from ReSIDfp but directly from the SID */
          return true;
        case SidBackendType::USB:
        case SidBackendType::NetDevice:
        case SidBackendType::ASID:
        case SidBackendType::Web:
        default: return false;
      }
    }

    /** @brief Returns true when backend requires reading from virtual Voice 3 */
    bool read_v3_virtual(void) {
      switch (backend_type) {
        case SidBackendType::None: /* No type set keeps the virtual voice 3, as before backend types existed */
        case SidBackendType::USB:
        case SidBackendType::NetDevice:
        case SidBackendType::ASID:
        /* When needed this gets overruled by web_api.cpp:usp_audio_configure setting real_reads to true */
        case SidBackendType::Web:
          return true;
        case SidBackendType::Embed:
        case SidBackendType::ReSIDfp:
        case SidBackendType::Wav:
        default: return false;
      }
    }

    /** @brief Returns the type of backend that is used for logging */
    const char * get_backend_type_string(void) {
      const char * bt = "Undefined";
      switch (backend_type) {
        case SidBackendType::Embed:     bt = "Embedded player";      break;
        case SidBackendType::USB:       bt = "USBSID-Pico";    break;
        case SidBackendType::ReSIDfp:   bt = "ReSIDfp";              break;
        case SidBackendType::Wav:       bt = "Wav";                  break;
        case SidBackendType::NetDevice: bt = "Network SID Device";            break;
        case SidBackendType::ASID:      bt = "ASID";                 break;
        case SidBackendType::Web:       bt = "Web";                  break;
        default: break;
      }
      return bt;
    }
};

/** @brief Throws everything away, the default */
class NullSidBackend final : public SidBackend
{
  public:
    void write(addr_t reg, data_t value, uint16_t cycles) override
    {
      (void)reg; (void)value; (void)cycles;
      ++writes;
    }
    void flush(void) override { ++flushes; }
    void reset(void) override { writes = 0; flushes = 0; }

    uint32_t writes = 0;
    uint32_t flushes = 0;
};

} /* namespace usbsid */

#endif /* _US_SID_SID_BACKEND_H_ */
