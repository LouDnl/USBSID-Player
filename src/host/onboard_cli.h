/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * onboard_cli.h
 * The onboard player front end of the CLI (--output onboard)
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
#ifndef _US_HOST_ONBOARD_CLI_H_
#define _US_HOST_ONBOARD_CLI_H_

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "sidfile.h"

namespace usbsid {

/** @brief One-shot commands, any combination, sent in this order */
enum OnboardOneShot : unsigned {
  kOnboardStop  = 1u << 0,
  kOnboardPause = 1u << 1,
  kOnboardNext  = 1u << 2,
  kOnboardPrev  = 1u << 3,
};

/**
 * @brief Everything the onboard front end needs from the command line
 */
struct OnboardOptions {
  const char * board = nullptr;    /**< serial or 1 based index, nullptr for the first board */
  unsigned one_shots = 0;          /**< OnboardOneShot bits, onboard_command() only */

  /* run_onboard() only */
  const uint8_t * data = nullptr;  /**< the tune file as given */
  size_t size = 0;
  bool is_sid = true;              /**< false for a program */
  uint16_t songs = 1;
  uint16_t default_song = 1;
  uint8_t sid_count = 1;
  uint16_t song = 0;               /**< -s, counting from 1, 0 for the tune's own */
  int seconds = 0;                 /**< -t */
  bool single = false;             /**< -1 */
  bool force_two = false;          /**< -f */
  bool detach = false;             /**< --detach */
  bool has_mute = false;
  uint8_t mute_mask[kMaxSids] = { 0 }; /**< -m, bits 0 to 2 per chip */
  /** Song length in ms for a song counting from 1, 0 for unknown or no check */
  std::function<uint32_t(uint16_t)> song_length_ms;
};

/**
 * @brief Send the one-shot commands to a board and return, never uploads
 *
 * @return process exit code
 */
int onboard_command(const OnboardOptions & opt);

/**
 * @brief Upload the tune to a board, start it and follow it until the end or a key
 *
 * @param opt   options
 * @param stop  set by the signal handlers
 * @return process exit code
 */
int run_onboard(const OnboardOptions & opt, volatile sig_atomic_t * stop);

} /* namespace usbsid */

#endif /* _US_HOST_ONBOARD_CLI_H_ */
