/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * host/session.h
 * A playback session for interactive frontends (Android, GUIs): owns the
 * machine and the player, runs them on an engine thread, takes commands
 * between frames and reports a status snapshot. Where the writes go and what
 * paces the frames is a SessionSink: phone audio, a board, the network, or
 * nothing (tests).
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

#pragma once
#ifndef _US_HOST_SESSION_H_
#define _US_HOST_SESSION_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "machine.h"
#include "pacing.h"
#include "player.h"
#include "rom.h"
#include "sid_backend.h"
#include "sidfile.h"
#include "songlengths.h"
#include "types.h"

namespace usbsid {

/** @brief Play time of a song nothing knows the length of, 0 for no limit. */
constexpr uint32_t kSessionDefaultSongMs = 5u * 60u * 1000u;

/**
 * @brief Where a session's register writes go and what paces its frames.
 *
 * Every call comes from the session's engine thread, or from the caller's
 * thread while no engine runs, never from both at once.
 */
class SessionSink
{
  public:
    virtual ~SessionSink(void) = default;

    /** @brief Short name for status and logs. */
    virtual const char * name(void) const = 0;

    /** @brief The backend the machine writes to. */
    virtual SidBackend & backend(void) = 0;

    /**
     * @brief Become the machine's backend, before a file is loaded.
     *
     * @return false when the output is unusable
     */
    virtual bool attach(Machine & machine) = 0;

    /**
     * @brief The file is parsed: clock and chip count are known.
     *
     * Runs before the tune's init, so its writes already use this setup.
     *
     * @param machine  the machine, on the tune's video standard
     * @param tune     the parsed SID file, nullptr for a program
     * @return false when the output cannot play it
     */
    virtual bool prepare(Machine & machine, const SidFile * tune) = 0;

    /**
     * @brief One frame was emulated: move its output on.
     *
     * Called outside the session lock. May block (a full audio ring), and
     * returns early once `stopping` is set.
     *
     * @return false when the output failed (device gone)
     */
    virtual bool frame_done(const std::atomic<bool> & stopping) = 0;

    /** @brief True when the session paces frames on the wall clock. */
    virtual bool wall_clock(void) const = 0;

    /** @brief Silence the output for a pause or a seek. */
    virtual void silence(Machine & machine) { (void)machine; }

    /** @brief Put the sound back after silence(), from the register file. */
    virtual void restore(Machine & machine) { (void)machine; }

    /** @brief Playback ended or the session closes. */
    virtual void detach(Machine & machine) { (void)machine; }

    /** @brief Audio underruns while playing, 0 for outputs without a ring. */
    virtual uint64_t underruns(void) const { return 0; }

    /** @brief Clipped samples, 0 for outputs without synthesis. */
    virtual uint64_t clipped(void) const { return 0; }
};

/** @brief Discards everything. Unpaced, or wall clock paced for timing tests. */
class NullSessionSink final : public SessionSink
{
  public:
    explicit NullSessionSink(bool paced = false) : paced_(paced) {}

    const char * name(void) const override { return "none"; }
    SidBackend & backend(void) override { return backend_; }
    bool attach(Machine & machine) override
    {
      machine.set_sid_backend(backend_);
      return true;
    }
    bool prepare(Machine & machine, const SidFile * tune) override
    {
      (void)machine; (void)tune;
      return true;
    }
    bool frame_done(const std::atomic<bool> & stopping) override
    {
      (void)stopping;
      return true;
    }
    bool wall_clock(void) const override { return paced_; }

    /** @brief Writes seen, for tests. */
    uint32_t writes(void) const { return backend_.writes; }

  private:
    NullSidBackend backend_;
    bool paced_;
};

/** @brief Playback choices that are not about the output. */
struct SessionOptions {
  /** Play the following subtune when a song reaches its length */
  bool auto_advance = true;
  /** Length of a song no database or file knows, 0 to play until stopped */
  uint32_t default_song_ms = kSessionDefaultSongMs;
  /** Honour song lengths at all */
  bool use_song_lengths = true;
};

/** @brief A snapshot of a session, copied out under its lock. */
struct SessionStatus {
  bool loaded = false;
  bool playing = false;        /* engine running a tune, paused or not */
  bool paused = false;
  bool seeking = false;
  bool ended = false;          /* the last song played out */
  bool failed = false;         /* the output failed, see error */
  bool is_prg = false;
  bool needs_roms = false;     /* cannot start without BASIC and KERNAL */
  bool stuck_without_roms = false;
  uint16_t song = 0;           /* 1 based */
  uint16_t songs = 0;
  uint32_t elapsed_ms = 0;     /* in the current song */
  uint32_t length_ms = 0;      /* 0: plays until stopped */
  uint64_t frames = 0;         /* since play() */
  double frame_rate = 0.0;
  uint32_t clock_hz = 0;
  uint8_t sid_count = 0;
  uint16_t chip_mute = 0;      /* bit 0 = chip 1 */
  uint8_t voice_mute[kMaxSids] = { 0 };  /* bits 0-2 = voices 1-3 */
  int64_t lag_us = 0;          /* wall clock pacing only */
  uint32_t resyncs = 0;
  uint64_t underruns = 0;
  uint64_t clipped = 0;
  char sink[16] = { 0 };
  char error[96] = { 0 };
};

/**
 * @brief A tune or program playing on an engine thread.
 *
 * Commands may come from any thread. While the engine runs they are queued
 * and executed between two frames; while it is stopped they run at once.
 */
class Session
{
  public:
    Session(void);
    ~Session(void);

    Session(const Session &) = delete;
    Session & operator=(const Session &) = delete;

    /**
     * @brief Choose the output. Stops playback first.
     *
     * @param sink  the output, owned by the session from here on
     */
    void set_sink(std::unique_ptr<SessionSink> sink);

    /** @brief Change playback options, applied from the next song on. */
    void set_options(const SessionOptions & options);

    /**
     * @brief Replace the ROM images, applied from the next play().
     *
     * @param basic    kRomSizeBasic bytes or nullptr
     * @param kernal   kRomSizeKernal bytes or nullptr (stub KERNAL)
     * @param chargen  kRomSizeChargen bytes or nullptr
     */
    void set_roms(const data_t * basic, const data_t * kernal, const data_t * chargen);

    /** @brief Back to the build's own ROM set. */
    void default_roms(void);

    /**
     * @brief Song length database text (Songlengths.md5), copied.
     *
     * @param text  the whole file, empty to drop it
     * @param len   its length
     */
    void set_song_lengths(const char * text, size_t len);

    /**
     * @brief Copy and parse a SID file, PRG or P00. Stops playback first.
     *
     * @param bytes  the file
     * @param len    its length
     * @param song   1 based subtune, 0 for the file's default
     * @return false when the file is not a tune or program
     */
    bool load(const data_t * bytes, size_t len, uint16_t song = 0);

    /**
     * @brief Start the loaded file and the engine thread.
     *
     * @return false without a loaded file, without a sink, or when the
     *         output or the start failed (see status().error)
     */
    bool play(void);

    /** @brief Stop the engine and silence the output. */
    void stop(void);

    /** @brief Hold or resume playback, the output silenced while held. */
    void pause(bool paused);

    /** @brief Following or previous subtune from its start, wrapping. */
    void next_subtune(void);
    void previous_subtune(void);

    /**
     * @brief One subtune from its start.
     *
     * @param song  1 to songs, out of range does nothing
     */
    void select_song(uint16_t song);

    /**
     * @brief Jump forward in the current song, emulating unpaced and silent.
     *
     * @param ms  position in the song to jump to, behind the current one
     *            does nothing
     */
    void seek(uint32_t ms);

    /**
     * @brief Mute one voice or a whole chip, kept across subtunes.
     *
     * @param chip   1 to kMaxSids
     * @param voice  1 to 3, 0 for the whole chip
     * @param muted  true to silence
     */
    void set_mute(uint8_t chip, uint8_t voice, bool muted);

    /**
     * @brief Run a function on the engine thread at the start of its life.
     *
     * For thread priority and similar; set before play().
     */
    void set_engine_thread_setup(std::function<void(void)> setup);

    /** @brief A copy of the current state. */
    SessionStatus status(void) const;

    /** @brief The loaded SID file, valid until the next load(). */
    const SidFile & tune(void) const { return player_.tune(); }

    /** @brief The loaded program, valid until the next load(). */
    const PrgFile & program(void) const { return player_.prg(); }

  private:
    void engine(void);
    void join(void);
    void run_or_queue(std::function<void(void)> command);
    bool begin(void);
    void after_song_start(void);
    void apply_mutes(void);
    void finish_seek(void);
    uint32_t song_length_ms(void) const;
    void fail(const char * message);
    void update_status(void);

    std::unique_ptr<Machine> machine_;
    Player player_;
    std::unique_ptr<SessionSink> sink_;
    std::unique_ptr<RomStore> rom_store_;
    bool custom_roms_ = false;
    SessionOptions options_;

    std::vector<data_t> bytes_;
    std::vector<char> lengths_db_;
    SongLengths lengths_;
    bool loaded_ = false;
    bool is_sid_ = false;
    uint16_t start_song_ = 0;

    Pacer pacer_;
    uint64_t frame_ = 0;
    uint64_t song_frame0_ = 0;
    uint64_t seek_to_ = 0;       /* frame a seek ends on, 0 for none */
    bool paused_ = false;
    bool ended_ = false;
    bool failed_ = false;
    uint16_t chip_mute_ = 0;
    uint8_t voice_mute_[kMaxSids] = { 0 };
    char error_[96] = { 0 };

    std::function<void(void)> thread_setup_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::deque<std::function<void(void)>> commands_;
    SessionStatus status_;
};

} /* namespace usbsid */

#endif /* _US_HOST_SESSION_H_ */
