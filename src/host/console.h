/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * console.h
 * One keypress at a time, never waits
 *
 *   POSIX          termios, with ICANON and ECHO off and a zero timeout read
 *   Windows        _kbhit and _getch from <conio.h>, no mode to change
 *
 * The MSYS2 MINGW64 build is a native Windows binary without <termios.h>
 *
 * Raw mode must be restored on every exit path including signals, or the shell
 * keeps no echo, RawConsole is the scope guard, console_restore() is for signal handlers
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

/** @brief No key waiting */
constexpr int kNoKey = -1;

#if !defined(_WIN32)
/* File scope for signal handlers, they cannot run a destructor */
inline termios & console_saved(void) { static termios saved{}; return saved; }
inline bool & console_raw(void) { static bool raw = false; return raw; }
#endif

/** @brief Restore the terminal, safe to call twice */
inline void console_restore(void)
{
#if !defined(_WIN32)
  if (!console_raw()) return;
  tcsetattr(STDIN_FILENO, TCSANOW, &console_saved());
  console_raw() = false;
#endif
}

/**
 * @brief Raw mode while alive, does nothing when stdin is not a terminal (pipe or script)
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
      /* ICANON off ~ key without newline, ECHO off ~ no echo on the status line */
      raw.c_lflag = static_cast<tcflag_t>(raw.c_lflag & ~(ICANON | ECHO));
      /* Return immediately */
      raw.c_cc[VMIN] = 0;
      raw.c_cc[VTIME] = 0;
      if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
      console_raw() = true;
#endif
    }

    ~RawConsole(void) { console_restore(); }

    RawConsole(const RawConsole &) = delete;
    RawConsole & operator=(const RawConsole &) = delete;

    /** @brief Keyboard attached */
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
 * @brief The following key or kNoKey, never waits
 *
 * Arrow keys (escape, '[', letter) are folded into the negative values below,
 * a bare escape is 27 for quit
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
  if (c == 0 || c == 224) {           /* Two byte form for arrows */
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

  /* Escape, maybe an arrow (VMIN 0 ~ rest is buffered, else bare escape) */
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
