/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * main_cli.cpp
 * The desktop front end.
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

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <sys/stat.h>

#include "machine.h"
#include "pacing.h"
#include "util/logging.h"
#include "player.h"
#include "prgfile.h"
#include "sid_trace.h"
#include "sid_usbsid.h"
#if US_HAVE_NETDEVICE
#include "sid_netdevice.h"
#endif
#if US_HAVE_RTMIDI
#include "sid_asid.h"
#endif
#include "sidfile.h"
#include "console.h"
#include "audio_out.h"
#include "sid_residfp.h"
#include "wav_write.h"
#include "songlengths.h"

/* Set by CMakeLists.txt from .version */
#ifndef US_PLAYER_VERSION
#define US_PLAYER_VERSION "unknown"
#endif
#define US_PLAYER_BANNER "USBSID-Player v" US_PLAYER_VERSION "\n"

using namespace usbsid;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int)
{
  /* Restore the terminal first, destructors do not run in a handler */
  console_restore();
  g_stop = 1;
}

/**
 * @brief Read a --mute/--solo list into a bit per voice per chip, return true if all parsed
 *
 * 1:3,2 is voice three of the first chip and all of the second.
 * A chip on its own means its three voices.
 * Chips and voices count from one, chips up to kMaxSids (15).
 *
 * @param spec  the argument as given
 * @param mask  out, kMaxSids bytes, bits 0 to 2 per chip
 */
bool parse_voice_spec(const char * spec, uint8_t mask[kMaxSids])
{
  for (uint8_t i = 0; i < kMaxSids; i++) mask[i] = 0;
  if (spec == nullptr || *spec == '\0') return false;
  const char * p = spec;
  while (*p != '\0') {
    if (*p < '0' || *p > '9') return false;
    long chip = 0;
    while (*p >= '0' && *p <= '9') {
      chip = chip * 10 + (*p++ - '0');
      if (chip > kMaxSids) return false;
    }
    if (chip < 1) return false;
    int voices = 0x7;
    if (*p == ':') {
      p++;
      if (*p < '1' || *p > '3') return false;
      voices = 1 << (*p++ - '1');
    }
    mask[chip - 1] = static_cast<uint8_t>(mask[chip - 1] | voices);
    if (*p == ',') { p++; continue; }
    if (*p != '\0') return false;
  }
  return true;
}

/** @brief Print what was silenced */
void print_voice_mask(const char * label, const uint8_t mask[kMaxSids])
{
  printf("  %-9s: ", label);
  bool first = true;
  for (int c = 0; c < kMaxSids; c++) {
    for (int v = 0; v < 3; v++) {
      if ((mask[c] & (1 << v)) == 0) continue;
      printf("%schip %d voice %d", first ? "" : ", ", c + 1, v + 1);
      first = false;
    }
  }
  printf("%s\n", first ? "nothing" : "");
}

/**
 * @brief Read a --select-sids list into a slot indexed array of tune SID numbers, return true if all parsed
 *
 * Entries are SID or SID:SLOT, counting from 1.
 * 3,4,5 fills slots one, two, three. 3:1,5:4 puts SID 3 in slot one and SID 5 in slot four.
 * A bare entry takes the lowest slot not claimed by an explicit one.
 * fm or fm:SLOT (at most once) is the tune's FM/OPL, stored as kSelectFmOpl.
 *
 * @param spec   the argument as given
 * @param out    out, kMaxSids entries, slot indexed, 0 for unused
 * @param count  out, one past the highest slot any entry claimed
 */
bool parse_sid_select(const char * spec, uint8_t out[kMaxSids], uint8_t & count)
{
  for (uint8_t i = 0; i < kMaxSids; i++) out[i] = 0;
  count = 0;
  if (spec == nullptr || *spec == '\0') return false;

  uint8_t sid_tok[kMaxSids];
  int16_t slot_tok[kMaxSids]; /* -1 = no explicit slot, else 0 based */
  uint8_t n_tok = 0;

  bool have_fm = false;
  const char * p = spec;
  while (*p != '\0') {
    long sid = 0;
    if ((p[0] == 'f' || p[0] == 'F') && (p[1] == 'm' || p[1] == 'M')) {
      if (have_fm) return false;
      have_fm = true;
      sid = kSelectFmOpl;
      p += 2;
    } else {
      if (*p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') {
        sid = sid * 10 + (*p++ - '0');
        if (sid > kMaxSids) return false;
      }
    }
    if (sid < 1 || n_tok >= kMaxSids) return false;

    long slot = -1;
    if (*p == ':') {
      p++;
      slot = 0;
      if (*p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') {
        slot = slot * 10 + (*p++ - '0');
        if (slot > kMaxSids) return false;
      }
      if (slot < 1) return false;
      slot--; /* 0 based */
    }

    sid_tok[n_tok] = static_cast<uint8_t>(sid);
    slot_tok[n_tok] = static_cast<int16_t>(slot);
    n_tok++;

    if (*p == ',') { p++; continue; }
    if (*p != '\0') return false;
  }
  if (n_tok == 0) return false;

  /* Explicit slots first, bare entries fill the rest */
  for (uint8_t i = 0; i < n_tok; i++) {
    if (slot_tok[i] < 0) continue;
    out[slot_tok[i]] = sid_tok[i];
    if (static_cast<uint8_t>(slot_tok[i] + 1) > count) {
      count = static_cast<uint8_t>(slot_tok[i] + 1);
    }
  }
  uint8_t cursor = 0;
  for (uint8_t i = 0; i < n_tok; i++) {
    if (slot_tok[i] >= 0) continue;
    while (cursor < kMaxSids && out[cursor] != 0) cursor++;
    if (cursor >= kMaxSids) return false;
    out[cursor] = sid_tok[i];
    if (static_cast<uint8_t>(cursor + 1) > count) count = static_cast<uint8_t>(cursor + 1);
    cursor++;
  }
  return true;
}

/**
 * @brief Read a --boards list into serial numbers, return true if all parsed
 *
 * Comma separated, a board is named by its serial number (see --list-boards).
 * Passed to USBSID_Manager::OpenAll(), a board that is not attached is skipped.
 *
 * @param spec  the argument as given
 * @param out   out, one entry per serial
 */
bool parse_board_order(const char * spec, std::vector<std::string> & out)
{
  out.clear();
  if (spec == nullptr || *spec == '\0') return false;
  const char * p = spec;
  while (*p != '\0') {
    const char * start = p;
    while (*p != '\0' && *p != ',') p++;
    if (p == start) return false;
    out.emplace_back(start, static_cast<size_t>(p - start));
    if (*p == ',') p++;
  }
  return !out.empty();
}

void usage(const char * argv0)
{
  printf(
    US_PLAYER_BANNER
    "usage: %s [options] <file.sid|file.prg|file.p00>\n"
    "\n"
    "  a SID file plays; a program is loaded where it says and started the\n"
    "  way you would start it, with RUN or with SYS\n"
    "\n"
    "  -s,  --song N     start at subtune N (default: the tune's own)\n"
    "  -t,  --seconds N  stop after N seconds (default: play until ctrl-c)\n"
    "  -i,  --info       print what the file says and exit\n"
    "  -n,  --no-device  run without hardware, useful for checking a tune\n"
    "  --no-roms         run on the stub KERNAL, without BASIC and character\n"
    "                    ROM, the way a build without ROMs does\n"
    "\n"
    "  sound:\n"
    "  -o,  --output M   usbsid (default)"
#if US_HAVE_RTMIDI
                         ", asid"
#endif
#if US_HAVE_NETDEVICE
                         ", nsd"
#endif
                         ", audio or wav.\n"
    "                    %s falls back to audio when no board is found\n"
    "  -lb, --list-boards\n"
    "                    list every attached board's serial number and exit\n"
    "  -b,  --boards SPEC\n"
    "                    open these boards for --output=usbsid instead of\n"
    "                    the one the driver picks, in this order (board 1\n"
    "                    is the first, etc.). SPEC is a comma separated list\n"
    "                    of serial numbers from --list-boards, for example\n"
    "                    AB12,CD34 opens AB12 as board 1 and CD34 as board 2\n"
    "                    regardless of USB-connect order, and leaves any\n"
    "                    other attached board untouched. The tune's SIDs are\n"
    "                    spread across every opened board's SIDs, board 1's\n"
    "                    first. (default: one board, the first in USB\n"
    "                    bus/port order)\n"
#if US_HAVE_RTMIDI
    "  -lm, --list-midi  list every available midi output device\n"
    "  -a,  --asid N     Midi supporting ASID port to use for --output=asid\n"
    "                    (default is 0)\n"
#endif
#if US_HAVE_NETDEVICE
    "  -nh, --net-host H Network SID Device server to connect to for\n"
    "                    --output=nsd (default 127.0.0.1)\n"
    "  -np, --net-port P its TCP port (default 6581)\n"
    "  -ns, --net-sids N SIDs to tell it about (default: the tune's own count)\n"
#endif
    "  -w,  --wav FILE   write a WAV instead of playing, implies --output=wav\n"
    "  -ra, --rate N     sample rate for audio and wav (default 44100). A device\n"
    "                    may impose its own, which is then what is used\n"
    "  -q,  --quality Q  fast (linear) or good (sinc, default)\n"
    "  -st, --stereo     pan multi-SID tunes per the v5 file's own hint\n"
    "                    (--output=audio/wav only; off by default, one channel)\n"
    "  -rv, --resid-volume N\n"
    "                    reSIDfp (SID) output level, percent, 0-300\n"
    "                    (--output=audio/wav only; default 100)\n"
    "  -fv, --fmopl-volume N\n"
    "                    FM/OPL output level, percent, 0-300 (same scope;\n"
    "                    default 50 - the OPL is the louder of the two chips)\n"
    "  -T,  --trace FILE write every SID register event to FILE, laid out like\n"
    "                    -srw (chip, C64 address, register:value, [C] cycle\n"
    "                    delta) plus the running cycle total and the play\n"
    "                    time MM:SS.mmm. Records what is played, also works\n"
    "                    with a board and with --wav; add -n for a silent\n"
    "                    run that only records\n"
    "  -m,  --mute SPEC  silence voices (gate/sustain forced off; everything\n"
    "                    else for that voice still reaches the backend). SPEC\n"
    "                    is a comma separated list of CHIP:VOICE or CHIP for\n"
    "                    all three, chips and voices counting from 1, for\n"
    "                    example 1:3 or 2 or 1:1,1:2\n"
    "  -S,  --solo SPEC  the other way round, and harder: only SPEC's writes\n"
    "                    reach the backend, everything else is dropped before\n"
    "                    it gets there (and before -srw sees it), not just\n"
    "                    silenced. --solo 1:2 --wav v2.wav records voice two\n"
    "                    on its own with no other chip/voice traffic\n"
    "  -ms, --mute-solo SPEC\n"
    "                    the old --solo: silence everything except SPEC\n"
    "                    the soft way, same as --mute. Kept under its own name\n"
    "                    for anyone relying on that behavior specifically\n"
    "  -P,  --pal        force PAL timing\n"
    "  -N,  --ntsc       force NTSC timing\n"
    "\n"
    "  hardware:\n"
    "  -rr               read the SID back from the chip, not the mirror\n"
    "  -f                force everything into socket two\n"
    "  -fa XX            force everything to physical base $XX (hex)\n"
    "  -ss, --select-sids SPEC\n"
    "                    play only these of the tune's SIDs, on the\n"
    "                    board's sockets in the order given. SPEC is a comma\n"
    "                    separated list of tune SID numbers counting from 1,\n"
    "                    each optionally followed by :SLOT to name the exact\n"
    "                    socket it lands on, also counting from 1. For\n"
    "                    example 3,4,5 puts the tune's 3rd SID on the board's\n"
    "                    first socket, its 4th on the second, and its 5th on\n"
    "                    the third; 3:1,5:4 instead puts the 3rd SID on the\n"
    "                    first socket and the 5th on the fourth, leaving the\n"
    "                    second and third empty. A bare entry claims the\n"
    "                    lowest socket no :SLOT entry already claimed. At\n"
    "                    most 4 sockets are used on one board, or every\n"
    "                    SID of the boards --boards opened. fm or fm:SLOT\n"
    "                    puts the tune's FM/OPL on that socket, only when\n"
    "                    it is the board's configured FM/OPL; with SPEC\n"
    "                    given, an FM/OPL not named this way is not played\n"
    "                    (--output=usbsid only; default: the tune's first N\n"
    "                    SIDs on the first N sockets, in order)\n"
    "  -oh, --overhead N Cycles one hardware access costs (default 1; 0 for\n"
    "                    netdevice software servers)\n"
    "  -sl, --songlengths F\n"
    "                    HVSC Songlengths database, to stop when the song\n"
    "                    ends. Found by itself in $SONGLENGTHS,\n"
    "                    ~/Songlengths.md5, $HVSCROOT or $HVSC_BASE\n"
    "                    DOCUMENTS/Songlengths.md5, or $HVSCDB.\n"
    "  -nsl, --no-songlengths\n"
    "                    ignore it even when one is found\n"
    "\n"
    "  logging, to stdout, same switches as old player:\n"
    "  -srw              SID reads and writes\n"
    "  -sidr / -sidw     SID reads or SID writes only\n"
    "  -c1rw / -c2rw     CIA1 / CIA2 reads and writes\n"
    "  -vrw / -vrrw      VIC register writes / reads\n"
    "  -lrw              every CPU read and write\n"
    "  -llrw             reads that come out of a ROM\n"
    "  -pla              banking changes\n"
    "  -ins              every instruction\n"
    "  -tim              the timers, once a frame\n"
    "  -lmem             the SID registers, once a frame\n"
    "\n"
    "  -h, --help        this\n",
    argv0, argv0);
}

bool read_file(const char * path, std::vector<data_t> & out)
{
  /* fopen opens a directory on Linux, reject it before ftell gives a bogus size */
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

  FILE * f = fopen(path, "rb");
  if (f == nullptr) return false;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) { fclose(f); return false; }
  out.resize(static_cast<size_t>(size));
  const size_t got = fread(out.data(), 1, out.size(), f);
  fclose(f);
  return got == out.size();
}

/**
 * @brief Frames as mm:ss.t for the status line
 *
 * Returns a static buffer, one caller per frame
 */
const char * play_time(uint64_t frames, double rate)
{
  static char buf[24];
  const double secs = (rate > 0.0) ? (static_cast<double>(frames) / rate) : 0.0;
  const unsigned total = static_cast<unsigned>(secs);
  const unsigned tenths = static_cast<unsigned>((secs - total) * 10.0);
  snprintf(buf, sizeof(buf), "%u:%02u.%u", total / 60, total % 60, tenths);
  return buf;
}

void print_tune(const SidFile & t, bool stereo)
{
  char text[kUtf8FieldSize];
  printf("  title    : %s\n", win1252_to_utf8(t.name, text, sizeof(text)));
  printf("  author   : %s\n", win1252_to_utf8(t.author, text, sizeof(text)));
  printf("  released : %s\n", win1252_to_utf8(t.released, text, sizeof(text)));
  printf("  format   : %s v%u, %u song%s, default %u\n",
         t.is_rsid ? "RSID" : "PSID", t.version, t.songs,
         t.songs == 1 ? "" : "s", t.start_song);
  printf("  memory   : load $%04x-$%04x, init $%04x, play $%04x\n",
         t.load_addr, t.load_last_addr, t.init_addr, t.play_addr);
  printf("  video    : %s\n",
         t.video_known ? vic_timing(t.video_model).name : "unspecified");
  printf("  sids     : %u", t.sid_count);
  for (uint8_t i = 0; i < t.sid_count; i++) printf(" $%04x", t.sid_addr[i]);
  /* Output is not decided yet, warn for both
   * Boards (usbsid/webusb) have 4 sockets, audio/nsd go up to kMaxSids (15) */
  if (t.sid_count > 4) {
    printf(" (a real board plays the first 4; --output=audio or "
           "--output=nsd plays all %u)", t.sid_count);
  }
  printf("\n");

  if (t.version == 5) {
    if (t.sid_count > 1) {
      const char * layout = "standard";
      switch (t.pan_layout) {
        case SidPanLayout::LCR:           layout = "L/C/R";           break;
        case SidPanLayout::CenterFirst:   layout = "center first";    break;
        case SidPanLayout::FullyCentered: layout = "fully centered";  break;
        default: break;
      }
      const char * mode = "direct";
      switch (t.pan_mode) {
        case SidPanMode::Reverse: mode = "reverse"; break;
        case SidPanMode::Group:   mode = "group";   break;
        case SidPanMode::Spread:  mode = "spread";  break;
        default: break;
      }
      /* Only honoured with --stereo on audio/wav, a board ignores it */
      printf("  panning  : %s/%s, %s:", layout, mode,
             stereo ? "honoured on --output=audio/wav (--stereo)"
                    : "hint only, this player mixes to one channel");
      /* t.sid_pan[] is kMaxSids sized, same as t.sid_count */
      for (uint8_t i = 0; i < t.sid_count; i++) {
        printf(" %c", t.sid_pan[i] == SidPan::Left ? 'L' : t.sid_pan[i] == SidPan::Right ? 'R' : 'C');
      }
      printf("\n");
    }
    if (t.has_fm_opl) printf("  FM/OPL   : yes, SFX Sound Expander / FM YAM compatible\n");
    if (t.has_embedded_song_lengths) {
      printf("  lengths  : embedded in the file (%u song%s)\n",
             t.song_length_table_count, t.song_length_table_count == 1 ? "" : "s");
    }
  }

  /* Chip model the tune was written for, decides the filter sound */
  const char * model = "unspecified";
  switch (t.sid_model) {
    case SidModel::Mos6581: model = "6581"; break;
    case SidModel::Mos8580: model = "8580"; break;
    case SidModel::Any:     model = "6581 or 8580"; break;
    default: break;
  }
  printf("  model    : %s\n", model);

  /* Raster or CIA per song, a CIA song called once a frame plays at the wrong speed */
  printf("  speed    : ");
  if (t.songs == 1) {
    printf("%s\n", t.song_uses_cia(1) ? "CIA timer" : "raster");
  } else {
    unsigned cia = 0;
    for (uint16_t n = 1; n <= t.songs && n <= 32; n++) {
      if (t.song_uses_cia(n)) cia++;
    }
    if (cia == 0)              printf("raster, every song\n");
    else if (cia == t.songs)   printf("CIA timer, every song\n");
    else {
      printf("mixed, CIA for song");
      for (uint16_t n = 1; n <= t.songs && n <= 32; n++) {
        if (t.song_uses_cia(n)) printf(" %u", n);
      }
      printf("\n");
    }
  }

  /* Where the file says the driver may go */
  if (t.start_page == 0) {
    printf("  freepages: none declared, the player picks\n");
  } else if (t.start_page == 0xff) {
    printf("  freepages: none at all, the file declares it\n");
  } else {
    printf("  freepages: $%02x00-$%02x%s, %u page%s\n", t.start_page,
           static_cast<uint8_t>(t.start_page + t.max_pages - 1), "ff",
           t.max_pages, t.max_pages == 1 ? "" : "s");
  }

  printf("  data     : %zu bytes at offset $%04x\n", t.data_size, t.data_offset);
  if (t.is_basic) printf("  basic    : yes, an RSID holding a BASIC program\n");
  if (t.is_mus_player) {
    printf("  format   : Compute!'s Sidplayer MUS data - this player has no MUS\n"
           "             decoder, it cannot be played\n");
  }
}

/**
 * @brief Where the sound comes out
 *
 * USBSID is tried first, falls back to Audio with no board.
 * NetDevice falls back to Audio when the server cannot be reached.
 */
enum class OutputMode { USBSID, Audio, Wav, NetDevice, ASID };

} /* namespace */

/* Default song length when the database has no entry: 5 minutes, 0 (--no-songlengths) plays until stopped */
constexpr uint32_t kDefaultSongMs = 5u * 60u * 1000u;

/* Songlengths.md5 database first, then the v5 embedded table (bit 10 of flags)
 * Neither known: kDefaultSongMs */
static uint32_t song_length_ms(const usbsid::SongLengths & lengths, const SidFile & tune,
                               uint16_t song, bool use_songlengths)
{
  if (!use_songlengths) return 0;
  uint32_t ms = lengths.valid ? lengths.for_song(song) : 0;
  if (ms == 0 && tune.has_embedded_song_lengths) ms = tune.embedded_song_length_ms(song);
  return (ms > 0) ? ms : kDefaultSongMs;
}

int main(int argc, char ** argv)
{
  const char * path = nullptr;
  const char * trace_path = nullptr;
  const char * mute_spec = nullptr;
  const char * solo_spec = nullptr;
  const char * mute_solo_spec = nullptr;
  const char * select_sids_spec = nullptr;
  uint16_t song = 0;
  int seconds = 0;
  bool info_only = false;
  bool no_device = false;
  bool no_roms = false;
  bool real_reads = false;
  bool force_socket_two = false;
  bool force_address = false;
  data_t forced_address = 0;
  int overhead = 1;
  bool overhead_given = false;
  const char * songlengths_path = nullptr;
  bool use_songlengths = true;
  VideoModel forced_model = VideoModel::Count; /* means "not forced" */

  /* Output mode, usbsid is tried first and falls back to audio with no board */
  OutputMode output = OutputMode::USBSID;
  const char * wav_path = nullptr;
  unsigned soft_rate = 44100;
  SoftSidQuality soft_quality = SoftSidQuality::Good;
  /* Opt in, off unless asked for */
  bool soft_stereo = false;
  /* Percent 0-300, ResidFpSidBackend/OplChip start at 100/50, the cli at 100/100 */
  int soft_sid_volume = 100;
  int soft_fm_volume = 100;

#if US_HAVE_NETDEVICE
  /* --output=nsd server, defaults match network_sid_device_v4.html and sid-device */
  const char * net_host = "127.0.0.1";
  int net_port = 6581;
  int net_sids = 0; /* 0 means "use the tune's own SID count" */
#endif

  const char * boards_spec = nullptr;
  bool list_boards = false;
#if US_HAVE_RTMIDI
  bool list_midi = false;
  int asid_midi_port = 0;
#endif

  for (int i = 1; i < argc; i++) {
    const char * a = argv[i];
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
    else if (!strcmp(a, "-i") || !strcmp(a, "--info")) info_only = true;
    else if (!strcmp(a, "-n") || !strcmp(a, "--no-device")) no_device = true;
    else if (!strcmp(a, "--no-roms")) no_roms = true;
    else if (!strcmp(a, "-P") || !strcmp(a, "--pal")) forced_model = VideoModel::Pal6569;
    else if (!strcmp(a, "-N") || !strcmp(a, "--ntsc")) forced_model = VideoModel::Ntsc6567R8;
    else if ((!strcmp(a, "-s") || !strcmp(a, "--song")) && i + 1 < argc)
      song = static_cast<uint16_t>(atoi(argv[++i]));
    else if ((!strcmp(a, "-t") || !strcmp(a, "--seconds")) && i + 1 < argc)
      seconds = atoi(argv[++i]);
    else if ((!strcmp(a, "-T") || !strcmp(a, "--trace")) && i + 1 < argc)
      trace_path = argv[++i];
    else if ((!strcmp(a, "-m") || !strcmp(a, "--mute")) && i + 1 < argc) mute_spec = argv[++i];
    else if ((!strcmp(a, "-S") || !strcmp(a, "--solo")) && i + 1 < argc) solo_spec = argv[++i];
    else if ((!strcmp(a, "-ms") || !strcmp(a, "--mute-solo")) && i + 1 < argc) mute_solo_spec = argv[++i];
    else if ((!strcmp(a, "-ss") || !strcmp(a, "--select-sids")) && i + 1 < argc)
      select_sids_spec = argv[++i];
    else if (!strcmp(a, "-lb") || !strcmp(a, "--list-boards")) list_boards = true;
    else if ((!strcmp(a, "-b") || !strcmp(a, "--boards")) && i + 1 < argc)
      boards_spec = argv[++i];
    else if (!strcmp(a, "-rr")) real_reads = true;
    else if (!strcmp(a, "-f")) force_socket_two = true;
    else if (!strcmp(a, "-fa") && i + 1 < argc) {
      force_address = true;
      forced_address = static_cast<data_t>(strtol(argv[++i], nullptr, 16));
    }
    else if ((!strcmp(a, "-oh") || !strcmp(a, "--overhead")) && i + 1 < argc)
    { overhead = atoi(argv[++i]); overhead_given = true; }
    else if (!strcmp(a, "-o") || !strncmp(a, "--output", 8)) {
      /* Both --output=wav and --output wav, -o takes the space form only */
      const char * v = nullptr;
      if (!strcmp(a, "-o")) { if (i + 1 < argc) v = argv[++i]; }
      else if (a[8] == '=') v = a + 9;
      else if (a[8] == '\0' && i + 1 < argc) v = argv[++i];
      if (v == nullptr) { printf("-o/--output needs usbsid, audio or wav\n"); return 2; }
      if (!strcmp(v, "usbsid")) output = OutputMode::USBSID;
      else if (!strcmp(v, "audio")) output = OutputMode::Audio;
      else if (!strcmp(v, "wav")) output = OutputMode::Wav;
      else if (!strcmp(v, "asid")) {
#if US_HAVE_RTMIDI
        output = OutputMode::ASID;
#else
        /* Build without RtMidi, see US_HAVE_RTMIDI */
        printf("--output=asid: not available in this build\n");
        return 2;
#endif
      }
      else if (!strcmp(v, "nsd")) {
#if US_HAVE_NETDEVICE
        output = OutputMode::NetDevice;
#else
        /* Build without network backend, see US_HAVE_NETDEVICE */
        printf("--output=nsd: not available in this build\n");
        return 2;
#endif
      }
      else {
        printf("unknown output '%s': use usbsid, audio"
#if US_HAVE_RTMIDI
                ", asid"
#endif
#if US_HAVE_NETDEVICE
                ", nsd"
#endif
                " or wav\n", v);
        return 2;
      }
    }
    else if ((!strcmp(a, "-w") || !strcmp(a, "--wav")) && i + 1 < argc) {
      wav_path = argv[++i];
      output = OutputMode::Wav;   /* naming a file is asking for it */
    }
#if US_HAVE_RTMIDI
    else if ((!strcmp(a, "-lm") || !strcmp(a, "--list-midi")))  list_midi = true;
    else if ((!strcmp(a, "-a") || !strcmp(a, "--asid")) && i + 1 < argc) {
      asid_midi_port = atoi(argv[++i]);
    }
#endif
#if US_HAVE_NETDEVICE
    else if ((!strcmp(a, "-nh") || !strcmp(a, "--net-host")) && i + 1 < argc)
      net_host = argv[++i];
    else if ((!strcmp(a, "-np") || !strcmp(a, "--net-port")) && i + 1 < argc)
      net_port = atoi(argv[++i]);
    else if ((!strcmp(a, "-ns") || !strcmp(a, "--net-sids")) && i + 1 < argc)
      net_sids = atoi(argv[++i]);
#endif
    else if ((!strcmp(a, "-ra") || !strcmp(a, "--rate")) && i + 1 < argc)
      soft_rate = static_cast<unsigned>(atoi(argv[++i]));
    else if ((!strcmp(a, "-q") || !strcmp(a, "--quality")) && i + 1 < argc) {
      const char * v = argv[++i];
      if (!strcmp(v, "fast")) soft_quality = SoftSidQuality::Fast;
      else if (!strcmp(v, "good")) soft_quality = SoftSidQuality::Good;
      else { printf("unknown quality '%s': use fast or good\n", v); return 2; }
    }
    else if (!strcmp(a, "-st") || !strcmp(a, "--stereo")) soft_stereo = true;
    else if ((!strcmp(a, "-rv") || !strcmp(a, "--resid-volume")) && i + 1 < argc)
      soft_sid_volume = atoi(argv[++i]);
    else if ((!strcmp(a, "-fv") || !strcmp(a, "--fmopl-volume")) && i + 1 < argc)
      soft_fm_volume = atoi(argv[++i]);
    else if ((!strcmp(a, "-sl") || !strcmp(a, "--songlengths")) && i + 1 < argc)
      songlengths_path = argv[++i];
    else if (!strcmp(a, "-nsl") || !strcmp(a, "--no-songlengths")) use_songlengths = false;
    else if (!strcmp(a, "-srw")) us_log.sid_rw = true;
    else if (!strcmp(a, "-sidr")) us_log.sid_r = true;
    else if (!strcmp(a, "-sidw")) us_log.sid_w = true;
    else if (!strcmp(a, "-c1rw")) us_log.cia1_rw = true;
    else if (!strcmp(a, "-c2rw")) us_log.cia2_rw = true;
    else if (!strcmp(a, "-vrw")) us_log.vic_rw = true;
    else if (!strcmp(a, "-vrrw")) us_log.vic_reg_reads = true;
    else if (!strcmp(a, "-lrw")) us_log.read_writes = true;
    else if (!strcmp(a, "-llrw")) us_log.rom_rw = true;
    else if (!strcmp(a, "-pla")) us_log.pla = true;
    else if (!strcmp(a, "-ins")) us_log.instructions = true;
    else if (!strcmp(a, "-tim")) us_log.timers = true;
    else if (!strcmp(a, "-lmem")) us_log.memstate = true;
    else if (a[0] != '-') path = a;
    else { printf("unknown option %s\n", a); usage(argv[0]); return 2; }
  }

  if (list_boards) {
    auto found = USBSID_Manager::Enumerate();
    if (found.empty()) {
      printf("no USBSID-Pico boards found\n");
    } else {
      for (size_t b = 0; b < found.size(); b++) {
        printf("  board %zu: serial %s\n", b + 1,
               found[b].serial.empty() ? "<unknown>" : found[b].serial.c_str());
      }
      printf("use --boards SERIAL,SERIAL,... to choose which of these "
             "to open, and in what order\n");
    }
    return 0;
  }

#if US_HAVE_RTMIDI
  if (list_midi) {
    ASIDBackend l_asid;
    l_asid.list_ports();
    return 0;
  }
#endif

  std::vector<std::string> board_order;
  if (boards_spec != nullptr) {
    if (!parse_board_order(boards_spec, board_order)) {
      printf("  cannot read --boards %s, expected a comma separated list of "
             "board serial numbers, for example AB12,CD34 - see "
             "--list-boards\n", boards_spec);
      return 2;
    }
  }

  if (path == nullptr) { usage(argv[0]); return 2; }

  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("cannot read %s\n", path);
    return 1;
  }

  /* File type comes from the content, a PSID declares it in the first four bytes */
  SidFile info;
  PrgFile program;
  const bool is_sid = sidfile_parse(bytes.data(), bytes.size(), info);
  const bool is_prg = !is_sid && prgfile_parse(bytes.data(), bytes.size(), program);

  if (!is_sid && !is_prg) {
    printf("%s is neither a SID file nor a program this player understands\n",
           path);
    return 1;
  }

  fputs(US_PLAYER_BANNER, stdout);
  printf("%s\n", path);
  if (is_sid) {
    print_tune(info, soft_stereo);
  } else {
    printf("  program  : %s%s%s\n",
           program.is_p00 ? "P00 container" : "PRG",
           program.name[0] != '\0' ? ", " : "",
           program.name[0] != '\0' ? program.name : "");
    printf("  memory   : $%04x-$%04x, %zu bytes\n",
           program.load_addr, program.end_addr, program.data_size);
    if (program.has_sys_stub) {
      printf("  start    : RUN, which SYSes to $%04x\n", program.sys_addr);
    } else if (program.is_basic()) {
      printf("  start    : RUN\n");
    } else {
      printf("  start    : SYS %u\n", program.load_addr);
    }
  }
  if (info_only) return 0;
  if (is_sid && info.is_mus_player) {
    /* Refuse MUS data, it is not 6502 code */
    return 1;
  }

  Machine machine;
  if (forced_model != VideoModel::Count) machine.set_video_model(forced_model);
  static RomStore no_rom_store;
  if (no_roms) {
    machine.mmu().roms = no_rom_store.roms();
    printf("  roms     : none, stub KERNAL\n");
  }

  /* Only one can be the machine's backend, tracing implies no hardware */
  std::vector<TraceSidBackend::Event> trace_buffer;
  TraceSidBackend * trace = nullptr;
  UsbSidBackend usb;
#if US_HAVE_RTMIDI
  ASIDBackend asid;
#endif
#if US_HAVE_NETDEVICE
  NetworkSidBackend net;
#endif

  /* Writes go nowhere while fast forwarding, see the 'f' key */
  NullSidBackend ff_null;
  /* The real backend, put back after fast forward */
  SidBackend * active_backend = nullptr;

  /* Software SID, outlives the play loop, configured only when the output needs it */
  ResidFpSidBackend soft;
  AudioOut audio;
  WavWriter wav;
  std::vector<int16_t> soft_buf;
  bool soft_active = false;
  /* Kept to reconfigure for the tune's video standard after load */
  unsigned soft_out_rate = 0;
  uint8_t soft_chips = 0;
  SoftSidModel soft_model = SoftSidModel::Mos6581;
  uint32_t soft_clock = 0;

  /* Chained in below once the downstream backend is known */
  if (trace_path != nullptr) {
    trace_buffer.resize(4u * 1000u * 1000u);
    trace = new TraceSidBackend(trace_buffer.data(), trace_buffer.size());
    trace->set_source(&machine.sid());
  }

  if (output == OutputMode::USBSID && !no_device) {
    if (usb.open(board_order)) {
      if (usb.num_boards() == 1) {
        printf("  device   : USBSID-Pico, pcb v%d, %d SID%s "
               "(socket one %d, socket two %d)\n",
               usb.pcb_version(), usb.num_sids(), usb.num_sids() == 1 ? "" : "s",
               usb.sids_socket_one(), usb.sids_socket_two());
      } else {
        printf("  device   : %d USBSID-Pico boards, %d SID%s total\n",
               usb.num_boards(), usb.num_sids(), usb.num_sids() == 1 ? "" : "s");
      }
      const auto & boards = usb.manager().Boards();
      if (!board_order.empty()) {
        for (size_t b = 0; b < boards.size(); b++) {
          printf("               board %zu: pcb v%d, %d SID%s, serial %s\n",
                 b + 1, boards[b].pcbversion, boards[b].numsids,
                 boards[b].numsids == 1 ? "" : "s",
                 boards[b].serial.empty() ? "<unknown>" : boards[b].serial.c_str());
        }
      }
      for (const auto & wanted : board_order) {
        bool opened = false;
        for (const auto & b : boards) if (b.serial == wanted) { opened = true; break; }
        if (!opened) printf("  warning  : --boards asked for %s, not opened "
                             "(not attached, or already claimed)\n", wanted.c_str());
      }
      machine.set_sid_backend(usb);
      active_backend = &usb;
    } else {
      /* Fallback is announced, silence looks like a broken tune */
      printf("  device   : none found, synthesising instead (--output=audio)\n");
      output = OutputMode::Audio;
    }
  }

#if US_HAVE_RTMIDI
  if (output == OutputMode::ASID && !no_device) {
    int nosids = info.sid_count;
    if (nosids > 4) {
      printf("ASID output supports a maximum of 4 SID Chips, tune has %d SID Chips!\n",
        nosids);
      return 2;
    }
    if (asid.asid_init(asid_midi_port, nosids)) {

      machine.set_sid_backend(asid);
      active_backend = &asid;
    } else {
      /* Fallback is announced, silence looks like a broken tune */
      printf("  device   : none found, synthesising instead (--output=audio)\n");
      output = OutputMode::Audio;
    }
  }
#endif

#if US_HAVE_NETDEVICE
  if (output == OutputMode::NetDevice) {
    if (net.connect(net_host, static_cast<uint16_t>(net_port))) {
      /* A SID file wants a chip count, a program says nothing, assume two
       * --net-sids overrides */
      const uint8_t chips = static_cast<uint8_t>(
        (net_sids > 0) ? net_sids : (is_sid ? info.sid_count : 2));
      net.set_sid_count(chips); /* clamped to kMaxSids (15) internally */
      printf("  device   : Network SID Device at %s:%d, protocol v%d, %u SID%s\n",
             net_host, net_port, net.protocol_version(), net.sid_count(),
             net.sid_count() == 1 ? "" : "s");
      /* A software server is its own clock, no bus overhead unless --overhead
       * Idle frames are sent as time (SidConfig::render_idle) */
      if (net.is_software()) {
        if (!overhead_given) overhead = 0;
        machine.sid().config().render_idle = true;
      }
      machine.set_sid_backend(net);
      active_backend = &net;
    } else {
      /* Fallback as above */
      printf("  device   : cannot reach %s:%d, synthesising instead (--output=audio)\n",
             net_host, net_port);
      output = OutputMode::Audio;
    }
  }
#endif

  if (output == OutputMode::Audio || output == OutputMode::Wav) {
    /* The device decides the rate, synthesis is configured for what it runs at */
    unsigned rate = soft_rate;
    if (output == OutputMode::Audio) {
      if (!audio.open(soft_rate, soft_stereo ? 2 : 1)) {
        printf("  audio    : %s\n", audio.error());
        return 1;
      }
      rate = audio.rate();
    }

    const uint32_t clock_hz = machine.vic().timing().clock_hz;
    /* Model and chip count from sidfile.cpp, Unknown and Any fall to 6581 */
    const SoftSidModel model = (is_sid && info.sid_model == SidModel::Mos8580)
                             ? SoftSidModel::Csg8580 : SoftSidModel::Mos6581;
    /* A SID file gives the chip count, a program gets two (spare is silent until written to) */
    const uint8_t chips = static_cast<uint8_t>(is_sid ? info.sid_count : 2);

    if (!soft.configure(chips, static_cast<double>(clock_hz), rate,
                        soft_quality, model, soft_stereo)) {
      printf("  audio    : reSIDfp would not accept %u Hz at a %u Hz clock\n",
             rate, clock_hz);
      return 1;
    }
    /* configure() clamps to kMaxSoftSids (15), soft.chips() has the built count */
    const uint8_t real_chips = soft.chips();
    /* Not touched by configure(), set once for the whole run */
    soft.set_sid_gain(static_cast<float>(
      (soft_sid_volume < 0) ? 0 : soft_sid_volume) / 100.0f);
    soft.set_fm_gain(static_cast<float>(
      (soft_fm_volume < 0) ? 0 : soft_fm_volume) / 100.0f);
    /* Panning hint from the v5 file, one call per chip built
     * No effect without --stereo, a program has no header */
    if (soft_stereo && is_sid) {
      for (uint8_t c = 0; c < real_chips; c++) {
        soft.set_pan(static_cast<uint8_t>(c + 1), info.sid_pan[c]);
      }
    }
    /* attach() sets access_overhead to 0, a software SID needs that */
    soft.attach(machine);
    active_backend = &soft;
    soft_active = true;
    soft_out_rate = rate;
    soft_chips = real_chips;
    soft_model = model;
    soft_clock = clock_hz;
    soft_buf.resize(65536);
    overhead = 0;   /* for the header below to report what is actually in force */

    if (output == OutputMode::Wav) {
      const char * out = (wav_path != nullptr) ? wav_path : "usbsid.wav";
      if (!wav.open(out, rate, soft_stereo ? 2 : 1)) {
        printf("  audio    : cannot write %s\n", out);
        return 1;
      }
      printf("  output   : %s, %u Hz, %s, %u chip%s, %s\n", out, rate,
             soft_stereo ? "stereo" : "mono", real_chips,
             real_chips == 1 ? "" : "s",
             soft_quality == SoftSidQuality::Good ? "sinc" : "linear");
    } else {
      printf("  output   : reSIDfp to the default audio device, %u Hz, %s, "
             "%u chip%s, %s\n", rate, soft_stereo ? "stereo" : "mono",
             real_chips, real_chips == 1 ? "" : "s",
             soft_quality == SoftSidQuality::Good ? "sinc" : "linear");
    }
    /* Stops the pacer too
     * Audio ring is the clock (drains at the device rate), a WAV renders as fast as it can */
    no_device = true;
  }

  /* Chain the trace in front of the playing backend, after attach() */
  if (trace != nullptr) {
    trace->set_next(active_backend);
    machine.set_sid_backend(*trace);
    active_backend = trace;
  }

  /* Hardware and access cost, set before loading as the init writes need the same spacing */
  SidConfig & sid_config = machine.sid().config();
  /* ReSIDfp audio and wav recording outputs need real reads for OSC3 and ENV3 */
  bool real_reads_override = (output == OutputMode::Audio || output == OutputMode::Wav);
  sid_config.real_reads = (real_reads_override ? real_reads_override : real_reads);
  sid_config.force_socket_two = force_socket_two;
  sid_config.force_address = force_address;
  sid_config.forced_address = forced_address;
  sid_config.access_overhead = static_cast<uint8_t>(overhead);
  /* Socket counts are board 1's: -f moves the tune to board 1's socket two */
  sid_config.sids_socket_one = usb.sids_socket_one();
  sid_config.sids_socket_two = usb.sids_socket_two();
  sid_config.fmopl_sid = usb.fmopl_sid();
  /* Several boards without -f/-fa/--select-sids: backend picks the first FM/OPL in --boards order
   * -f/-fa and --select-sids turn that off, FM/OPL is parked (fmopl_sid -1)
   * and only reached through an fm entry */
  const bool multi_board = usb.is_open() && usb.num_boards() > 1;
  const bool force_remap = force_socket_two || force_address;
  if ((multi_board && force_remap) || select_sids_spec != nullptr) {
    sid_config.fmopl_sid = -1;
  }
  /* ASID carries FM/OPL in message 0x60, leave $df40/$df50 parked */
  if (output == OutputMode::ASID) {
    sid_config.fmopl_sid = -1;
  }
  if (multi_board && select_sids_spec == nullptr && is_sid && info.has_fm_opl) {
    if (force_remap) {
      printf("  fm/opl   : not used, -f/-fa move the SIDs "
             "(name it with --select-sids fm:SLOT)\n");
    } else if (usb.fmopl_sid() >= 1) {
      printf("  fm/opl   : board %d, slot %d\n", usb.fmopl_board(), usb.fmopl_sid());
    } else {
      printf("  fm/opl   : none of the boards has one configured\n");
    }
  }

  /* --select-sids maps tune SIDs to board sockets (or logical slots with several --boards)
   * Output only, emulation still sees every SID. Unused on other outputs */
  if (select_sids_spec != nullptr) {
    uint8_t sids[kMaxSids];
    uint8_t count = 0;
    if (!parse_sid_select(select_sids_spec, sids, count)) {
      printf("  cannot read --select-sids %s, expected a comma separated "
             "list of SID numbers counting from 1, each optionally followed "
             "by :SLOT, and fm for the FM/OPL, for example 3,4,5 or "
             "3:1,5:4,fm:2\n", select_sids_spec);
      return 2;
    }
    const bool fm_ok = usb.set_sid_select(sids, count);
    const uint8_t slots = static_cast<uint8_t>(usb.num_slots());
    if (output == OutputMode::USBSID) {
      printf("  sid select:\n");
      bool any = false;
      for (uint8_t s = 0; s < count && s < slots; s++) {
        if (sids[s] == 0) continue;
        if (sids[s] == kSelectFmOpl) printf("   slot %u <- FM/OPL\n", s + 1);
        else printf("   slot %u <- tune SID %u\n", s + 1, sids[s]);
        any = true;
      }
      if (!any) printf(" (nothing in range)");
      uint8_t beyond = 0;
      for (uint8_t s = slots; s < count; s++) if (sids[s] != 0) beyond++;
      if (beyond > 0) printf(" (%u beyond slot %u ignored, only %u socket%s)",
                              beyond, slots, slots, slots == 1 ? "" : "s");
      printf("\n");
      if (!fm_ok && usb.is_open()) {
        printf("  warning  : --select-sids fm names a slot that is not a "
               "configured FM/OPL, FM/OPL not played\n");
      }
      if (is_sid) {
        for (uint8_t s = 0; s < count && s < slots; s++) {
          if (sids[s] == kSelectFmOpl) {
            if (!info.has_fm_opl) {
              printf("  warning  : --select-sids fm, but the tune does not "
                     "flag FM/OPL use\n");
            }
            continue;
          }
          if (sids[s] != 0 && sids[s] > info.sid_count) {
            printf("  warning  : tune only has %u SID%s, --select-sids asks "
                   "for SID %u\n", info.sid_count,
                   info.sid_count == 1 ? "" : "s", sids[s]);
          }
        }
      }
    }
  }

  /* --solo, set before Player/init_tune(), the first power-on writes are dropped too
   * Only chip/voice combos in SPEC reach the backend
   * Dropped in Mos6581_8580::io_write() before the backend and before -srw logs it */
  if (solo_spec != nullptr) {
    uint8_t mask[kMaxSids] = { 0 };
    if (!parse_voice_spec(solo_spec, mask)) {
      printf("  cannot read --solo %s, expected CHIP or CHIP:VOICE, "
             "comma separated, counting from 1\n", solo_spec);
      return 2;
    }
    for (int c = 0; c < kMaxSids; c++) {
      for (int v = 0; v < 3; v++) {
        if (mask[c] & (1 << v)) {
          machine.sid().set_voice_solo(static_cast<uint8_t>(c + 1),
                                       static_cast<uint8_t>(v + 1));
        }
      }
    }
    print_voice_mask("solo", mask);
  }

  Player player(machine);
  if (is_sid) {
    if (!player.load_sid(bytes.data(), bytes.size(), song)) {
      printf("cannot load the tune\n");
      return 1;
    }
    /* Header read, set the board clock before the init writes go out */
    if (usb.is_open()) usb.set_clock_rate(machine.vic().timing().clock_hz);
#if US_HAVE_RTMIDI
    if (output == OutputMode::ASID) {
      asid.sendSIDEnvironment(machine.vic().timing().clock_hz < 1000000, 1);
      asid.sendSIDType(info.sid_model == SidModel::Mos6581);
    }
#endif
#if US_HAVE_NETDEVICE
    if (output == OutputMode::NetDevice) {
      net.configure_tune(machine.vic().timing().clock_hz > 1000000, info.sid_model);
    }
#endif
    /* A BASIC RSID is a program after load_sid, started by RUN. Ask the player */
    if (player.is_prg()) {
      printf("  start    : RUN, this is an RSID holding a BASIC program\n");
      if (!player.init_prg()) {
        printf("cannot start the program\n");
        return 1;
      }
    } else if (!player.init_tune(song)) {
      printf("cannot initialise the tune\n");
      return 1;
    }
  } else {
    if (!player.load_prg(bytes.data(), bytes.size())) {
      printf("cannot load the program\n");
      return 1;
    }
    if (usb.is_open()) usb.set_clock_rate(machine.vic().timing().clock_hz);
#if US_HAVE_NETDEVICE
    if (output == OutputMode::NetDevice) {
      net.configure_tune(machine.vic().timing().clock_hz > 1000000, SidModel::Unknown);
    }
#endif
    /* Boots a machine and types RUN, takes a moment */
    if (!player.init_prg()) {
      printf("cannot start the program\n");
      return 1;
    }
  }

  const VicTiming & timing = machine.vic().timing();

  /* Synthesis was configured before the file was read, an NTSC tune moves the clock after parsing
   * reSIDfp derives its resampler from the clock, PAL 985248 vs NTSC 1022727 is 3.8% off
   * Nothing played yet, costs one resampler table */
  if (soft_active && timing.clock_hz != soft_clock) {
    if (!soft.configure(soft_chips, static_cast<double>(timing.clock_hz),
                        soft_out_rate, soft_quality, soft_model, soft_stereo)) {
      printf("  audio    : reSIDfp would not accept %u Hz at a %u Hz clock\n",
             soft_out_rate, timing.clock_hz);
      return 1;
    }
    /* configure() resets pan_[] to Center, set the tune's panning hint again */
    if (soft_stereo && is_sid) {
      for (uint8_t c = 0; c < soft.chips(); c++) {
        soft.set_pan(static_cast<uint8_t>(c + 1), info.sid_pan[c]);
      }
    }
    soft.attach(machine);
    soft_clock = timing.clock_hz;
    /* attach() takes the backend, put the trace back in front */
    if (trace != nullptr) machine.set_sid_backend(*trace);
  }

  /* Mute after load, init_tune() powers the machine on again
   * The mute sits in the SID layer, same for board, speakers and WAV */
  if (mute_spec != nullptr || mute_solo_spec != nullptr) {
    uint8_t mask[kMaxSids] = { 0 };
    const char * spec = (mute_solo_spec != nullptr) ? mute_solo_spec : mute_spec;
    if (!parse_voice_spec(spec, mask)) {
      printf("  cannot read --%s %s, expected CHIP or CHIP:VOICE, "
             "comma separated, counting from 1\n",
             (mute_solo_spec != nullptr) ? "mute-solo" : "mute", spec);
      return 2;
    }
    if (mute_solo_spec != nullptr) {
      /* Only as many chips as the tune has, capped at kMaxSids */
      const int chips_here = is_sid
        ? ((player.tune().sid_count > kMaxSids) ? kMaxSids : player.tune().sid_count)
        : 2;
      for (int c = 0; c < kMaxSids; c++) {
        mask[c] = (c < chips_here) ? static_cast<uint8_t>(~mask[c] & 0x7) : 0;
      }
    }
    for (int c = 0; c < kMaxSids; c++) {
      for (int v = 0; v < 3; v++) {
        if (mask[c] & (1 << v)) {
          machine.sid().set_voice_mute(static_cast<uint8_t>(c + 1),
                                       static_cast<uint8_t>(v + 1), true);
        }
      }
    }
    print_voice_mask("muted", mask);
  }

  Pacer pacer;
  pacer.start(vic_cycles_per_frame(machine.video_model()), timing.clock_hz);


  if (is_sid) {
    printf("  playing  : song %u of %u, %s at %.2f Hz, driver at $%04x\n",
           player.song(), player.songs(), timing.name, pacer.frame_rate(),
           player.driver_address());
  } else {
    printf("  running  : %s at %.2f Hz\n", timing.name, pacer.frame_rate());
  }


  /* Song lengths from the database, key is the MD5 of the whole file */
  SongLengths lengths;
  char db_path[1024] = { 0 };
  /* A v5 tune can embed its length table (see print_tune())
   * song_length_ms() prefers it, same as the external database
   * Warn only when nothing knows the length */
  const uint32_t embedded_ms = (is_sid && player.tune().has_embedded_song_lengths)
    ? player.tune().embedded_song_length_ms(player.song()) : 0;
  if (is_sid && use_songlengths &&
      songlengths_find_file(songlengths_path, db_path, sizeof(db_path))) {
    std::vector<char> db;
    FILE * dbf = fopen(db_path, "rb");
    if (dbf != nullptr) {
      fseek(dbf, 0, SEEK_END);
      const long dbn = ftell(dbf);
      fseek(dbf, 0, SEEK_SET);
      if (dbn > 0) {
        db.resize(static_cast<size_t>(dbn));
        if (fread(db.data(), 1, db.size(), dbf) != db.size()) db.clear();
      }
      fclose(dbf);
    }
    char key[33];
    songlengths_key(bytes.data(), bytes.size(), key);
    if (!db.empty()) lengths = songlengths_lookup(db.data(), db.size(), key);
    if (lengths.valid) {
      const uint32_t ms = lengths.for_song(player.song());
      printf("  length   : %u:%02u.%03u for this song, %u in the database\n",
             ms / 60000u, (ms / 1000u) % 60u, ms % 1000u, lengths.count);
    } else if (embedded_ms > 0) {
      printf("  length   : %u:%02u for this song, from the file's own v5 song length table\n",
             embedded_ms / 60000u, (embedded_ms / 1000u) % 60u);
    } else {
      printf("  length   : not in %s, using %u:%02u\n", db_path,
             kDefaultSongMs / 60000u, (kDefaultSongMs / 1000u) % 60u);
    }
  } else if (is_sid && use_songlengths) {
    if (embedded_ms > 0) {
      printf("  length   : %u:%02u for this song, from the file's own v5 song length table\n",
             embedded_ms / 60000u, (embedded_ms / 1000u) % 60u);
    } else if (songlengths_path != nullptr) {
      /* A path given on the command line that is not there is a mistake */
      printf("  length   : cannot read %s\n", songlengths_path);
    } else {
      printf("  length   : no Songlengths database found. Point --songlengths at "
             "one, or set $SONGLENGTHS or $HVSCROOT\n");
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  const uint64_t frame_limit = (seconds > 0)
    ? static_cast<uint64_t>(static_cast<double>(seconds) * pacer.frame_rate())
    : 0;

  /* Raw console while the loop runs, restored by the destructor or on_signal, no-op when stdin is not a terminal */
  RawConsole console;
  const bool keys = RawConsole::interactive();
  /* Status line and log lines cannot share a terminal, logs win */
  const bool status_line = keys && !us_log.timers && !us_log.memstate;

  if (keys) {
    printf("  keys     : space pause, n/right p/left change subtune, r restart song,\n"
           "             f fast forward, 1/2/3 toggle mute of a voice, c select sid,\n"
           "             s/q/Esc/ctrl-c quit\n");
  } else {
    printf("  press ctrl-c to stop\n");
  }

  /* Pause silences the chip, a board plays the last latched note otherwise
   * The register file is kept to restore */
  const auto silence = [&]() {
#if US_HAVE_NETDEVICE
    if (output == OutputMode::NetDevice) {
      /* Server holds the last latched state, send volume zero on every chip */
      for (uint8_t c = 0; c < net.sid_count(); c++)
        net.write(static_cast<addr_t>(c * 0x20 + 0x18), 0x00, 4);
      net.send_pending();
      return;
    }
#endif
    if (no_device) return;
    /* Board MUTE for the immediate silence, zeroed registers keep it silent after unmute */
    /* physical_reg(): follow -f/-fa to the socket the tune plays on */
    for (data_t r = 0; r <= 0x18; r++) usb.write(machine.sid().physical_reg(r), 0x00, 4);
    usb.flush();
    usb.mute(true);
  };
  const auto restore_registers = [&]() {
#if US_HAVE_NETDEVICE
    if (output == OutputMode::NetDevice) {
      for (uint8_t c = 0; c < net.sid_count(); c++)
        net.write(static_cast<addr_t>(c * 0x20 + 0x18),
                  machine.sid().peek(static_cast<addr_t>(c * 0x20 + 0x18)), 4);
      net.send_pending();
      return;
    }
#endif
    if (no_device) return;
    /* Unmute first, then restore the registers */
    usb.mute(false);
    for (data_t r = 0; r <= 0x18; r++)
      usb.write(machine.sid().physical_reg(r), machine.sid().peek(r), 4);
    usb.flush();
  };

  /* Once per frame, a Network SID Device server is its own clock (net.wait_for_room()), the pacer only keeps the time display */
  const auto pace = [&](uint64_t f) {
#if US_HAVE_NETDEVICE
    if (output == OutputMode::NetDevice) { net.wait_for_room(g_stop); return; }
#endif
    pacer.wait_for_frame(f);
  };

  bool paused = false;
  bool fast = false;
  uint64_t frame = 0;          /* monotonic: the pacer and -t depend on it */
  uint64_t song_frame0 = 0;    /* where the current song started, for the clock */
#if US_HAVE_RTMIDI
  int asid_mult_sent = 1;      /* speed multiplier in the last ASID environment message */
#endif
  uint64_t status_at = 0;
  uint8_t mute_chip = 1;       /* which SID the digit keys address */

  /* Clock shown to the user
   * A Network SID Device runs ahead in bursts, show wall time since the last anchor
   * clamped to the emulated time, never going back */
  using disp_clock = std::chrono::steady_clock;
  disp_clock::time_point disp_t0 = disp_clock::now();
  disp_clock::time_point disp_status_next = disp_t0; /* following status print */
  uint64_t disp_base = 0;      /* frames shown at the anchor */
  uint64_t disp_last = 0;      /* last frames shown, frozen while paused */
  const auto disp_anchor = [&](uint64_t shown) {
    disp_base = shown;
    disp_last = shown;
    disp_t0 = disp_clock::now();
    disp_status_next = disp_t0;
  };
  const auto disp_frames = [&]() -> uint64_t {
    const uint64_t emu = frame - song_frame0;
    if (output != OutputMode::NetDevice) return emu;
    if (paused) return disp_last;
    if (fast) { disp_last = emu; return emu; }
    const double secs =
      std::chrono::duration<double>(disp_clock::now() - disp_t0).count();
    uint64_t v = disp_base + static_cast<uint64_t>(secs * pacer.frame_rate());
    if (v > emu) v = emu;
    if (v < disp_last) v = disp_last;
    disp_last = v;
    return v;
  };

  /* New song (n, p or r): clock, display anchor and pacer restart, pause is lifted
   * Not while seeking, the fast-forward exit restores the chip */
  const auto song_started = [&]() {
    song_frame0 = frame;
    disp_anchor(0);
    if (!no_device) pacer.rebase(frame);
    if (paused) {
      paused = false;
      player.pause(false);
      if (!fast) restore_registers();
    }
  };

  /* Status cadence: wall time for a Network SID Device, every 5 emulated frames otherwise */
  const auto status_due = [&]() -> bool {
    if (output == OutputMode::NetDevice) return disp_clock::now() >= disp_status_next;
    return (frame - status_at) >= 5;
  };
  const auto print_status = [&]() {
    status_at = frame;
    disp_status_next = disp_clock::now() + std::chrono::milliseconds(100);
    /* Song length beside the clock when the database knows it */
    const uint32_t song_ms = song_length_ms(lengths, player.tune(), player.song(), use_songlengths);
    char total[24] = { 0 };
    if (song_ms > 0) {
      snprintf(total, sizeof(total), " / %u:%02u.%u",
               song_ms / 60000u, (song_ms / 1000u) % 60u, (song_ms / 100u) % 10u);
    }
    /* Voice number when on, dash when muted, only shown once something is muted */
    const uint8_t mask = machine.sid().voice_mute(mute_chip);
    char voices[24] = { 0 };
    const uint8_t sids = (sid_config.count == 0) ? 1 : sid_config.count;
    if (mask != 0 || sids > 1) {
      snprintf(voices, sizeof(voices), "  sid %u %c%c%c", mute_chip,
               (mask & 1) ? '-' : '1', (mask & 2) ? '-' : '2',
               (mask & 4) ? '-' : '3');
    }
    printf("\r  %s  %s%s  song %u/%u%s  %llu frames\033[K",
           fast ? ">> seeking" : (paused ? "||" : " >"),
           play_time(disp_frames(), pacer.frame_rate()), total,
           player.song(), player.songs(), voices,
           static_cast<unsigned long long>(frame));
    fflush(stdout);
  };

  /* -t counts absolute frames, a Network SID Device queues audio, there the limit is the shown time to let the tail play out */
  const auto limit_reached = [&]() -> bool {
    if (frame_limit == 0) return false;
    if (output == OutputMode::NetDevice) return song_frame0 + disp_frames() >= frame_limit;
    return frame >= frame_limit;
  };

  bool net_lost = false; /* the Network SID Device went away: exit status 1 */
  bool audio_wall = false;    /* audio device stalled: paced on the wall clock */
  uint64_t audio_dropped = 0; /* samples dropped while the device stalled */
  while (!g_stop && !limit_reached()) {
    if (keys) {
      const int k = console_key();
      switch (k) {
        case ' ':
          disp_frames(); /* freeze or resume from what is shown at the moment */
          paused = !paused;
          if (!paused) disp_anchor(disp_last);
          player.pause(paused);
          if (paused) silence(); else restore_registers();
          break;
        /* The player wraps and re-initialises, restart_song() is the absolute form and refuses an out of range number */
        case 'n': case kKeyRight:
        case 'p': case kKeyLeft: {
          if (player.songs() <= 1) break;
          const uint16_t before = player.song();
          if (k == 'n' || k == kKeyRight) player.next_subtune();
          else                            player.previous_subtune();
          if (player.song() != before) song_started();
          break;
        }
        /* Restart the current song, also works on a single song tune */
        case 'r':
          if (player.restart_song(player.song())) song_started();
          break;
        case 'f':
          /* Fast forward is a seek, the board sits out every write gap
           * No pacing, writes thrown away, chip silenced and restored from the register file after
           * Pacer re-anchored at the end of the seek */
          fast = !fast;
          if (fast) {
            silence();
            machine.set_sid_backend(ff_null);
          } else {
            disp_anchor(frame - song_frame0);
            if (active_backend != nullptr) {
              machine.set_sid_backend(*active_backend);
              pacer.rebase(frame);
              restore_registers();
            }
          }
          break;
        /* Mute a voice of the selected chip, digits address a voice, c moves between chips */
        case '1': case '2': case '3': {
          const uint8_t voice = static_cast<uint8_t>(k - '0');
          const bool now = (machine.sid().voice_mute(mute_chip) &
                            (1u << (voice - 1))) != 0;
          machine.sid().set_voice_mute(mute_chip, voice, !now);
          break;
        }
        case 'c': {
          const uint8_t count = (sid_config.count == 0) ? 1 : sid_config.count;
          mute_chip = static_cast<uint8_t>((mute_chip % count) + 1);
          break;
        }
        case 's': g_stop = 1; break;
        case 'q': case 27: g_stop = 1; break;
        default: break;
      }
    }

    if (paused) {
      /* Sleep, there is no pacer without a device */
      if (!no_device && output != OutputMode::NetDevice) pacer.wait_for_frame(frame);
      else std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (status_line) {
        const uint64_t shown = disp_frames();
        const uint32_t song_ms = song_length_ms(lengths, player.tune(), player.song(), use_songlengths);
        if (song_ms > 0) {
          printf("\r  ||  %s / %u:%02u.%u  song %u/%u\033[K",
                 play_time(shown, pacer.frame_rate()),
                 song_ms / 60000u, (song_ms / 1000u) % 60u, (song_ms / 100u) % 10u,
                 player.song(), player.songs());
        } else {
          printf("\r  ||  %s  song %u/%u\033[K",
                 play_time(shown, pacer.frame_rate()),
                 player.song(), player.songs());
        }
        fflush(stdout);
      }
      continue;
    }

    player.run_frame();
    ++frame;

#if US_HAVE_RTMIDI
    /* Init sets the CIA timer, play rate is read from CIA1 over the first frames, environment message is only resent on a multiplier change */
    if (output == OutputMode::ASID && is_sid && frame - song_frame0 <= 8) {
      const int mult = asid_speed_multiplier(
        player.tune().song_uses_cia(player.song()),
        machine.cia1().latch_a(),
        vic_cycles_per_frame(machine.video_model()));
      if (mult != asid_mult_sent) {
        asid.sendSIDEnvironment(machine.vic().timing().clock_hz < 1000000, mult);
        asid_mult_sent = mult;
      }
    }
#endif

    /* Push synthesised audio to the device or file, a full ring means wait, never drop audio */
    if (soft_active) {
      /* take() counts audio frames (mono 1, stereo 2 samples)
       * wav.write()/audio.push() want interleaved samples, convert with channels() */
      const size_t ch = soft.channels();
      size_t frames_got;
      while ((frames_got = soft.take(soft_buf.data(), soft_buf.size() / ch)) != 0) {
        const size_t n = frames_got * ch;
        if (output == OutputMode::Wav) {
          wav.write(soft_buf.data(), n);
        } else {
          size_t at = 0;
          while (at < n) {
            const size_t put = audio.push(soft_buf.data() + at, n - at);
            at += put;
            if (put == 0) {
              /* Stalled device never drains, drop the rest of the frame and pace on the wall clock */
              if (audio.stalled()) { audio_dropped += n - at; break; }
              /* Ring full, sleep a fraction of the buffer */
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
              if (g_stop) break;
            }
          }
        }
        if (g_stop) break;
      }
    }

    if (status_line && status_due()) print_status();

    if (US_UNLIKELY(us_log.timers)) {
      printf("[TIM] frame %llu raster %u  cia1 a:%04x b:%04x icr:%02x  "
             "cia2 a:%04x b:%04x icr:%02x\n",
             static_cast<unsigned long long>(frame), machine.vic().raster(),
             machine.cia1().counter_a(), machine.cia1().counter_b(),
             machine.cia1().icr(),
             machine.cia2().counter_a(), machine.cia2().counter_b(),
             machine.cia2().icr());
    }
    if (US_UNLIKELY(us_log.memstate)) {
      printf("[MEM] ");
      for (data_t r = 0; r <= 0x18; r++) printf("%02x", machine.sid().peek(r));
      printf("\n");
    }

    /* Song end from the database when no -t was given, a tune with more songs moves on to the following one */
    if (use_songlengths && frame_limit == 0) {
      const uint32_t ms = song_length_ms(lengths, player.tune(), player.song(), use_songlengths);
      if (ms > 0) {
        const double played_ms =
          1000.0 * static_cast<double>(frame - song_frame0) / pacer.frame_rate();
        /* Last song ends on the shown time for a Network SID Device, lets the queued tail play out */
        const double shown_ms =
          1000.0 * static_cast<double>(disp_frames()) / pacer.frame_rate();
        const bool more = player.song() < player.songs();
        const bool over = (!more && output == OutputMode::NetDevice)
          ? shown_ms >= static_cast<double>(ms)
          : played_ms >= static_cast<double>(ms);
        if (over) {
          if (more) {
            player.next_subtune();
            song_frame0 = frame;
            disp_anchor(0);
            if (!no_device) pacer.rebase(frame);
          } else {
            if (status_line) print_status();
            printf("\r  done     %s, the last song has played out        \n",
                   play_time(disp_frames(), pacer.frame_rate()));
            break;
          }
        }
      }
    }

    /* Not while seeking */
    if (!no_device && !fast) pace(frame);

    /* Stalled audio device holds nothing back, pace on the wall clock while stalled */
    if (output == OutputMode::Audio && !fast) {
      const bool stalled = audio.stalled();
      if (stalled && !audio_wall) pacer.rebase(frame);
      audio_wall = stalled;
      if (stalled) pacer.wait_for_frame(frame);
    }

#if US_HAVE_NETDEVICE
    /* Dead server is noticed by the backend and writes are dropped, exit when it is gone, no reconnect */
    if (output == OutputMode::NetDevice && !net.is_connected()) {
      if (status_line) printf("\n");
      printf("  device   : connection to %s:%d lost\n", net_host, net_port);
      net_lost = true;
      break;
    }
#endif
  }

  player.stop();
  if (status_line) print_status(); /* last status shows the final shown time */
  printf("\n  stopped after %llu frames\n",
         static_cast<unsigned long long>(frame));

  /* Chips are summed without headroom, N chips can reach N times full scale and clip
   * Underrun is silence from an emulation that did not keep up */
  if (soft_active) {
    printf("  audio    : %llu samples",
           static_cast<unsigned long long>(soft.samples()));
    if (soft.clipped() > 0) {
      printf(", %llu clipped", static_cast<unsigned long long>(soft.clipped()));
    }
    if (output == OutputMode::Audio && audio.underruns() > 0) {
      printf(", %llu underrun", static_cast<unsigned long long>(audio.underruns()));
    }
    if (output == OutputMode::Audio && audio_dropped > 0) {
      printf(", %llu dropped (device stalled)",
             static_cast<unsigned long long>(audio_dropped));
    }
    printf("\n");
  }

  if (trace != nullptr) {
    FILE * out = fopen(trace_path, "w");
    if (out != nullptr) {
      trace->set_clock_hz(machine.vic().timing().clock_hz);
      trace->dump(out);
      fclose(out);
      printf("  trace    : %zu events written to %s%s\n", trace->count(),
             trace_path, trace->dropped() ? " (buffer overflowed)" : "");
    } else {
      printf("  cannot write %s\n", trace_path);
    }
    delete trace;
  }

  usb.close();
#if US_HAVE_RTMIDI
  asid.asid_close();
#endif
#if US_HAVE_NETDEVICE
  net.disconnect();
#endif
  return net_lost ? 1 : 0;
}
