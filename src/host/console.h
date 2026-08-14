/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * console.h
 * One keypress at a time, without waiting for one.
 *
 * The player's loop has a frame to spend and cannot afford to block on input,
 * so this reads a key if there is one and says so if there is not. That means
 * raw mode, and raw mode is where the two platforms part company:
 *
 *   POSIX          termios, with ICANON and ECHO off and a zero timeout read
 *   Windows        _kbhit and _getch from <conio.h>, and no mode to change
 *
 * The MSYS2 MINGW64 build in the release matrix is a **native Windows binary**
 * and has no <termios.h> at all, which is why this is conditional from the
 * start rather than added when the release job goes red.
 *
 * The part that matters to whoever runs the player: **raw mode must be put back
 * on every exit path**, including a signal, or the shell is left with no echo
 * and no line editing afterwards. RawConsole is a scope guard for that reason,
 * and `console_restore()` exists so a signal handler can call it too, since a
 * handler cannot run a destructor.
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

#pragma once
#ifndef _US_HOST_CONSOLE_H_
#define _US_HOST_CONSOLE_H_

#include <cstdio>

#if defined(_WIN32)
  #include <conio.h>
#else
  #include <termios.h>
  #include <unistd.h>
#endif

namespace usbsid {

/** @brief No key waiting. Distinct from every key, so 0 is not usable. */
constexpr int kNoKey = -1;

#if !defined(_WIN32)
/* File scope so a signal handler can reach it: a handler cannot run a
 * destructor, and leaving the terminal raw is the one failure here that
 * outlives the process. */
inline termios & console_saved(void) { static termios saved{}; return saved; }
inline bool & console_raw(void) { static bool raw = false; return raw; }
#endif

/** @brief Put the terminal back the way it was. Safe to call twice. */
inline void console_restore(void)
{
#if !defined(_WIN32)
  if (!console_raw()) return;
  tcsetattr(STDIN_FILENO, TCSANOW, &console_saved());
  console_raw() = false;
#endif
}

/**
 * @brief Raw mode for as long as this is alive.
 *
 * Does nothing when stdin is not a terminal, which is what makes the player
 * usable from a pipe or a script: there is no mode to set and no keys to read,
 * and asking for either would fail rather than be ignored.
 */
class RawConsole
{
  public:
    RawConsole(void)
    {
#if !defined(_WIN32)
      if (!isatty(STDIN_FILENO)) return;
      if (tcgetattr(STDIN_FILENO, &console_saved()) != 0) return;
      termios raw = console_saved();
      /* ICANON off so a key arrives without a newline, ECHO off so it does not
       * appear on the line the status is being drawn on. */
      raw.c_lflag = static_cast<tcflag_t>(raw.c_lflag & ~(ICANON | ECHO));
      /* Return immediately whether or not anything was typed. */
      raw.c_cc[VMIN] = 0;
      raw.c_cc[VTIME] = 0;
      if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
      console_raw() = true;
#endif
    }

    ~RawConsole(void) { console_restore(); }

    RawConsole(const RawConsole &) = delete;
    RawConsole & operator=(const RawConsole &) = delete;

    /** @brief Is a keyboard actually attached? */
    static bool interactive(void)
    {
#if defined(_WIN32)
      return true;
#else
      return isatty(STDIN_FILENO) != 0;
#endif
    }
};

/**
 * @brief The next key, or kNoKey. Never waits.
 *
 * An arrow key arrives as three bytes, escape then '[' then a letter. Those are
 * folded into the single negative values below so a caller can switch on one
 * value, and a bare escape still comes back as 27 for "quit".
 */
constexpr int kKeyUp    = -2;
constexpr int kKeyDown  = -3;
constexpr int kKeyRight = -4;
constexpr int kKeyLeft  = -5;

inline int console_key(void)
{
#if defined(_WIN32)
  if (!_kbhit()) return kNoKey;
  const int c = _getch();
  if (c == 0 || c == 224) {           /* the two byte form for the arrows */
    if (!_kbhit()) return kNoKey;
    switch (_getch()) {
      case 72: return kKeyUp;
      case 80: return kKeyDown;
      case 77: return kKeyRight;
      case 75: return kKeyLeft;
      default: return kNoKey;
    }
  }
  return c;
#else
  if (!console_raw()) return kNoKey;
  unsigned char c = 0;
  if (read(STDIN_FILENO, &c, 1) != 1) return kNoKey;
  if (c != 27) return static_cast<int>(c);

  /* Escape, so possibly an arrow. With VMIN 0 the rest of the sequence is
   * already in the buffer if it is coming; nothing more means a bare escape. */
  unsigned char b = 0;
  if (read(STDIN_FILENO, &b, 1) != 1 || b != '[') return 27;
  if (read(STDIN_FILENO, &b, 1) != 1) return 27;
  switch (b) {
    case 'A': return kKeyUp;
    case 'B': return kKeyDown;
    case 'C': return kKeyRight;
    case 'D': return kKeyLeft;
    default:  return kNoKey;
  }
#endif
}

} /* namespace usbsid */

#endif /* _US_HOST_CONSOLE_H_ */
