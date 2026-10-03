/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_session.cpp
 * The playback session: engine thread, commands, song lengths, pacing.
 *
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
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
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "session.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;

namespace {

/**
 * @brief A three song PSID: init sets the volume, play bumps a register.
 *
 * @return the file bytes
 */
std::vector<data_t> three_song_tune(void)
{
  std::vector<data_t> f(0x7c, 0);
  memcpy(f.data(), "PSID", 4);
  f[0x05] = 2;                      /* version 2 */
  f[0x07] = 0x7c;                   /* data offset */
  f[0x08] = 0x10; f[0x09] = 0x00;   /* load $1000 */
  f[0x0a] = 0x10; f[0x0b] = 0x00;   /* init $1000 */
  f[0x0c] = 0x10; f[0x0d] = 0x06;   /* play $1006 */
  f[0x0f] = 3;                      /* three songs */
  f[0x11] = 1;                      /* start with the first */
  memcpy(&f[0x16], "Session test", 12);
  const data_t code[] = {
    0xa9, 0x0f, 0x8d, 0x18, 0xd4, 0x60,  /* $1000 lda #15, sta $d418, rts */
    0xee, 0x01, 0xd4, 0x60,              /* $1006 inc $d401, rts */
  };
  f.insert(f.end(), code, code + sizeof(code));
  return f;
}

/**
 * @brief Poll the status until a condition holds or time runs out.
 *
 * @return true when the condition held in time
 */
bool wait_for(Session & s, std::function<bool(const SessionStatus &)> done,
              int timeout_ms = 5000)
{
  for (int t = 0; t < timeout_ms; t += 5) {
    if (done(s.status())) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done(s.status());
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int test_refusals(void)
{
  Session s;
  US_CHECK(!s.play(), "nothing loaded, nothing plays");
  const data_t junk[] = { 'P', 'S', 'I', 'D', 0, 0 };
  US_CHECK(!s.load(junk, sizeof(junk)), "a truncated header is refused");
  US_CHECK(s.status().failed && s.status().error[0] != 0, "with an error to show");

  const std::vector<data_t> tune = three_song_tune();
  US_CHECK(s.load(tune.data(), tune.size()), "the test tune loads");
  US_CHECK(!s.status().failed, "and clears the error");
  US_CHECK(!s.play(), "but without an output it does not play");
  US_CHECK_EQ_STR(s.tune().name, "Session test", "the tune is readable before play");
  return 0;
}

int test_song_end_and_advance(void)
{
  Session s;
  NullSessionSink * sink = new NullSessionSink(false);
  s.set_sink(std::unique_ptr<SessionSink>(sink));
  SessionOptions o;
  o.default_song_ms = 200;
  s.set_options(o);

  const std::vector<data_t> tune = three_song_tune();
  US_CHECK(s.load(tune.data(), tune.size()), "loads");
  US_CHECK(s.play(), "plays");
  US_CHECK(wait_for(s, [](const SessionStatus & st) { return st.ended; }),
           "unpaced, all three 200 ms songs play out");
  const SessionStatus st = s.status();
  US_CHECK_EQ_U(st.song, 3u, "ending on the last song");
  US_CHECK(!st.playing, "and no longer playing");
  US_CHECK(sink->writes() > 20, "the play routine wrote: %u", sink->writes());

  s.stop();
  US_CHECK(!s.status().playing, "stopped");
  return 0;
}

int test_paced_controls(void)
{
  Session s;
  s.set_sink(std::unique_ptr<SessionSink>(new NullSessionSink(true)));
  SessionOptions o;
  o.default_song_ms = 0;  /* forever */
  s.set_options(o);

  const std::vector<data_t> tune = three_song_tune();
  s.load(tune.data(), tune.size());
  US_CHECK(s.play(), "plays paced");
  sleep_ms(400);
  SessionStatus st = s.status();
  /* 50.125 frames a second: 400 ms is 20 frames, wide margin for CI */
  US_CHECK(st.frames >= 12 && st.frames <= 30, "paced at about 50 Hz: %llu frames",
           static_cast<unsigned long long>(st.frames));
  US_CHECK(st.length_ms == 0, "no length, plays until stopped");

  s.pause(true);
  US_CHECK(wait_for(s, [](const SessionStatus & x) { return x.paused; }, 1000), "pauses");
  const uint64_t held = s.status().frames;
  sleep_ms(150);
  US_CHECK_EQ_U(s.status().frames, held, "no frames while paused");
  s.pause(false);
  US_CHECK(wait_for(s, [held](const SessionStatus & x) { return x.frames > held + 2; }, 2000),
           "resumes");

  s.next_subtune();
  US_CHECK(wait_for(s, [](const SessionStatus & x) { return x.song == 2; }, 1000),
           "next subtune");
  US_CHECK(s.status().elapsed_ms < 300, "with its clock from zero");
  s.previous_subtune();
  s.previous_subtune();
  US_CHECK(wait_for(s, [](const SessionStatus & x) { return x.song == 3; }, 1000),
           "previous wraps to the last");
  s.select_song(1);
  US_CHECK(wait_for(s, [](const SessionStatus & x) { return x.song == 1; }, 1000),
           "select a song by number");

  s.seek(10000);
  US_CHECK(wait_for(s, [](const SessionStatus & x) {
             return !x.seeking && x.elapsed_ms >= 10000; }, 2000),
           "a ten second seek takes well under real time");

  s.set_mute(1, 2, true);
  s.set_mute(2, 0, true);
  US_CHECK(wait_for(s, [](const SessionStatus & x) {
             return x.voice_mute[0] == 0x02 && x.chip_mute == 0x02; }, 1000),
           "mutes are recorded");
  s.next_subtune();
  wait_for(s, [](const SessionStatus & x) { return x.song == 2; }, 1000);
  US_CHECK(s.status().voice_mute[0] == 0x02, "and kept across subtunes");

  s.stop();
  US_CHECK(!s.status().playing, "stops");
  return 0;
}

int test_program_without_roms(void)
{
  Session s;
  s.set_sink(std::unique_ptr<SessionSink>(new NullSessionSink(false)));
  s.set_roms(nullptr, nullptr, nullptr);
  /* 10 SYS2061, then a loop writing the SID volume */
  const data_t prg[] = {
    0x01, 0x08, 0x0b, 0x08, 0x0a, 0x00, 0x9e, 0x32, 0x30, 0x36, 0x31, 0x00, 0x00, 0x00,
    0xa9, 0x0f, 0x8d, 0x18, 0xd4, 0x4c, 0x0d, 0x08,
  };
  US_CHECK(s.load(prg, sizeof(prg)), "a program loads");
  US_CHECK(s.play(), "and starts on the stub KERNAL");
  US_CHECK(wait_for(s, [](const SessionStatus & x) { return x.frames > 10; }, 2000), "runs");
  const SessionStatus st = s.status();
  US_CHECK(st.is_prg && st.length_ms == 0, "a program has no song length");
  s.stop();

  /* BASIC without a SYS line cannot start without ROMs, and says why */
  const data_t basic[] = {
    0x01, 0x08, 0x09, 0x08, 0x0a, 0x00, 0x99, 0x22, 0x41, 0x22, 0x00, 0x00, 0x00,
  };
  US_CHECK(s.load(basic, sizeof(basic)), "a BASIC program loads");
  US_CHECK(s.status().needs_roms, "flagged as needing ROMs");
  US_CHECK(!s.play(), "and does not start");
  US_CHECK(strstr(s.status().error, "ROM") != nullptr, "the error names the ROMs");
  return 0;
}

} /* namespace */

int us_test_session(void)
{
  US_TEST_BEGIN("session");

  test_refusals();
  test_song_end_and_advance();
  test_paced_controls();
  test_program_without_roms();

  US_TEST_END("session");
}

US_TEST_MAIN(us_test_session)
