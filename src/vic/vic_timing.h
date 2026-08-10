/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * vic_timing.h
 * The four video standards, as data. A SID tune written for one and played
 * with the timing of another has every register write in the wrong place, so
 * this table decides both the VIC and the USBSID clock rate.
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
#ifndef _US_VIC_VIC_TIMING_H_
#define _US_VIC_VIC_TIMING_H_

#include "types.h"

namespace usbsid {

enum class VideoModel : uint8_t {
  Pal6569 = 0,      /* PAL-B, the ordinary European C64 */
  Ntsc6567R8,       /* NTSC, the common revision */
  Ntsc6567R56A,     /* NTSC, the early revision, one cycle shorter */
  PalN6572,         /* PAL-N, Drean */
  Count
};

struct VicTiming {
  uint16_t cycles_per_line;
  uint16_t lines_per_frame;
  uint32_t clock_hz;
  const char * name;
};

/* Cycles and lines are the chip's, the clock rates are the ones USBSID-Pico
 * accepts (see old player: DEFAULT, PAL, NTSC, DREAN, NTSC2). */
constexpr VicTiming kVicTiming[static_cast<uint8_t>(VideoModel::Count)] = {
  { 63, 312,  985248, "PAL 6569"         },
  { 65, 263, 1022727, "NTSC 6567R8"      },
  { 64, 262, 1022727, "NTSC 6567R56A"    },
  { 65, 312, 1023440, "PAL-N 6572"       },
};

US_ALWAYS_INLINE const VicTiming & vic_timing(VideoModel model)
{
  return kVicTiming[static_cast<uint8_t>(model)];
}

/* Cycles in a whole frame, which is what a tune's play routine is paced by */
US_ALWAYS_INLINE uint32_t vic_cycles_per_frame(VideoModel model)
{
  const VicTiming & t = vic_timing(model);
  return static_cast<uint32_t>(t.cycles_per_line) * t.lines_per_frame;
}

} /* namespace usbsid */

#endif /* _US_VIC_VIC_TIMING_H_ */
