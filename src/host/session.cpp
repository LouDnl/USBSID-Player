/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session.cpp
 * See session.h.
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

#include "session.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "prgfile.h"
#include "vic_timing.h"

namespace usbsid {

namespace {

/* How long an idle engine (paused, ended) sleeps between command checks */
constexpr auto kIdleWait = std::chrono::milliseconds(20);

/** @brief Writes from a seek go nowhere. */
NullSidBackend & seek_backend(void)
{
  static NullSidBackend backend;
  return backend;
}

/** @brief True for a PSID or RSID header. */
bool is_sid_file(const data_t * bytes, size_t len)
{
  return len >= 4 && (bytes[0] == 'P' || bytes[0] == 'R') &&
         bytes[1] == 'S' && bytes[2] == 'I' && bytes[3] == 'D';
}

} /* namespace */

Session::Session(void)
  : machine_(new Machine()), player_(*machine_), rom_store_(new RomStore())
{
}

Session::~Session(void)
{
  stop();
  if (sink_) sink_->detach(*machine_);
}

void Session::set_sink(std::unique_ptr<SessionSink> sink)
{
  stop();
  std::lock_guard<std::mutex> lk(lock_);
  if (sink_) sink_->detach(*machine_);
  sink_ = std::move(sink);
  update_status();
}

void Session::set_options(const SessionOptions & options)
{
  run_or_queue([this, options](void) { options_ = options; });
}

void Session::set_roms(const data_t * basic, const data_t * kernal, const data_t * chargen)
{
  std::lock_guard<std::mutex> lk(lock_);
  rom_store_->set(basic, kernal, chargen);
  custom_roms_ = true;
}

void Session::default_roms(void)
{
  std::lock_guard<std::mutex> lk(lock_);
  custom_roms_ = false;
}

void Session::set_song_lengths(const char * text, size_t len)
{
  std::lock_guard<std::mutex> lk(lock_);
  lengths_db_.assign(text, text + ((text != nullptr) ? len : 0));
}

bool Session::load(const data_t * bytes, size_t len, uint16_t song)
{
  stop();
  std::lock_guard<std::mutex> lk(lock_);
  loaded_ = false;
  ended_ = failed_ = false;
  error_[0] = 0;
  lengths_ = SongLengths{};
  if (bytes == nullptr || len == 0) {
    fail("no file");
    update_status();
    return false;
  }

  bytes_.assign(bytes, bytes + len);
  is_sid_ = is_sid_file(bytes_.data(), bytes_.size());
  start_song_ = song;

  const bool ok = is_sid_ ? player_.load_sid(bytes_.data(), bytes_.size(), song)
                          : player_.load_prg(bytes_.data(), bytes_.size());
  if (!ok) {
    fail(is_sid_ ? "not a valid SID file" : "not a valid program");
    update_status();
    return false;
  }

  if (is_sid_ && !lengths_db_.empty()) {
    char key[33];
    songlengths_key(bytes_.data(), bytes_.size(), key);
    lengths_ = songlengths_lookup(lengths_db_.data(), lengths_db_.size(), key);
  }
  loaded_ = true;
  frame_ = song_frame0_ = 0;
  update_status();
  return true;
}

bool Session::play(void)
{
  stop();
  {
    std::lock_guard<std::mutex> lk(lock_);
    if (!begin()) {
      update_status();
      return false;
    }
    stopping_ = false;
    running_ = true;
    update_status();
  }
  thread_ = std::thread(&Session::engine, this);
  return true;
}

void Session::stop(void)
{
  if (running_) {
    stopping_ = true;
    wake_.notify_all();
    join();
  }
  std::lock_guard<std::mutex> lk(lock_);
  commands_.clear();
  if (sink_ && player_.playing()) sink_->silence(*machine_);
  player_.stop();
  paused_ = false;
  seek_to_ = 0;
  update_status();
}

void Session::pause(bool paused)
{
  run_or_queue([this, paused](void) {
    if (!player_.playing() || paused == paused_ || ended_ || failed_) return;
    paused_ = paused;
    player_.pause(paused);
    if (paused) {
      sink_->silence(*machine_);
    } else {
      sink_->restore(*machine_);
      pacer_.rebase(frame_);
    }
  });
}

void Session::next_subtune(void)
{
  run_or_queue([this](void) {
    if (!player_.playing() || player_.songs() <= 1) return;
    player_.next_subtune();
    after_song_start();
  });
}

void Session::previous_subtune(void)
{
  run_or_queue([this](void) {
    if (!player_.playing() || player_.songs() <= 1) return;
    player_.previous_subtune();
    after_song_start();
  });
}

void Session::select_song(uint16_t song)
{
  run_or_queue([this, song](void) {
    if (!player_.playing() || player_.is_prg()) return;
    if (player_.restart_song(song)) after_song_start();
  });
}

void Session::seek(uint32_t ms)
{
  run_or_queue([this, ms](void) {
    if (!player_.playing() || ended_ || failed_ || pacer_.frame_rate() <= 0.0) return;
    const uint64_t target = song_frame0_ +
      static_cast<uint64_t>(static_cast<double>(ms) * pacer_.frame_rate() / 1000.0);
    if (target <= frame_) return;
    if (seek_to_ == 0) {
      sink_->silence(*machine_);
      machine_->set_sid_backend(seek_backend());
    }
    seek_to_ = target;
  });
}

void Session::set_mute(uint8_t chip, uint8_t voice, bool muted)
{
  if (chip < 1 || chip > kMaxSids || voice > 3) return;
  run_or_queue([this, chip, voice, muted](void) {
    const uint8_t c = static_cast<uint8_t>(chip - 1);
    if (voice == 0) {
      if (muted) chip_mute_ = static_cast<uint16_t>(chip_mute_ | (1u << c));
      else chip_mute_ = static_cast<uint16_t>(chip_mute_ & ~(1u << c));
      machine_->sid().set_chip_mute(chip, muted);
    } else {
      const uint8_t bit = static_cast<uint8_t>(1u << (voice - 1));
      if (muted) voice_mute_[c] = static_cast<uint8_t>(voice_mute_[c] | bit);
      else voice_mute_[c] = static_cast<uint8_t>(voice_mute_[c] & ~bit);
      machine_->sid().set_voice_mute(chip, voice, muted);
    }
  });
}

void Session::set_engine_thread_setup(std::function<void(void)> setup)
{
  std::lock_guard<std::mutex> lk(lock_);
  thread_setup_ = std::move(setup);
}

SessionStatus Session::status(void) const
{
  std::lock_guard<std::mutex> lk(lock_);
  return status_;
}

/* ---- engine ------------------------------------------------------------- */

void Session::engine(void)
{
  if (thread_setup_) thread_setup_();

  std::unique_lock<std::mutex> lk(lock_);
  while (!stopping_) {
    while (!commands_.empty()) {
      std::function<void(void)> command = std::move(commands_.front());
      commands_.pop_front();
      command();
    }
    if (stopping_) break;

    if (paused_ || ended_ || failed_ || !player_.playing()) {
      update_status();
      wake_.wait_for(lk, kIdleWait);
      continue;
    }

    player_.run_frame();
    ++frame_;

    const bool seeking = (seek_to_ != 0);
    if (seeking) {
      if (frame_ >= seek_to_) finish_seek();
    } else {
      const uint32_t length = song_length_ms();
      const double rate = pacer_.frame_rate();
      if (length > 0 && rate > 0.0 &&
          1000.0 * static_cast<double>(frame_ - song_frame0_) / rate >=
          static_cast<double>(length)) {
        if (options_.auto_advance && !player_.is_prg() &&
            player_.song() < player_.songs()) {
          player_.next_subtune();
          after_song_start();
        } else {
          ended_ = true;
          sink_->silence(*machine_);
        }
      }
    }
    update_status();

    lk.unlock();
    bool ok = true;
    if (!seeking) {
      ok = sink_->frame_done(stopping_);
      if (ok && sink_->wall_clock() && !stopping_) pacer_.wait_for_frame(frame_);
    }
    lk.lock();
    if (!ok) fail("the output stopped working");
  }
  update_status();
}

void Session::join(void)
{
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void Session::run_or_queue(std::function<void(void)> command)
{
  std::lock_guard<std::mutex> lk(lock_);
  if (running_) {
    commands_.push_back(std::move(command));
    wake_.notify_all();
  } else {
    command();
    update_status();
  }
}

/**
 * @brief Set up the machine, output and tune for play(). Lock held.
 *
 * @return false with error_ set when anything refused
 */
bool Session::begin(void)
{
  ended_ = failed_ = false;
  error_[0] = 0;
  paused_ = false;
  seek_to_ = 0;
  frame_ = song_frame0_ = 0;

  if (!loaded_) { fail("nothing loaded"); return false; }
  if (!sink_) { fail("no output chosen"); return false; }

  machine_->mmu().roms = custom_roms_ ? rom_store_->roms() : Roms{};
  if (!sink_->attach(*machine_)) { fail("the output is not available"); return false; }

  const bool loaded = is_sid_
    ? player_.load_sid(bytes_.data(), bytes_.size(), start_song_)
    : player_.load_prg(bytes_.data(), bytes_.size());
  if (!loaded) { fail("cannot load the file"); return false; }

  if (!sink_->prepare(*machine_, is_sid_ ? &player_.tune() : nullptr)) {
    fail("the output cannot play this file");
    return false;
  }

  const bool started = player_.is_prg() ? player_.init_prg() : player_.init_tune(0);
  if (!started) {
    fail(player_.needs_roms() ? "needs the C64 BASIC and KERNAL ROMs" : "cannot start");
    return false;
  }

  pacer_.start(vic_cycles_per_frame(machine_->video_model()),
               machine_->vic().timing().clock_hz);
  after_song_start();
  return true;
}

/** @brief A song (re)started: clock from zero, mutes back on. Lock held. */
void Session::after_song_start(void)
{
  song_frame0_ = frame_;
  ended_ = false;
  if (seek_to_ != 0) {
    seek_to_ = 0;
    machine_->set_sid_backend(sink_->backend());
  }
  if (paused_) {
    paused_ = false;
    sink_->restore(*machine_);
  }
  apply_mutes();
  pacer_.rebase(frame_);
}

/** @brief Put the kept mutes back after an init powered the machine on. */
void Session::apply_mutes(void)
{
  for (uint8_t c = 0; c < kMaxSids; c++) {
    const uint8_t chip = static_cast<uint8_t>(c + 1);
    if (chip_mute_ & (1u << c)) machine_->sid().set_chip_mute(chip, true);
    for (uint8_t v = 0; v < 3; v++) {
      if (voice_mute_[c] & (1u << v)) {
        machine_->sid().set_voice_mute(chip, static_cast<uint8_t>(v + 1), true);
      }
    }
  }
}

/** @brief A seek reached its frame: writes go out again. Lock held. */
void Session::finish_seek(void)
{
  seek_to_ = 0;
  machine_->set_sid_backend(sink_->backend());
  sink_->restore(*machine_);
  pacer_.rebase(frame_);
}

/** @brief Length of the current song in ms, 0 for no limit. Lock held. */
uint32_t Session::song_length_ms(void) const
{
  if (!options_.use_song_lengths || player_.is_prg()) return 0;
  const uint16_t song = player_.song();
  uint32_t ms = lengths_.valid ? lengths_.for_song(song) : 0;
  if (ms == 0 && player_.tune().has_embedded_song_lengths) {
    ms = player_.tune().embedded_song_length_ms(song);
  }
  return (ms > 0) ? ms : options_.default_song_ms;
}

void Session::fail(const char * message)
{
  failed_ = true;
  snprintf(error_, sizeof(error_), "%s", message);
}

/** @brief Copy the state into status_. Lock held. */
void Session::update_status(void)
{
  SessionStatus & s = status_;
  s.loaded = loaded_;
  s.playing = running_ && player_.playing() && !ended_ && !failed_;
  s.paused = paused_;
  s.seeking = (seek_to_ != 0);
  s.ended = ended_;
  s.failed = failed_;
  s.is_prg = player_.is_prg();
  s.needs_roms = loaded_ && player_.needs_roms();
  s.stuck_without_roms = player_.stuck_without_roms();
  s.song = player_.is_prg() ? 0 : player_.song();
  s.songs = player_.is_prg() ? 0 : player_.songs();
  s.frame_rate = pacer_.frame_rate();
  s.elapsed_ms = (s.frame_rate > 0.0)
    ? static_cast<uint32_t>(1000.0 * static_cast<double>(frame_ - song_frame0_) / s.frame_rate)
    : 0;
  s.length_ms = loaded_ ? song_length_ms() : 0;
  s.frames = frame_;
  s.clock_hz = machine_->vic().timing().clock_hz;
  s.sid_count = machine_->sid().config().count;
  s.chip_mute = chip_mute_;
  memcpy(s.voice_mute, voice_mute_, sizeof(s.voice_mute));
  s.lag_us = pacer_.lag_us();
  s.resyncs = pacer_.resyncs();
  s.underruns = sink_ ? sink_->underruns() : 0;
  s.clipped = sink_ ? sink_->clipped() : 0;
  snprintf(s.sink, sizeof(s.sink), "%s", sink_ ? sink_->name() : "");
  snprintf(s.error, sizeof(s.error), "%s", error_);
}

} /* namespace usbsid */
