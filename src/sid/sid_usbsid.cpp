/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * sid_usbsid.cpp
 *
 * The call sequence follows old player ~ src/emulation.cpp, which is what the
 * firmware and the current player both use: initialise with buffering and
 * cycled writes, reset the chips, then feed cycled writes and flush at the
 * end of every frame.
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

#include "sid_usbsid.h"

#include "USBSID.h"

namespace usbsid {

UsbSidBackend::UsbSidBackend(void) {}

UsbSidBackend::~UsbSidBackend(void)
{
  close();
}

bool UsbSidBackend::open(void)
{
  if (open_) return true;

  device_ = new USBSID_NS::USBSID_Class();
  /* threaded, with cycles: the ring buffer and the cycle exact write path */
  if (device_->USBSID_Init(true, true) < 0) {
    delete device_;
    device_ = nullptr;
    return false;
  }
  open_ = true;

  uint8_t socket_config[SOCKET_BUFFER_SIZE];
  device_->USBSID_GetSocketConfig(socket_config);
  sids_one_ = device_->USBSID_GetSocketNumSIDS(1, socket_config);
  sids_two_ = device_->USBSID_GetSocketNumSIDS(2, socket_config);
  num_sids_ = device_->USBSID_GetNumSIDs();
  fmopl_sid_ = device_->USBSID_GetFMOplSID();
  pcb_version_ = device_->USBSID_GetPCBVersion();

  device_->USBSID_ResetAllRegisters();
  device_->USBSID_Reset();
  return true;
}

void UsbSidBackend::close(void)
{
  if (!open_ || device_ == nullptr) return;
  device_->USBSID_Flush();
  device_->USBSID_DisableThread();
  device_->USBSID_ResetAllRegisters();
  device_->USBSID_Reset();
  delete device_;
  device_ = nullptr;
  open_ = false;
}

void UsbSidBackend::set_clock_rate(uint32_t hz)
{
  if (!open_) return;
  if (static_cast<uint32_t>(device_->USBSID_GetClockRate()) != hz) {
    /* holding the SIDs in reset while the clock changes is strongly advised */
    device_->USBSID_SetClockRate(static_cast<long>(hz), true);
  }
}

void UsbSidBackend::write(data_t reg, data_t value, uint16_t cycles)
{
  if (!open_) return;
  /* $80 and above are not SID registers: they are the FM/OPL addresses that no
   * chip claimed, and only a transport that carries FM itself can use them. This
   * one talks to a board, so it drops them, which is what happened before they
   * were forwarded at all. */
  if (reg >= 0x80) return;
  /* Straight through. The cycle the access itself costs has already been taken
   * off upstream, by SidConfig::access_overhead in cycles_since_last_event(),
   * so what arrives here is the pre-delay the board should sit out and nothing
   * more. Taking a second cycle off here, which is what this used to do, made
   * every write land a cycle early and the error piled up across a frame. */
  device_->USBSID_WriteRingCycled(reg, value, cycles);
}

data_t UsbSidBackend::read(data_t reg, uint16_t cycles)
{
  (void)cycles;
  if (!open_) return 0;
  return device_->USBSID_Read(reg);
}

/**
 * @brief A gap too long to carry with a write.
 *
 * Counted and otherwise ignored, on purpose. The driver has
 * `USBSID_WaitForCycle()`, which spins on the clock until the time has passed,
 * and calling it here is wrong twice over: the pacer already holds playback to
 * real time frame by frame, so the wait is served a second time and a silent
 * stretch takes twice as long as it should. A tune that opens with a couple of
 * seconds of unpacking, like Coma Light 13, fell more than a second behind
 * doing exactly that, and then the pacer sprinted to catch up and buried the
 * driver's ring buffer in a burst it had no room for. That was heard as a
 * crackle when the digi started.
 *
 * Dropping the gap costs nothing: the device paces itself from the deltas it
 * is given, and with nothing to do it simply waits for the next write, which
 * arrives when the pacer sends it.
 */
void UsbSidBackend::wait(uint16_t cycles)
{
  waited_ += cycles;
}

void UsbSidBackend::flush(void)
{
  if (!open_) return;
  device_->USBSID_SetFlush();
}

void UsbSidBackend::reset(void)
{
  if (!open_) return;
  device_->USBSID_ResetAllRegisters();
  device_->USBSID_Reset();
}

void UsbSidBackend::mute(bool muted)
{
  if (!open_) return;
  if (muted) device_->USBSID_Mute();
  else       device_->USBSID_UnMute();
}

} /* namespace usbsid */
