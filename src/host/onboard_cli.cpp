/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * onboard_cli.cpp
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

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "USBSID.h"
#include "USBSID_Manager.h"
#include "console.h"
#include "onboard_cli.h"
#include "onboard_map.h"

namespace usbsid {

namespace {

using USBSID_NS::USBSID_Class;
using Clock = std::chrono::steady_clock;

/** @brief Poll interval of the board play time */
constexpr int kPollMs = 250;

/** @brief Failed polls in a row before the board counts as gone */
constexpr int kMaxFailedPolls = 8;

/** @brief Sockets the board has, PlayerMute counts chips from 1 */
constexpr uint8_t kBoardChips = 4;

/** @brief Case insensitive compare, serials are uppercase hex */
bool str_ieq(const std::string & a, const char * b)
{
  size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; i++) {
    if (toupper(static_cast<unsigned char>(a[i])) != toupper(static_cast<unsigned char>(b[i]))) return false;
  }
  return i == a.size() && b[i] == '\0';
}

/** @brief Milliseconds as m:ss.t for the status line, static buffer */
const char * fmt_ms(uint32_t ms)
{
  static char buf[24];
  snprintf(buf, sizeof(buf), "%u:%02u.%u", ms / 60000u, (ms / 1000u) % 60u, (ms / 100u) % 10u);
  return buf;
}

/** @brief Send one player command, report a failure, true when sent */
bool send_cmd(USBSID_Class & dev, uint8_t cmd)
{
  if (dev.USBSID_PlayerCommand(cmd) < 0) {
    printf("\n  error sending player command $%02X\n", cmd);
    return false;
  }
  return true;
}

/** @brief Send the max play time, report a failure, true when sent */
bool send_playtime(USBSID_Class & dev, uint32_t ms)
{
  if (dev.USBSID_PlayerSetPlaytime(ms) < 0) {
    printf("\n  error sending the play time\n");
    return false;
  }
  return true;
}

/**
 * @brief Open exactly one board without touching its SID state
 *
 * @param dev   driver instance, not yet initialised
 * @param spec  serial or 1 based index as printed by --list-boards, nullptr for the first
 * @return true when the board is open
 */
bool open_board(USBSID_Class & dev, const char * spec)
{
  const auto found = USBSID_Manager::Enumerate();
  if (found.empty()) {
    printf("no USBSID-Pico boards found\n");
    return false;
  }
  size_t sel = found.size();
  if (spec == nullptr) {
    sel = 0;
  } else {
    /* Serial wins over index */
    for (size_t i = 0; i < found.size(); i++) {
      if (!found[i].serial.empty() && str_ieq(found[i].serial, spec)) { sel = i; break; }
    }
    if (sel == found.size() && spec[0] != '\0') {
      char * end = nullptr;
      const long idx = strtol(spec, &end, 10);
      if (*end == '\0' && idx >= 1 && idx <= static_cast<long>(found.size())) {
        sel = static_cast<size_t>(idx - 1);
      }
    }
  }
  if (sel == found.size()) {
    printf("no board matches '%s', available boards:\n", spec);
    for (size_t i = 0; i < found.size(); i++) {
      printf("  board %zu: serial %s\n", i + 1,
             found[i].serial.empty() ? "<unknown>" : found[i].serial.c_str());
    }
    return false;
  }

  /* Serial survives a replug, the index is the fallback when it was unreadable */
  if (!found[sel].serial.empty()) dev.USBSID_SetTargetSerial(found[sel].serial);
  else dev.USBSID_SetTargetIndex(static_cast<int>(sel));
  /* Passive: no mute, bus clear or clock query that would disturb a playing tune */
  dev.USBSID_SetPassive(true);
  if (dev.USBSID_Init(false, false) < 0 || !dev.USBSID_isOpen()) {
    printf("cannot open the board\n");
    dev.USBSID_Close();
    return false;
  }
  printf("  board    : %zu, serial %s\n", sel + 1,
         found[sel].serial.empty() ? "<unknown>" : found[sel].serial.c_str());
  return true;
}

/** @brief Close the board, the driver wants a moment after the last command */
void close_board(USBSID_Class & dev)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  dev.USBSID_Close();
}

/** @brief False when the firmware reports it has no onboard player, unknown (-1) continues */
bool has_onboard_player(USBSID_Class & dev)
{
  const int features = dev.USBSID_GetFeatures();
  if (features >= 0 && (features & USBSID_NS::US_FEATURE_EMULATOR) == 0) {
    printf("no onboard player, flash an _em firmware\n");
    return false;
  }
  return true;
}

} /* namespace */

int onboard_command(const OnboardOptions & opt)
{
  auto dev = std::make_unique<USBSID_Class>();
  if (!open_board(*dev, opt.board)) return 1;
  if (!has_onboard_player(*dev)) { close_board(*dev); return 1; }

  bool ok = true;
  if (opt.one_shots & kOnboardStop) {
    printf("  stopping playback\n");
    ok = send_cmd(*dev, USBSID_NS::SID_PLAYER_STOP) && ok;
  }
  if (opt.one_shots & kOnboardPause) {
    printf("  (un)pausing playback\n");
    ok = send_cmd(*dev, USBSID_NS::SID_PLAYER_PAUSE) && ok;
  }
  if (opt.one_shots & kOnboardNext) {
    printf("  playing next subtune\n");
    ok = send_cmd(*dev, USBSID_NS::SID_PLAYER_NEXT) && ok;
  }
  if (opt.one_shots & kOnboardPrev) {
    printf("  playing previous subtune\n");
    ok = send_cmd(*dev, USBSID_NS::SID_PLAYER_PREV) && ok;
  }
  close_board(*dev);
  return ok ? 0 : 1;
}

int run_onboard(const OnboardOptions & opt, volatile sig_atomic_t * stop)
{
  auto devp = std::make_unique<USBSID_Class>();
  USBSID_Class & dev = *devp;
  if (!open_board(dev, opt.board)) return 1;
  if (!has_onboard_player(dev)) { close_board(dev); return 1; }

  const uint16_t songs = (opt.songs == 0) ? 1 : opt.songs;
  const uint16_t default_song = (opt.default_song == 0) ? 1 : opt.default_song;
  uint16_t cur = (opt.song != 0) ? opt.song : default_song;

  /* The CLI advances songs itself only while attached, without -t and without -1 */
  const bool may_advance = !opt.detach && opt.seconds <= 0 && !opt.single;
  const auto len_of = [&](uint16_t song) -> uint32_t {
    if (opt.seconds > 0) return static_cast<uint32_t>(opt.seconds) * 1000u;
    return opt.song_length_ms ? opt.song_length_ms(song) : 0u;
  };
  const auto more_after = [&](uint16_t song) -> bool { return may_advance && song < songs; };
  const auto board_max = [&](uint16_t song) -> uint32_t {
    return onboard_board_max_ms(len_of(song), more_after(song));
  };

  /* Upload and start, same order as send_sid */
  const OnboardLoadPlan plan = onboard_load_plan(opt.song, default_song);
  printf("  onboard  : uploading %zu bytes\n", opt.size);
  send_cmd(dev, USBSID_NS::SID_PLAYER_STOP);
  if (dev.USBSID_UploadTune(opt.data, opt.size,
        opt.is_sid ? USBSID_NS::UPLOAD_FILE_SID : USBSID_NS::UPLOAD_FILE_PRG) < 0) {
    printf("error uploading the tune\n");
    close_board(dev);
    return 1;
  }
  if (!send_playtime(dev, board_max(cur))) { close_board(dev); return 1; }
  if (opt.force_two) send_cmd(dev, USBSID_NS::SID_PLAYER_TWO);
  if (dev.USBSID_PlayerLoad(plan.load_arg) < 0) {
    printf("error loading the tune\n");
    close_board(dev);
    return 1;
  }
  send_cmd(dev, USBSID_NS::SID_PLAYER_START);
  if (plan.prev_after) send_cmd(dev, USBSID_NS::SID_PLAYER_PREV);

  /* Voice mute state per chip, bits 0 to 2 */
  uint8_t muted[kBoardChips] = { 0, 0, 0, 0 };
  if (opt.has_mute) {
    for (uint8_t c = 0; c < kMaxSids; c++) {
      if (opt.mute_mask[c] == 0) continue;
      if (c >= kBoardChips) {
        printf("  mute     : chip %u ignored, the board has %u\n", c + 1, kBoardChips);
        continue;
      }
      for (uint8_t v = 0; v < 3; v++) {
        if ((opt.mute_mask[c] & (1u << v)) == 0) continue;
        dev.USBSID_PlayerMute(static_cast<uint8_t>(c + 1), static_cast<uint8_t>(v + 1), true);
        muted[c] = static_cast<uint8_t>(muted[c] | (1u << v));
      }
    }
  }

  printf("  playing  : song %u of %u on the board\n", cur, songs);

  if (opt.detach) {
    printf("  detached : the board keeps playing\n");
    close_board(dev);
    return 0;
  }

  RawConsole console;
  const bool keys = RawConsole::interactive();
  if (keys) {
    printf("  keys     : space pause, n/right p/left change subtune, r restart song,\n"
           "             f fast forward, 1/2/3 toggle mute of a voice, c select sid,\n"
           "             s/q/Esc/ctrl-c quit\n");
  } else {
    printf("  press ctrl-c to stop\n");
  }

  const uint8_t chips = static_cast<uint8_t>(
    (opt.sid_count == 0) ? 1 : (opt.sid_count > kBoardChips ? kBoardChips : opt.sid_count));
  uint8_t chip = 1;
  bool paused = false;
  bool ffwd = false;
  uint32_t last_ms = 0;
  int failed = 0;
  int exit_code = 0;
  bool lost = false;

  /* Right after a song change a reading can still belong to the old song */
  bool settling = false;
  uint32_t before_change = 0;
  Clock::time_point changed_at = Clock::now();
  const auto song_changed = [&]() {
    before_change = last_ms;
    last_ms = 0;
    changed_at = Clock::now();
    settling = true;
    paused = false;
  };

  const auto print_status = [&]() {
    if (!keys) return;
    const uint32_t len = len_of(cur);
    char total[32] = { 0 };
    if (len > 0) snprintf(total, sizeof(total), " / %s", fmt_ms(len));
    if (paused) {
      printf("\r  ||  %s%s  song %u/%u\033[K", fmt_ms(last_ms), total, cur, songs);
    } else {
      const uint8_t mask = muted[chip - 1];
      char voices[24] = { 0 };
      if (mask != 0 || chips > 1) {
        snprintf(voices, sizeof(voices), "  sid %u %c%c%c", chip,
                 (mask & 1) ? '-' : '1', (mask & 2) ? '-' : '2', (mask & 4) ? '-' : '3');
      }
      printf("\r  %s  %s%s  song %u/%u%s\033[K", ffwd ? ">> seeking" : " >",
             fmt_ms(last_ms), total, cur, songs, voices);
    }
    fflush(stdout);
  };

  Clock::time_point next_poll = Clock::now();
  bool quit = false;
  bool done = false;
  while (!*stop && !quit) {
    if (keys) {
      const int k = console_key();
      switch (k) {
        case ' ':
          if (send_cmd(dev, USBSID_NS::SID_PLAYER_PAUSE)) paused = !paused;
          print_status();
          break;
        case 'n': case kKeyRight:
        case 'p': case kKeyLeft: {
          if (songs <= 1) break;
          const bool fwd = (k == 'n' || k == kKeyRight);
          send_cmd(dev, fwd ? USBSID_NS::SID_PLAYER_NEXT : USBSID_NS::SID_PLAYER_PREV);
          cur = fwd ? onboard_next_song(cur, songs) : onboard_prev_song(cur, songs);
          send_playtime(dev, board_max(cur));
          song_changed();
          print_status();
          break;
        }
        case 'r': {
          /* Load again without a new upload, the tune stays in the board's RAM */
          if (paused) send_cmd(dev, USBSID_NS::SID_PLAYER_PAUSE);
          const OnboardLoadPlan again = onboard_load_plan(cur, default_song);
          send_playtime(dev, board_max(cur));
          if (dev.USBSID_PlayerLoad(again.load_arg) < 0) printf("\n  error loading the tune\n");
          if (again.prev_after) send_cmd(dev, USBSID_NS::SID_PLAYER_PREV);
          song_changed();
          print_status();
          break;
        }
        case 'f':
          if (dev.USBSID_PlayerFfwd(!ffwd) >= 0) ffwd = !ffwd;
          print_status();
          break;
        case '1': case '2': case '3': {
          const uint8_t voice = static_cast<uint8_t>(k - '0');
          const uint8_t bit = static_cast<uint8_t>(1u << (voice - 1));
          const bool now_muted = (muted[chip - 1] & bit) == 0;
          if (dev.USBSID_PlayerMute(chip, voice, now_muted) >= 0) {
            muted[chip - 1] = now_muted ? static_cast<uint8_t>(muted[chip - 1] | bit)
                                        : static_cast<uint8_t>(muted[chip - 1] & ~bit);
          }
          print_status();
          break;
        }
        case 'c':
          chip = static_cast<uint8_t>((chip % chips) + 1);
          print_status();
          break;
        case 's': case 'q': case 27: quit = true; break;
        default: break;
      }
    }

    if (dev.USBSID_IsDeviceLost()) { lost = true; break; }

    if (!quit && Clock::now() >= next_poll) {
      next_poll = Clock::now() + std::chrono::milliseconds(kPollMs);
      const long ms = dev.USBSID_PlayerTime();
      if (ms < 0) {
        if (++failed >= kMaxFailedPolls) { lost = true; break; }
      } else {
        failed = 0;
        last_ms = static_cast<uint32_t>(ms);
        if (settling) {
          const uint32_t since = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - changed_at).count());
          settling = onboard_reading_stale(before_change, last_ms, since);
        }
        print_status();
        if (!settling && !paused) {
          const OnboardEnd end = onboard_end_action(last_ms, len_of(cur), more_after(cur));
          if (end == OnboardEnd::Advance) {
            send_cmd(dev, USBSID_NS::SID_PLAYER_NEXT);
            cur = onboard_next_song(cur, songs);
            send_playtime(dev, board_max(cur));
            song_changed();
          } else if (end == OnboardEnd::Done) {
            printf("\r  done     %s, %s        \n", fmt_ms(last_ms),
                   (opt.seconds > 0) ? "the time limit has passed"
                   : opt.single ? "the song has played out" : "the last song has played out");
            quit = true;
            done = true;
          }
        }
      }
    }

    if (!quit) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (lost) {
    printf("\n  lost contact with the board\n");
    exit_code = 1;
  } else {
    /* Leave the board silent, a paused board ignores STOP */
    if (ffwd) dev.USBSID_PlayerFfwd(false);
    if (paused) dev.USBSID_PlayerCommand(USBSID_NS::SID_PLAYER_PAUSE);
    dev.USBSID_PlayerCommand(USBSID_NS::SID_PLAYER_STOP);
    if (keys && !done) printf("\n");
  }
  close_board(dev);
  return exit_code;
}

} /* namespace usbsid */
