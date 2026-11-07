/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * test_onboard.cpp
 * The onboard player decisions: load plan, song wrap, end action.
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

#include <chrono>

#include <cstdio>

#include "onboard_map.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;

namespace {

void test_load_plan(void)
{
  /* No -s, or the default song: the file's own default */
  OnboardLoadPlan p = onboard_load_plan(0, 3);
  US_CHECK(p.load_arg == 0 && !p.prev_after, "no song loads the default");
  p = onboard_load_plan(3, 3);
  US_CHECK(p.load_arg == 0 && !p.prev_after, "the default song loads the default");
  p = onboard_load_plan(1, 1);
  US_CHECK(p.load_arg == 0 && !p.prev_after, "song 1 of a default 1 tune loads the default");

  /* Song 2 and up are x = N - 1 */
  p = onboard_load_plan(2, 1);
  US_CHECK(p.load_arg == 1 && !p.prev_after, "song 2 is x 1");
  p = onboard_load_plan(5, 1);
  US_CHECK(p.load_arg == 4 && !p.prev_after, "song 5 is x 4");

  /* Song 1 has no spelling when the default is not 1 */
  p = onboard_load_plan(1, 4);
  US_CHECK(p.load_arg == 1 && p.prev_after, "song 1 of a default 4 tune is x 1 then PREV");
}

void test_wrap(void)
{
  US_CHECK_EQ_U(onboard_next_song(1, 3), 2, "next");
  US_CHECK_EQ_U(onboard_next_song(3, 3), 1, "next wraps");
  US_CHECK_EQ_U(onboard_prev_song(2, 3), 1, "prev");
  US_CHECK_EQ_U(onboard_prev_song(1, 3), 3, "prev wraps");
  US_CHECK_EQ_U(onboard_next_song(1, 1), 1, "single song next");
  US_CHECK_EQ_U(onboard_prev_song(1, 1), 1, "single song prev");
}

void test_end(void)
{
  US_CHECK(onboard_end_action(99999, 0, true) == OnboardEnd::None, "no length, no end");
  US_CHECK(onboard_end_action(999, 1000, true) == OnboardEnd::None, "before the length");
  US_CHECK(onboard_end_action(1000, 1000, true) == OnboardEnd::Advance, "at the length, more songs");
  US_CHECK(onboard_end_action(1000, 1000, false) == OnboardEnd::Done, "at the length, last song");
  US_CHECK(onboard_end_action(5000, 1000, false) == OnboardEnd::Done, "past the length");
}

void test_board_max(void)
{
  US_CHECK_EQ_U(onboard_board_max_ms(0, true), kOnboardNoLimit, "no length is no limit");
  US_CHECK_EQ_U(onboard_board_max_ms(0, false), kOnboardNoLimit, "no length, last song");
  US_CHECK_EQ_U(onboard_board_max_ms(60000, true), 63000, "advancing adds slack");
  US_CHECK_EQ_U(onboard_board_max_ms(60000, false), 60000, "last song is exact");
}

void test_stale(void)
{
  US_CHECK(onboard_reading_stale(5000, 5100, 200), "same or higher reading is old");
  US_CHECK(!onboard_reading_stale(5000, 120, 200), "lower reading is the new song");
  US_CHECK(!onboard_reading_stale(5000, 5100, 1000), "after a second it counts");
}

} /* namespace */

int us_test_onboard(void)
{
  US_TEST_BEGIN("onboard");

  test_load_plan();
  test_wrap();
  test_end();
  test_board_max();
  test_stale();

  US_TEST_END("onboard");
}

US_TEST_MAIN(us_test_onboard)
