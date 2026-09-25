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
#include "util/logging.h"

namespace usbsid {

namespace {

/**
 * @brief SIDs a socket holds, parsed from a raw GetSocketConfig() reply.
 *
 * Same decoding as USBSID_Class::USBSID_GetSocketNumSIDS().
 *
 * @param cfg     SOCKET_BUFFER_SIZE byte socket config reply
 * @param socket  1 or 2
 * @returns 0, 1 or 2
 */
int socket_num_sids(const uint8_t * cfg, int socket)
{
  const uint8_t b = cfg[(socket == 1) ? 2 : 5];
  if (((b & 0xf0) >> 4) != 1) return 0;
  return ((b & 0x0f) == 1) ? 2 : 1;
}

} /* namespace */

UsbSidBackend::UsbSidBackend(void)
{
  for (int16_t & slot : chip_to_logical_) slot = -1;
}

UsbSidBackend::~UsbSidBackend(void)
{
  close();
}

bool UsbSidBackend::open(const std::vector<std::string> & board_order)
{
  if (open_) return true;

  /* One empty serial opens a single board by the driver's own default
   * target (index 0), without probing every attached board first. */
  const std::vector<std::string> targets =
    board_order.empty() ? std::vector<std::string>{ std::string() } : board_order;
  /* threaded, with cycles: the ring buffer and the cycle exact write path */
  if (!manager_.OpenAll(targets, true, true)) return false;
  open_ = true;
  single_ = (manager_.BoardCount() == 1);

  const auto & board = manager_.Boards().front();
  if (board.socketconfig_valid) {
    sids_one_ = socket_num_sids(board.socketconfig, 1);
    sids_two_ = socket_num_sids(board.socketconfig, 2);
  }
  pcb_version_ = board.pcbversion;
  if (single_) {
    num_sids_ = board.numsids;
    fmopl_sid_ = board.fmoplsid;
    fmopl_board_ = (fmopl_sid_ >= 1) ? 1 : -1;
  } else {
    num_sids_ = manager_.TotalSIDs();
    find_first_fmopl();
  }

  manager_.ResetAllRegistersAll();
  manager_.StartAll();
  return true;
}

void UsbSidBackend::close(void)
{
  if (!open_) return;
  manager_.FlushAll();
  manager_.ResetAllRegistersAll();
  manager_.StartAll();
  manager_.CloseAll();
  open_ = false;
  single_ = true;
  num_sids_ = sids_one_ = sids_two_ = 0;
  fmopl_sid_ = fmopl_board_ = pcb_version_ = -1;
}

/**
 * @brief Several boards: point fmopl_sid_ at the first board's FM/OPL.
 *
 * Logical slots run board by board in open order (the --boards order):
 * the first slot slot_is_fmopl() accepts is on the first board
 * listed that has an FM/OPL configured. A board whose FM/OPL SID number has
 * no slot in the map is skipped.
 */
void UsbSidBackend::find_first_fmopl(void)
{
  fmopl_sid_ = fmopl_board_ = -1;
  const auto & map = manager_.LogicalMap();
  for (size_t i = 0; i < map.size(); i++) {
    if (!slot_is_fmopl(static_cast<int>(i))) continue;
    fmopl_sid_ = static_cast<int>(i) + 1;
    fmopl_board_ = map[i].board_index + 1;
    return;
  }
}

void UsbSidBackend::set_clock_rate(uint32_t hz)
{
  if (!open_) return;
  /* Hold the SIDs in reset while the clock changes. Each board skips the
   * device write when its rate is unchanged. */
  manager_.SetClockRateAll(static_cast<long>(hz), true);
}

/**
 * @brief See the header. Also where the FM/OPL "unclaimed" park lands.
 *
 * `kFmOplParkBase` (mos6581_8580.cpp) sits at chip index kMaxSids, one past
 * the last real chip: its block starts at `kChipLimit`. Without
 * --select-sids it is dropped. With it, fmopl_sid is -1 on the SID side,
 * every FM/OPL write is parked, and the park goes to the slot an `fm` entry
 * named (fm_slot_), already checked by set_sid_select() to be an FM/OPL.
 *
 * One board: slots map straight onto its four sockets (register < 0x80),
 * whatever its socket config reports. Several boards: slots follow
 * manager_.LogicalMap(), only the SIDs each board has configured.
 */
bool UsbSidBackend::remap(addr_t reg, int & logical_sid, uint8_t & board_reg) const
{
  constexpr addr_t kChipLimit = static_cast<addr_t>(kMaxSids) * 0x20;
  int target;
  if (reg >= kChipLimit) {
    if (!select_active_ || fm_slot_ < 0 || reg >= kChipLimit + 0x20) return false;
    target = fm_slot_;
  } else {
    const int chip = static_cast<int>(reg >> 5);
    target = select_active_ ? chip_to_logical_[chip] : chip;
  }
  if (target < 0) return false;

  const auto & map = manager_.LogicalMap();
  if (single_) {
    if (target >= 4 || map.empty()) return false;
    logical_sid = 0;
    board_reg = static_cast<uint8_t>((target * 0x20) + (reg & 0x1f));
    return true;
  }

  if (static_cast<size_t>(target) >= map.size()) return false;
  logical_sid = target;
  board_reg = static_cast<uint8_t>((map[static_cast<size_t>(target)].local_slot * 0x20)
                                   + (reg & 0x1f));
  return true;
}

bool UsbSidBackend::slot_is_fmopl(int slot) const
{
  if (!open_ || slot < 0) return false;
  const auto & boards = manager_.Boards();
  if (single_) return !boards.empty() && boards.front().fmoplsid == slot + 1;

  const auto & map = manager_.LogicalMap();
  if (static_cast<size_t>(slot) >= map.size()) return false;
  const auto & entry = map[static_cast<size_t>(slot)];
  if (entry.board_index < 0 || static_cast<size_t>(entry.board_index) >= boards.size()) {
    return false;
  }
  return boards[static_cast<size_t>(entry.board_index)].fmoplsid == entry.local_slot + 1;
}

bool UsbSidBackend::set_sid_select(const uint8_t * tune_sids, uint8_t count)
{
  for (int16_t & slot : chip_to_logical_) slot = -1;
  fm_slot_ = -1;
  select_active_ = (count > 0);
  bool fm_ok = true;
  const int total = num_slots();
  const uint8_t slots = (count > total) ? static_cast<uint8_t>(total) : count;
  for (uint8_t slot = 0; slot < slots; slot++) {
    const uint8_t tune_sid = tune_sids[slot];
    if (tune_sid == kSelectFmOpl) {
      if (slot_is_fmopl(slot)) fm_slot_ = slot;
      else fm_ok = false;
      continue;
    }
    if (tune_sid < 1 || tune_sid > kMaxSids) continue;
    chip_to_logical_[tune_sid - 1] = static_cast<int16_t>(slot);
  }
  return fm_ok;
}

void UsbSidBackend::write(addr_t reg, data_t value, uint16_t cycles)
{
  if (!open_) return;
  int logical_sid;
  uint8_t board_reg;
  if (!remap(reg, logical_sid, board_reg)) return;
  /* Mos6581_8580::io_write() logs the tune-side line; log the board side */
  if (!single_) {
    US_LOG_IF(sid_rw, "  -> board %d sid %d $%02x:%02x\n",
              manager_.LogicalMap()[static_cast<size_t>(logical_sid)].board_index + 1,
              (board_reg >> 5) + 1, board_reg, value);
  }
  /* Straight through: access_overhead is already subtracted upstream
   * (cycles_since_last_event()), do not subtract it again here. */
  manager_.WriteRingCycled(logical_sid, board_reg, value, cycles);
}

data_t UsbSidBackend::read(addr_t reg, uint16_t cycles)
{
  (void)cycles;
  if (!open_) return 0;
  int logical_sid;
  uint8_t board_reg;
  if (!remap(reg, logical_sid, board_reg)) return 0;
  return manager_.Read(logical_sid, board_reg);
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
  manager_.FlushAll();
}

void UsbSidBackend::reset(void)
{
  if (!open_) return;
  /* Throw the queue away before resetting, not after.
   *
   * The two calls below go out through USBSID_SingleWrite(), which bypasses
   * the driver's ring, so they overtake whatever is still queued in it. Without
   * this the board is reset and *then* plays out the backlog that was already
   * on its way: up to ring_size / 4 writes, which is 2048 at the default size,
   * each still carrying its own cycle delay. A stop or a subtune change keeps
   * making noise for as long as that takes to drain, which at ordinary write
   * rates is tens of seconds.
   *
   * Dropping it is right rather than merely convenient: those writes belong to
   * a tune that is being abandoned, and there is nothing to be gained by
   * delivering them late. */
  manager_.ResetRingBufferAll();
  manager_.ResetAllRegistersAll();
  manager_.StartAll();
  manager_.UnMuteAll();
}

void UsbSidBackend::mute(bool muted)
{
  if (!open_) return;
  if (muted) manager_.MuteAll();
  else       manager_.UnMuteAll();
}

} /* namespace usbsid */
