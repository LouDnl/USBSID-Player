/*
 * USBSID-Player web backend.
 *
 * src/web/stub/USBSID.h
 *
 * Header-only stand-in for the libusb-based USBSID-Pico driver
 * (lib/driver/src/USBSID.h) used by the WEB (Emscripten/WASM) build.
 *
 * The WEB target compiles the DESKTOP code paths (which call into
 * USBSID_NS::USBSID_Class) but must NOT link libusb: in the browser the
 * actual USB/MIDI transport lives in JavaScript. This stub mirrors the small
 * slice of the driver API the player calls and routes every register write /
 * flush / read through the C hooks declared below. Those hooks are the single
 * seam the JS side drains (see src/web/web_backend.cpp).
 *
 * File author: LouD
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
#ifndef _US_WEB_STUB_USBSID_H
#define _US_WEB_STUB_USBSID_H

#include <cstdint>
#include <cstddef>

/* Mirror of the real driver define (lib/driver/src/USBSID.h) */
#ifndef SOCKET_BUFFER_SIZE
#define SOCKET_BUFFER_SIZE 12
#endif

/*
 * C hooks implemented in src/web/web_backend.cpp. In P1 these are no-op /
 * console stubs; in P2 they enqueue into the heap ring buffer that the JS
 * worker drains and forwards over WebUSB.
 */
extern "C" {
  void    usbsid_web_write(uint8_t reg, uint8_t val, uint16_t cycles);
  uint8_t usbsid_web_read(uint8_t reg);
  void    usbsid_web_flush(void);
  void    usbsid_web_reset(void);
  void    usbsid_web_set_clockrate(long clockrate);
}

namespace USBSID_NS
{

  /* Minimal API-compatible stand-in for the libusb USBSID_Class */
  class USBSID_Class
  {
  public:
    USBSID_Class(void) {}
    ~USBSID_Class(void) {}

    /* Lifecycle */
    int  USBSID_Init(bool /*start_threaded*/, bool /*with_cycles*/) { return 0; }
    int  USBSID_Close(void) { return 0; }

    /* Control */
    void USBSID_Reset(void)             { usbsid_web_reset(); }
    void USBSID_ResetAllRegisters(void) { usbsid_web_reset(); }
    void USBSID_Mute(void)              {}
    void USBSID_UnMute(void)            {}
    void USBSID_Flush(void)             { usbsid_web_flush(); }
    void USBSID_SetFlush(void)          { usbsid_web_flush(); }
    void USBSID_DisableThread(void)     {}

    /* Clock */
    long USBSID_GetClockRate(void)                       { return _clockrate; }
    void USBSID_SetClockRate(long clockrate, bool /*t*/) { _clockrate = clockrate; usbsid_web_set_clockrate(clockrate); }

    /* Socket / SID inventory. Web build assumes a single SID in socket one
     * unless the JS side overrides via web_backend setters (future). */
    int      USBSID_GetNumSIDs(void)                                        { return _numsids; }
    int      USBSID_GetFMOplSID(void)                                       { return -1; }
    int      USBSID_GetPCBVersion(void)                                     { return 13; }
    uint8_t* USBSID_GetSocketConfig(uint8_t socket_config[])                { for (int i = 0; i < SOCKET_BUFFER_SIZE; i++) socket_config[i] = 0; return socket_config; }
    int      USBSID_GetSocketNumSIDS(int socket, uint8_t[])                 { return (socket == 1) ? _numsids : 0; }
    int      USBSID_GetSocketChipType(int /*socket*/, uint8_t[])            { return 0; }
    int      USBSID_GetSocketSIDType1(int /*socket*/, uint8_t[])            { return 0; }
    int      USBSID_GetSocketSIDType2(int /*socket*/, uint8_t[])            { return 0; }

    /* I/O */
    unsigned char USBSID_Read(uint8_t reg)                                  { return usbsid_web_read(reg); }
    void USBSID_WriteRingCycled(uint8_t reg, uint8_t val, uint16_t cycles)  { usbsid_web_write(reg, val, cycles); }
    uint_fast64_t USBSID_WaitForCycle(uint_fast16_t /*cycles*/)             { return 0; }

  private:
    long _clockrate = 985248; /* PAL default */
    int  _numsids   = 1;
  };

} /* namespace USBSID_NS */

#endif /* _US_WEB_STUB_USBSID_H */
