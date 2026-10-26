/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * session.h
 * Playback session for interactive frontends (Android, GUIs)
 * Machine and player run on an engine thread, commands are taken between
 * frames and a status snapshot is reported. A SessionSink decides where the
 * writes go and what paces the frames (phone audio, board, network, none)
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
#ifndef _US_HOST_SESSION_H_
#define _US_HOST_SESSION_H_

#include <atomic>
#include <chrono>
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

/** @brief Play time of a song with unknown length, 0 for no limit */
constexpr uint32_t kSessionDefaultSongMs = 5u * 60u * 1000u;

/**
 * @brief Where the register writes go and what paces the frames
 *
 * Calls come from the engine thread, or from the caller thread while no engine runs
 */
class SessionSink
{
  public:
    virtual ~SessionSink(void) = default;

    /** @brief Short name for status and logs */
    virtual const char * name(void) const = 0;

    /** @brief The backend the machine writes to */
    virtual SidBackend & backend(void) = 0;

    /**
     * @brief Become the machine backend before a file is loaded, false when the output is unusable
     */
    virtual bool attach(Machine & machine) = 0;

    /**
     * @brief File is parsed and clock and chip count are known, false when the output cannot play it
     *
     * Runs before the tune init
     *
     * @param machine  the machine, on the tune video standard
     * @param tune     the parsed SID file, nullptr for a program
     */
    virtual bool prepare(Machine & machine, const SidFile * tune) = 0;

    /**
     * @brief One frame is emulated, pass on its output, false when the output failed (device gone)
     *
     * Called outside the session lock, may block (full audio ring) and returns early when stopping is set
     */
    virtual bool frame_done(const std::atomic<bool> & stopping) = 0;

    /** @brief True when the session paces frames on the wall clock */
    virtual bool wall_clock(void) const = 0;

    /**
     * @brief True when the output plays behind the emulation by a queue
     *
     * Elapsed time is shown from the wall clock (clamped to the emulated one)
     * and the last song plays out before the end
     */
    virtual bool lags_audio(void) const { return false; }

    /** @brief Silence the output for a pause or seek */
    virtual void silence(Machine & machine) { (void)machine; }

    /** @brief Restore the sound after silence() from the register file */
    virtual void restore(Machine & machine) { (void)machine; }

    /** @brief Playback ended or the session closes */
    virtual void detach(Machine & machine) { (void)machine; }

    /** @brief Audio underruns while playing, 0 without a ring */
    virtual uint64_t underruns(void) const { return 0; }

    /** @brief Clipped samples, 0 without synthesis */
    virtual uint64_t clipped(void) const { return 0; }
};

/** @brief Discards everything, unpaced or wall clock paced */
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

    /** @brief Writes seen */
    uint32_t writes(void) const { return backend_.writes; }

  private:
    NullSidBackend backend_;
    bool paced_;
};

/** @brief Playback choices unrelated to the output */
struct SessionOptions {
  /** Play the following subtune when a song reaches its length */
  bool auto_advance = true;
  /** Length of a song no database or file knows, 0 to play until stopped */
  uint32_t default_song_ms = kSessionDefaultSongMs;
  /** Honour song lengths at all */
  bool use_song_lengths = true;
};

/** @brief Snapshot of a session, copied under the lock */
struct SessionStatus {
  bool loaded = false;
  bool playing = false;        /* Engine running a tune, paused or not */
  bool paused = false;
  bool seeking = false;
  bool ended = false;          /* Last song played out */
  bool failed = false;         /* Output failed, see error */
  bool is_prg = false;
  bool needs_roms = false;     /* Needs BASIC and KERNAL */
  bool stuck_without_roms = false;
  uint16_t song = 0;           /* 1 based */
  uint16_t songs = 0;
  uint32_t elapsed_ms = 0;     /* In the current song */
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
 * @brief A tune or program playing on an engine thread
 *
 * Commands come from any thread, queued between two frames while the engine
 * runs and executed at once while it is stopped
 */
class Session
{
  public:
    Session(void);
    ~Session(void);

    Session(const Session &) = delete;
    Session & operator=(const Session &) = delete;

    /**
     * @brief Choose the output, stops playback first
     *
     * @param sink  the output, owned by the session
     */
    void set_sink(std::unique_ptr<SessionSink> sink);

    /** @brief Change playback options, applied from the following song */
    void set_options(const SessionOptions & options);

    /**
     * @brief Replace the ROM images, applied at the following play()
     *
     * @param basic    kRomSizeBasic bytes or nullptr
     * @param kernal   kRomSizeKernal bytes or nullptr (stub KERNAL)
     * @param chargen  kRomSizeChargen bytes or nullptr
     */
    void set_roms(const data_t * basic, const data_t * kernal, const data_t * chargen);

    /** @brief Back to the default ROM set of the build */
    void default_roms(void);

    /**
     * @brief Copy the song length database text (Songlengths.md5)
     *
     * @param text  file contents, empty to drop it
     * @param len   length
     */
    void set_song_lengths(const char * text, size_t len);

    /**
     * @brief Copy and parse a SID file, PRG or P00, false when it is neither, stops playback first
     *
     * @param bytes  file contents
     * @param len    length
     * @param song   1 based subtune, 0 for the file default
     */
    bool load(const data_t * bytes, size_t len, uint16_t song = 0);

    /**
     * @brief Start the loaded file and the engine thread
     *
     * Returns false without a loaded file or sink, or when the output or start
     * failed (see status().error)
     */
    bool play(void);

    /** @brief Stop the engine and silence the output */
    void stop(void);

    /** @brief Hold or resume playback, output is silenced while held */
    void pause(bool paused);

    /** @brief Following or previous subtune from its start, wraps */
    void next_subtune(void);
    void previous_subtune(void);

    /**
     * @brief One subtune from its start
     *
     * @param song  1 to songs, out of range does nothing
     */
    void select_song(uint16_t song);

    /**
     * @brief Jump forward in the current song, emulating unpaced and silent
     *
     * @param ms  position to jump to, behind the current one does nothing
     */
    void seek(uint32_t ms);

    /**
     * @brief Mute one voice or a whole chip, kept across subtunes
     *
     * @param chip   1 to kMaxSids
     * @param voice  1 to 3, 0 for the whole chip
     * @param muted  true to silence
     */
    void set_mute(uint8_t chip, uint8_t voice, bool muted);

    /**
     * @brief Run a function at the start of the engine thread (priority etc), set before play()
     */
    void set_engine_thread_setup(std::function<void(void)> setup);

    /** @brief Copy of the current state */
    SessionStatus status(void) const;

    /** @brief Loaded SID file, valid until the following load() */
    const SidFile & tune(void) const { return player_.tune(); }

    /** @brief Loaded program, valid until the following load() */
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
    uint64_t shown_frames(void) const;
    void anchor_shown(uint64_t shown);

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

    /* Shown elapsed time for lagging outputs (see shown_frames()) */
    mutable std::chrono::steady_clock::time_point shown_t0_ =
      std::chrono::steady_clock::now();
    mutable uint64_t shown_base_ = 0;  /* song frames shown at the anchor */
    mutable uint64_t shown_last_ = 0;  /* last song frames shown */

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
