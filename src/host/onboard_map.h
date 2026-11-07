/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * onboard_map.h
 * Pure decisions of the onboard player front end, no driver types
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
#ifndef _US_HOST_ONBOARD_MAP_H_
#define _US_HOST_ONBOARD_MAP_H_

#include <cstdint>

namespace usbsid {

/** @brief Board play time meaning "no limit" */
constexpr uint32_t kOnboardNoLimit = 0xFFFFFFFFu;

/** @brief Extra board play time while the CLI sends NEXT at the song end */
constexpr uint32_t kOnboardAdvanceSlackMs = 3000u;

/** @brief Poll reading older than this stops counting as stale */
constexpr uint32_t kOnboardStaleMs = 1000u;

/**
 * @brief How to start a song on the board
 *
 * PlayerLoad(load_arg) starts the tune's default song for 0, song load_arg + 1 otherwise.
 * prev_after: send PREV once after START, song 1 has no spelling when the default is not 1.
 */
struct OnboardLoadPlan {
  uint8_t load_arg;
  bool prev_after;
};

/**
 * @brief Load argument for a song counting from 1
 *
 * @param song          wanted song, 0 for the tune's own
 * @param default_song  the tune's default song, counting from 1
 */
inline OnboardLoadPlan onboard_load_plan(uint16_t song, uint16_t default_song)
{
  if (song == 0 || song == default_song) return { 0, false };
  if (song == 1) return { 1, true };
  return { static_cast<uint8_t>(song - 1), false };
}

/** @brief Song after cur, wraps to 1 */
inline uint16_t onboard_next_song(uint16_t cur, uint16_t songs)
{
  return (cur >= songs) ? 1 : static_cast<uint16_t>(cur + 1);
}

/** @brief Song before cur, wraps to the last */
inline uint16_t onboard_prev_song(uint16_t cur, uint16_t songs)
{
  return (cur <= 1) ? songs : static_cast<uint16_t>(cur - 1);
}

/** @brief What to do at a poll reading */
enum class OnboardEnd { None, Advance, Done };

/**
 * @brief End of song decision
 *
 * @param ms      board play time
 * @param len_ms  song length, 0 for no end check
 * @param more    a following song is played by the CLI
 */
inline OnboardEnd onboard_end_action(uint32_t ms, uint32_t len_ms, bool more)
{
  if (len_ms == 0 || ms < len_ms) return OnboardEnd::None;
  return more ? OnboardEnd::Advance : OnboardEnd::Done;
}

/**
 * @brief Max play time to give the board for a song
 *
 * @param len_ms   song length, 0 for none
 * @param advance  the CLI sends NEXT at len_ms, the board must outlast that
 */
inline uint32_t onboard_board_max_ms(uint32_t len_ms, bool advance)
{
  if (len_ms == 0) return kOnboardNoLimit;
  return advance ? len_ms + kOnboardAdvanceSlackMs : len_ms;
}

/**
 * @brief A reading right after a song change can still be the old song's
 *
 * @param before_ms  last reading before the change
 * @param ms         the latest reading
 * @param since_ms   time since the change
 */
inline bool onboard_reading_stale(uint32_t before_ms, uint32_t ms, uint32_t since_ms)
{
  return ms >= before_ms && since_ms < kOnboardStaleMs;
}

} /* namespace usbsid */

#endif /* _US_HOST_ONBOARD_MAP_H_ */
