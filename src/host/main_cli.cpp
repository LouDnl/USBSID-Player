/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * main_cli.cpp
 * The desktop front end.
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
#include "sidfile.h"
#include "console.h"
#include "audio_out.h"
#include "sid_residfp.h"
#include "wav_write.h"
#include "songlengths.h"

using namespace usbsid;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int)
{
  /* Put the terminal back before anything else. A handler cannot run a
   * destructor, so RawConsole's cannot help here, and a player killed with
   * ctrl-c must not leave a shell with no echo. */
  console_restore();
  g_stop = 1;
}

/**
 * @brief Read a `--mute`/`--solo` list into a bit per voice per chip.
 *
 * `1:3,2` is voice three of the first chip and all of the second. A chip on its
 * own means its three voices, which is the common case and saves writing them
 * out. Chips and voices count from one, the way the sockets and the datasheet
 * do, because the alternative is a flag that means something different from
 * every other place these are named. Chip numbers go up to kMaxSids (15),
 * same ceiling as --select-sids: Mos6581_8580::set_voice_mute() already
 * accepts any chip up to kMaxSids regardless of output, so capping the parser
 * at 4 here would silence --output=audio/nsd or multi-board runs incorrectly
 * for chip 5 and up.
 *
 * @param spec  the argument as given
 * @param mask  out, kMaxSids bytes, bits 0 to 2 per chip
 * @returns true if the whole string parsed, false on the first thing that did
 *          not, with mask left as far as it got
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

/** @brief Print what was silenced, so a WAV file's provenance is on screen. */
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
 * @brief Read a `--select-sids` list into a slot indexed array of tune SID
 * numbers, 0 marking a slot nobody claimed.
 *
 * Comma separated entries, each `SID` or `SID:SLOT`, counting from 1 like
 * --mute/--solo. `3,4,5` packs into consecutive slots one, two, three - see
 * UsbSidBackend::set_sid_select(), which already treats a 0 entry as "empty,
 * skip it". `3:1,5:4` instead puts tune SID 3 in slot one and tune SID 5 in
 * slot four, leaving two and three empty. A bare entry claims the lowest
 * numbered slot not already claimed by an explicit one, regardless of where
 * in the spec that explicit entry appears - so `1,3:1` and `3:1,1` both put
 * tune SID 1 in slot two.
 *
 * `fm` in place of a SID number (`fm` or `fm:SLOT`, at most once) is the
 * tune's FM/OPL, stored as kSelectFmOpl; UsbSidBackend::set_sid_select()
 * only honours it on a slot that is its board's FM/OPL.
 *
 * @param spec   the argument as given
 * @param out    out, kMaxSids entries, slot indexed, 0 for unused
 * @param count  out, one past the highest slot any entry claimed
 * @returns true if the whole string parsed, false on the first thing that did
 *          not, with out/count left as far as it got
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

  /* Explicit slots are placed first, so a bare entry can never displace one
   * named later in the spec than itself. */
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
 * @brief Read a `--boards` list into serial numbers, in the order given.
 *
 * Comma separated. A board is named by its own serial number, not a
 * position, since which board USB happens to enumerate first is not
 * something worth relying on - see --list-boards for what a board's serial
 * actually is. Passed straight through to USBSID_Manager::OpenAll(),
 * which opens exactly these boards, in this order, and skips one that
 * turns out not to be attached.
 *
 * @param spec  the argument as given
 * @param out   out, one entry per comma separated serial
 * @returns true if the whole string parsed, false on a stray empty entry
 *          (e.g. a leading, trailing, or doubled comma)
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
    "usage: %s [options] <file.sid|file.prg|file.p00>\n"
    "\n"
    "  a SID file plays; a program is loaded where it says and started the\n"
    "  way you would start it, with RUN or with SYS\n"
    "\n"
    "  -s, --song N      start at subtune N (default: the tune's own)\n"
    "  -t, --seconds N   stop after N seconds (default: play until ctrl-c)\n"
    "  -i, --info        print what the file says and exit\n"
    "  -n, --no-device   run without hardware, useful for checking a tune\n"
    "\n"
    "  sound:\n"
#if US_HAVE_NETDEVICE
    "  -o, --output M    usbsid (default), audio, wav, or nsd. usbsid\n"
    "                    falls back to audio when no board is found\n"
#else
    "  -o, --output M    usbsid (default), audio, or wav. usbsid falls back\n"
    "                    to audio when no board is found\n"
#endif
    "  -lb, --list-boards  list every attached board's serial number and exit\n"
    "  -b, --boards SPEC  open these boards for --output=usbsid instead of\n"
    "                    the one the driver picks, in this order (board 1\n"
    "                    is the first, etc.). SPEC is a comma separated list\n"
    "                    of serial numbers from --list-boards, for example\n"
    "                    AB12,CD34 opens AB12 as board 1 and CD34 as board 2\n"
    "                    regardless of USB-connect order, and leaves any\n"
    "                    other attached board untouched. The tune's SIDs are\n"
    "                    spread across every opened board's SIDs, board 1's\n"
    "                    first. (default: one board, the first in USB\n"
    "                    bus/port order)\n"
    "  -w, --wav FILE    write a WAV instead of playing, implies --output=wav\n"
#if US_HAVE_NETDEVICE
    "  -nh, --net-host H  Network SID Device server to connect to for\n"
    "                    --output=nsd (default 127.0.0.1)\n"
    "  -np, --net-port P  its TCP port (default 6581)\n"
    "  -ns, --net-sids N  SIDs to tell it about (default: the tune's own count)\n"
#endif
    "  -ra, --rate N     sample rate for audio and wav (default 44100). A device\n"
    "                    may impose its own, which is then what is used\n"
    "  -q, --quality Q   fast (linear) or good (sinc, default)\n"
    "  -st, --stereo     pan multi-SID tunes per the v5 file's own hint\n"
    "                    (--output=audio/wav only; off by default, one channel)\n"
    "  -rv, --resid-volume N  reSIDfp (SID) output level, percent, 0-300\n"
    "                    (--output=audio/wav only; default 100)\n"
    "  -fv, --fmopl-volume N  FM/OPL output level, percent, 0-300 (same scope;\n"
    "                    default 50 - the OPL is the louder of the two chips)\n"
    "  -T, --trace FILE  write every SID register event to FILE, laid out like\n"
    "                    -srw (chip, C64 address, register:value, [C] cycle\n"
    "                    delta) plus the running cycle total and the play\n"
    "                    time MM:SS.mmm. Records what is played, so it works\n"
    "                    with a board and with --wav; add -n for a silent\n"
    "                    run that only records\n"
    "  -m, --mute SPEC   silence voices (gate/sustain forced off; everything\n"
    "                    else for that voice still reaches the backend). SPEC\n"
    "                    is a comma separated list of CHIP:VOICE or CHIP for\n"
    "                    all three, chips and voices counting from 1, for\n"
    "                    example 1:3 or 2 or 1:1,1:2\n"
    "  -S, --solo SPEC   the other way round, and harder: only SPEC's writes\n"
    "                    reach the backend at all, everything else is dropped\n"
    "                    before it gets there (and before -srw sees it), not\n"
    "                    just silenced. --solo 1:2 --wav v2.wav records voice\n"
    "                    two on its own with no other chip/voice traffic\n"
    "  -ms, --mute-solo SPEC  the old --solo: silence everything except SPEC\n"
    "                    the soft way, same as --mute. Kept under its own name\n"
    "                    for anyone relying on that behavior specifically\n"
    "  -P, --pal         force PAL timing\n"
    "  -N, --ntsc        force NTSC timing\n"
    "\n"
    "  hardware:\n"
    "  -rr               read the SID back from the chip, not the mirror\n"
    "  -f                force everything into socket two\n"
    "  -fa XX            force everything to physical base $XX (hex)\n"
    "  -ss, --select-sids SPEC  play only these of the tune's SIDs, on the\n"
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
    "  -oh, --overhead N  cycles one hardware access costs (default 1)\n"
    "  -sl, --songlengths F  HVSC Songlengths database, to stop when the song\n"
    "                    ends. Found by itself in $SONGLENGTHS,\n"
    "                    ~/Songlengths.md5, $HVSCROOT or $HVSC_BASE\n"
    "                    DOCUMENTS/Songlengths.md5, or $HVSCDB.\n"
    "  -nsl, --no-songlengths  ignore it even when one is found\n"
    "\n"
    "  logging, to stdout, same switches as old player:\n"
    "  -srw              SID reads and writes\n"
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
    argv0);
}

bool read_file(const char * path, std::vector<data_t> & out)
{
  /* fopen(path, "rb") happily opens a directory on Linux, and ftell() on that
   * stream returns whatever bogus/huge value the filesystem reports as its
   * "size" rather than failing - resize() on that then throws std::bad_alloc
   * instead of the ordinary "cannot read" error every other bad path gets. */
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
 * @brief Frames as mm:ss.t, for the status line.
 *
 * Returned in a static buffer, which is fine for one caller a frame and would
 * not be if there were two.
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
  printf("  title    : %s\n", t.name);
  printf("  author   : %s\n", t.author);
  printf("  released : %s\n", t.released);
  printf("  format   : %s v%u, %u song%s, default %u\n",
         t.is_rsid ? "RSID" : "PSID", t.version, t.songs,
         t.songs == 1 ? "" : "s", t.start_song);
  printf("  memory   : load $%04x-$%04x, init $%04x, play $%04x\n",
         t.load_addr, t.load_last_addr, t.init_addr, t.play_addr);
  printf("  video    : %s\n",
         t.video_known ? vic_timing(t.video_model).name : "unspecified");
  printf("  sids     : %u", t.sid_count);
  for (uint8_t i = 0; i < t.sid_count; i++) printf(" $%04x", t.sid_addr[i]);
  /* --output=usbsid/webusb plays a real board, which only ever has 4
   * physical sockets (UsbSidBackend, EmbeddedSidBackend, WebSidBackend all
   * stay a hard 4 chip placeholder by design - see mos6581_8580.h's own
   * kMaxSids comment); --output=audio/nsd synthesises or forwards up
   * to kMaxSids (15), so this caveat does not apply to them. Which one this
   * run ends up using is not decided yet at this point in main(), so the
   * warning names both rather than guessing. */
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
      /* Whether this is actually honoured, not just parsed, depends on
       * --stereo (ResidFpSidBackend::set_pan(), main()) and on ending up at
       * --output=audio/wav: a real board is untouched by --stereo and still
       * plays one channel per chip regardless. Which output this run lands
       * on is not decided yet at this point in main(), same reasoning as the
       * sid_count > 4 warning above, so this only reports whether --stereo
       * was asked for, not whether a board ignored it. */
      printf("  panning  : %s/%s, %s:", layout, mode,
             stereo ? "honoured on --output=audio/wav (--stereo)"
                    : "hint only, this player mixes to one channel");
      /* t.sid_pan[] is sized kMaxSids (sidfile.h), the same ceiling
       * t.sid_count is already held to by the parser, so every chip shown
       * above has a panning entry here too - no separate cap needed. */
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

  /* The chip the tune was written for. Worth showing because it decides how the
   * filter sounds, and a tune that says nothing is a tune whose author did not
   * mind. */
  const char * model = "unspecified";
  switch (t.sid_model) {
    case SidModel::Mos6581: model = "6581"; break;
    case SidModel::Mos8580: model = "8580"; break;
    case SidModel::Any:     model = "6581 or 8580"; break;
    default: break;
  }
  printf("  model    : %s\n", model);

  /* Raster or CIA, per song. This is the single most useful line when playback
   * is wrong: a CIA driven song that is being called once a frame plays at the
   * wrong speed, and the speed word is where that is declared. */
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

  /* Where the file says the driver may go. TODO 1b and TODO 1 were both about
   * this landing somewhere it should not, so it is worth being able to see it
   * without a debugger. */
  if (t.start_page == 0) {
    printf("  freepages: none declared, the player picks\n");
  } else if (t.start_page == 0xff) {
    printf("  freepages: none at all, the file says so\n");
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
 * @brief Where the sound comes out.
 *
 * `USBSID` is the default and is always tried first. With no board it becomes
 * `Audio` rather than playing silently: a machine with no hardware still wants
 * to hear the tune, and silence that needs explaining is worse than a fallback
 * that says what it did. `NetDevice` gets the same treatment: a server that
 * cannot be reached falls back to `Audio` rather than playing silently.
 */
enum class OutputMode { USBSID, Audio, Wav, NetDevice };

} /* namespace */

/* Default song length when the database has no entry: 5 minutes (matches
 * the browser player). Without a cap, an unattended playlist run stalls
 * forever on the first unknown tune. 0 (--no-songlengths) means play until
 * stopped. */
constexpr uint32_t kDefaultSongMs = 5u * 60u * 1000u;

/*
 * The external Songlengths.md5 database is checked first: it is the curated,
 * widely used source and covers tunes that have no embedded table at all. A
 * v5 tune's own embedded lengths (bit 10 of flags, see sidfile.h) are the
 * fallback, used when the database has nothing for this exact file. Either
 * way, no length known at all still means "play for five minutes" rather
 * than "play until stopped", per kDefaultSongMs above.
 */
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
  bool real_reads = false;
  bool force_socket_two = false;
  bool force_address = false;
  data_t forced_address = 0;
  int overhead = 1;
  const char * songlengths_path = nullptr;
  bool use_songlengths = true;
  VideoModel forced_model = VideoModel::Count; /* means "not forced" */

  /* Where the sound comes out. `usbsid` is the default and always tried first;
   * with no board it falls back to `audio` rather than playing silently, which
   * is what a machine with no hardware wants. See --output in the usage. */
  OutputMode output = OutputMode::USBSID;
  const char * wav_path = nullptr;
  unsigned soft_rate = 44100;
  SoftSidQuality soft_quality = SoftSidQuality::Good;
  /* Opt in: every existing --output=audio/wav consumer assumed one channel,
   * so this stays off unless asked for. See ResidFpSidBackend::configure()'s
   * own comment on why it is a configure()-time choice and not a toggle. */
  bool soft_stereo = false;
  /* Percent, 0-300; 100/50 are the defaults ResidFpSidBackend/OplChip already
   * start at, so leaving these untouched is a no-op - see soft.set_sid_gain()/
   * set_fm_gain() below. */
  int soft_sid_volume = 100;
  int soft_fm_volume = 50;

#if US_HAVE_NETDEVICE
  /* --output=nsd: a Network SID Device server to send writes to
   * instead of local hardware. Defaults match the protocol's own stated
   * defaults (network_sid_device_v4.html), the same ones sid-device uses. */
  const char * net_host = "127.0.0.1";
  int net_port = 6581;
  int net_sids = 0; /* 0 means "use the tune's own SID count" */
#endif

  const char * boards_spec = nullptr;
  bool list_boards = false;

  for (int i = 1; i < argc; i++) {
    const char * a = argv[i];
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
    else if (!strcmp(a, "-i") || !strcmp(a, "--info")) info_only = true;
    else if (!strcmp(a, "-n") || !strcmp(a, "--no-device")) no_device = true;
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
      overhead = atoi(argv[++i]);
    else if (!strcmp(a, "-o") || !strncmp(a, "--output", 8)) {
      /* Both spellings, because both get typed: --output=wav and --output wav;
       * -o only ever takes the space separated form. */
      const char * v = nullptr;
      if (!strcmp(a, "-o")) { if (i + 1 < argc) v = argv[++i]; }
      else if (a[8] == '=') v = a + 9;
      else if (a[8] == '\0' && i + 1 < argc) v = argv[++i];
      if (v == nullptr) { printf("-o/--output needs usbsid, audio or wav\n"); return 2; }
      if (!strcmp(v, "usbsid")) output = OutputMode::USBSID;
      else if (!strcmp(v, "audio")) output = OutputMode::Audio;
      else if (!strcmp(v, "wav")) output = OutputMode::Wav;
      else if (!strcmp(v, "nsd")) {
#if US_HAVE_NETDEVICE
        output = OutputMode::NetDevice;
#else
        /* No Winsock port of sid_netdevice.cpp yet; it uses BSD sockets
         * directly. See US_HAVE_NETDEVICE in CMakeLists.txt. */
        printf("--output=nsd: not available in this build (no Windows port yet)\n");
        return 2;
#endif
      }
      else { printf("unknown output '%s': use usbsid, audio, wav or nsd\n", v); return 2; }
    }
    else if ((!strcmp(a, "-w") || !strcmp(a, "--wav")) && i + 1 < argc) {
      wav_path = argv[++i];
      output = OutputMode::Wav;   /* naming a file is asking for it */
    }
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

  /* What kind of file it is comes from the file, not from its name: a PSID
   * says so in its first four bytes, and anything else that parses as a
   * program is one. */
  SidFile info;
  PrgFile program;
  const bool is_sid = sidfile_parse(bytes.data(), bytes.size(), info);
  const bool is_prg = !is_sid && prgfile_parse(bytes.data(), bytes.size(), program);

  if (!is_sid && !is_prg) {
    printf("%s is neither a SID file nor a program this player understands\n",
           path);
    return 1;
  }

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
    /* print_tune() already said why; refuse rather than call MUS data as
     * 6502 code, which is what init_addr defaulting to load_addr means for
     * a file like this (SidFile::is_mus_player's own comment). */
    return 1;
  }

  Machine machine;
  if (forced_model != VideoModel::Count) machine.set_video_model(forced_model);

  /* The trace backend records, the USBSID backend plays. Only one of them
   * can be the machine's backend, so tracing implies no hardware. */
  std::vector<TraceSidBackend::Event> trace_buffer;
  TraceSidBackend * trace = nullptr;
  UsbSidBackend usb;
#if US_HAVE_NETDEVICE
  NetworkSidBackend net;
#endif

  /* Where the writes go while fast forwarding: nowhere. See the 'f' key. */
  NullSidBackend ff_null;
  /* Whatever the real backend turns out to be, so fast forward can put it back
   * rather than guessing which of the three it was. */
  SidBackend * active_backend = nullptr;

  /* The software SID and its two ways out. Declared here so they outlive the
   * play loop; configured only if the output mode turns out to need them, since
   * building four reSIDfp chips costs a resampler table each. */
  ResidFpSidBackend soft;
  AudioOut audio;
  WavWriter wav;
  std::vector<int16_t> soft_buf;
  bool soft_active = false;
  /* Kept so the synthesis can be reconfigured once the tune's own video
   * standard is known. See the reconfigure after the load. */
  unsigned soft_out_rate = 0;
  uint8_t soft_chips = 0;
  SoftSidModel soft_model = SoftSidModel::Mos6581;
  uint32_t soft_clock = 0;

  /* Built here, chained in below once its downstream backend is known, so
   * `-T` can trace a run that is also actually audible on a device. */
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
      /* The fallback, and it says so. Playing silently was the old behaviour and
       * is indistinguishable from a broken tune. */
      printf("  device   : none found, synthesising instead (--output=audio)\n");
      output = OutputMode::Audio;
    }
  }

#if US_HAVE_NETDEVICE
  if (output == OutputMode::NetDevice) {
    if (net.connect(net_host, static_cast<uint16_t>(net_port))) {
      /* A SID file says how many chips it wants; a program says nothing, so
       * two are assumed, same reasoning and the same fallback the software
       * backend below uses for its own chip count. --net-sids overrides
       * both when the server's own idea of chip count needs to differ. */
      const uint8_t chips = static_cast<uint8_t>(
        (net_sids > 0) ? net_sids : (is_sid ? info.sid_count : 2));
      net.set_sid_count(chips); /* clamped to kMaxSids (15) internally */
      printf("  device   : Network SID Device at %s:%d, protocol v%d, %u SID%s\n",
             net_host, net_port, net.protocol_version(), net.sid_count(),
             net.sid_count() == 1 ? "" : "s");
      machine.set_sid_backend(net);
      active_backend = &net;
    } else {
      /* Same reasoning as the usbsid fallback above: playing silently is
       * indistinguishable from a broken tune. */
      printf("  device   : cannot reach %s:%d, synthesising instead (--output=audio)\n",
             net_host, net_port);
      output = OutputMode::Audio;
    }
  }
#endif

  if (output == OutputMode::Audio || output == OutputMode::Wav) {
    /* The device gets to decide the rate. Asking a device fixed at 48000 for
     * 44100 gets a resampler for free whether or not that was wanted, so the
     * synthesis is configured for what the device actually runs at. */
    unsigned rate = soft_rate;
    if (output == OutputMode::Audio) {
      if (!audio.open(soft_rate, soft_stereo ? 2 : 1)) {
        printf("  audio    : %s\n", audio.error());
        return 1;
      }
      rate = audio.rate();
    }

    const uint32_t clock_hz = machine.vic().timing().clock_hz;
    /* The tune's own chip model and chip count, both of which sidfile.cpp has
     * already worked out from the flags and the reserved word. Using its answers
     * rather than re-decoding the header here means there is one place that can
     * be wrong about it. The model is the difference between the two filters and
     * is audible; Unknown and Any both fall to 6581, which is what a player
     * without an opinion should do. */
    const SoftSidModel model = (is_sid && info.sid_model == SidModel::Mos8580)
                             ? SoftSidModel::Csg8580 : SoftSidModel::Mos6581;
    /* A SID file says how many chips it wants and that is the answer. A program
     * says nothing, so two are made: a real C64 has one, but a program that
     * drives a second chip at $d420 is exactly the case where guessing one is
     * unrecoverable, and the spare instance costs a few hundred kilobytes of
     * host memory and is silent until something writes to it. */
    const uint8_t chips = static_cast<uint8_t>(is_sid ? info.sid_count : 2);

    if (!soft.configure(chips, static_cast<double>(clock_hz), rate,
                        soft_quality, model, soft_stereo)) {
      printf("  audio    : reSIDfp would not accept %u Hz at a %u Hz clock\n",
             rate, clock_hz);
      return 1;
    }
    /* configure() clamps internally to kMaxSoftSids (15); a tune whose own
     * header count somehow exceeds even that (kMaxSids itself is the file
     * format's own ceiling, so this should not happen) still gets whatever
     * actually got built via soft.chips(), not the raw request above. */
    const uint8_t real_chips = soft.chips();
    /* Neither of these is touched by configure() - see their own comments -
     * so setting them once here, rather than after every configure() call
     * (soft.chips()/soft.set_pan() need that, these don't), is enough for
     * the whole run including the PAL/NTSC reconfigure below. */
    soft.set_sid_gain(static_cast<float>(
      (soft_sid_volume < 0) ? 0 : soft_sid_volume) / 100.0f);
    soft.set_fm_gain(static_cast<float>(
      (soft_fm_volume < 0) ? 0 : soft_fm_volume) / 100.0f);
    /* The v5 file's own panning hint (sidfile.cpp's compute_panning(), run
     * at parse time - see is_sid's own info.sid_pan[]), one call per chip
     * actually built. No effect at all unless soft_stereo/--stereo, and a
     * program (not is_sid) has no header to take one from, so it stays the
     * implicit all-Center configure() already reset to. */
    if (soft_stereo && is_sid) {
      for (uint8_t c = 0; c < real_chips; c++) {
        soft.set_pan(static_cast<uint8_t>(c + 1), info.sid_pan[c]);
      }
    }
    /* attach() is what sets access_overhead to 0, which a software SID needs and
     * a board does not. Doing it here rather than asking the caller to remember
     * is the point of it being on the backend. */
    soft.attach(machine);
    active_backend = &soft;
    soft_active = true;
    soft_out_rate = rate;
    soft_chips = real_chips;
    soft_model = model;
    soft_clock = clock_hz;
    soft_buf.resize(65536);
    overhead = 0;   /* so the header below reports what is actually in force */

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
    /* Also stops the pacer, which is deliberate and not a side effect.
     *
     * With an audio device the **ring is the clock**: it drains at exactly the
     * device rate, and pushing into a full ring is what holds the emulation
     * back. Pacing against the wall clock as well would be two clocks
     * disagreeing by a fraction of a percent, which is an underrun or a growing
     * latency depending on which way it goes. A WAV has no clock at all and
     * should render as fast as the machine can. */
    no_device = true;
  }

  /* Chain the trace in front of whatever ended up playing. `attach()` above
   * made the software SID the machine's backend, so this has to come after it;
   * what attach() did to the access overhead stays done. */
  if (trace != nullptr) {
    trace->set_next(active_backend);
    machine.set_sid_backend(*trace);
    active_backend = trace;
  }

  /* What the hardware is, and what one access to it costs. Before anything is
   * loaded, because initialising a tune writes registers and those writes have
   * to go to the same chip, and be spaced the same way, as the ones that come
   * after. Loading fills in the tune's own chip count and addresses and leaves
   * all of this alone. */
  SidConfig & sid_config = machine.sid().config();
  sid_config.real_reads = real_reads;
  sid_config.force_socket_two = force_socket_two;
  sid_config.force_address = force_address;
  sid_config.forced_address = forced_address;
  sid_config.access_overhead = static_cast<uint8_t>(overhead);
  /* Socket counts are board 1's: -f moves the tune to board 1's socket two */
  sid_config.sids_socket_one = usb.sids_socket_one();
  sid_config.sids_socket_two = usb.sids_socket_two();
  sid_config.fmopl_sid = usb.fmopl_sid();
  /* Several boards without -f/-fa/--select-sids: the backend already picked
   * the first FM/OPL in --boards order. -f/-fa move chips by slot and could
   * put a SID on top of it: they turn that pick off. --select-sids always
   * turns it off, on one board too: the FM/OPL is then parked (fmopl_sid -1)
   * and only reaches a board through an `fm` entry, see below. */
  const bool multi_board = usb.is_open() && usb.num_boards() > 1;
  const bool force_remap = force_socket_two || force_address;
  if ((multi_board && force_remap) || select_sids_spec != nullptr) {
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

  /* --select-sids picks which of the tune's SIDs land on which board socket
   * (or, with several --boards open, logical slot), in place of the default
   * first-N-to-first-N mapping. Set on the backend itself rather than in
   * SidConfig, because it is purely an output concern: the emulation still
   * sees, mutes and traces every one of the tune's SIDs exactly as it always
   * did, only what actually reaches the hardware changes. Harmless, and
   * silently unused, on any other --output. */
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
      printf("  sid select:");
      bool any = false;
      for (uint8_t s = 0; s < count && s < slots; s++) {
        if (sids[s] == 0) continue;
        if (sids[s] == kSelectFmOpl) printf(" slot %u <- FM/OPL", s + 1);
        else printf(" slot %u <- tune SID %u", s + 1, sids[s]);
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

  /* --solo, set before Player/init_tune() so even the very first power-on
   * writes are dropped for anything not in SPEC - unlike --mute/--mute-solo
   * below, set_voice_solo() pushes nothing to the backend itself (it only
   * flips state io_write() consults), so there is no "set before load"
   * hazard the way there would be for an active push like set_voice_mute's.
   * Unlike --mute-solo it also needs no tune sid_count to clamp against:
   * it isn't inverted, SPEC names exactly what to keep. Only the chip/voice
   * combos named in SPEC ever reach the backend at all; everything else is
   * dropped in Mos6581_8580::io_write() before the backend write and before
   * -srw logs it, not merely silenced - so a soloed board sees only the
   * soloed traffic and -srw reflects exactly that, from the first write. */
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
    /* The header has been read, so the video standard is settled and the board
     * can be put on the right clock before the tune's init writes go out.
     * Setting it afterwards ran the whole init at whatever rate the previous
     * tune left behind. */
    if (usb.is_open()) usb.set_clock_rate(machine.vic().timing().clock_hz);
    /* A BASIC RSID has been turned into a program by load_sid, because that is
     * what it is: there is nothing to initialise, it is started by RUN. Ask the
     * player which it decided on rather than assuming from the file's name. */
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
    /* This boots a machine and types RUN at the prompt, so it takes a moment */
    if (!player.init_prg()) {
      printf("cannot start the program\n");
      return 1;
    }
  }

  const VicTiming & timing = machine.vic().timing();

  /* The synthesis was configured before the file was read, because that is
   * where the audio device has to be opened, and at that point the machine was
   * still on whatever standard it powers on with. An NTSC tune moves it after
   * the header is parsed, and reSIDfp derives its resampler from the clock, so
   * leaving it configured for the old one renders the whole tune at the wrong
   * rate: PAL against NTSC is 985248 against 1022727, so 3.8% long and 3.8%
   * flat, and a thirty second render finished three seconds late.
   *
   * Nothing has been played yet, the tune's init writes go out on the first
   * frame, so reconfiguring here costs one resampler table and no state. */
  if (soft_active && timing.clock_hz != soft_clock) {
    if (!soft.configure(soft_chips, static_cast<double>(timing.clock_hz),
                        soft_out_rate, soft_quality, soft_model, soft_stereo)) {
      printf("  audio    : reSIDfp would not accept %u Hz at a %u Hz clock\n",
             soft_out_rate, timing.clock_hz);
      return 1;
    }
    /* configure() rebuilds every chip from scratch and resets pan_[] to
     * Center - see its own comment - so the tune's panning hint has to go
     * back on too, the same as the first configure() above. */
    if (soft_stereo && is_sid) {
      for (uint8_t c = 0; c < soft.chips(); c++) {
        soft.set_pan(static_cast<uint8_t>(c + 1), info.sid_pan[c]);
      }
    }
    soft.attach(machine);
    soft_clock = timing.clock_hz;
    /* attach() takes the machine's backend, so the trace has to go back in
     * front of it. */
    if (trace != nullptr) machine.set_sid_backend(*trace);
  }

  /* Muting, after the load, because `init_tune()` powers the machine on again
   * and a mute set before it would be forgotten.
   *
   * The mute lives in the SID layer, on the way out to whatever is playing, so
   * this works the same for a board, for the speakers and for a WAV: recording
   * three files with `--mute-solo 1:1`, `1:2` and `1:3` gives the three
   * voices separately from the same run of the same emulation. */
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
      /* Only as far as the tune has chips: inverting all of them would report
       * a one chip tune's other fourteen as silenced, which reads as a fault.
       * Capped at kMaxSids same as player.cpp's own sid.count clamp. */
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


  /* How long the songs are, if a database can be found. The key is the MD5 of
   * the whole file, so it is computed from the bytes that were read rather than
   * from anything the parser worked out. */
  SongLengths lengths;
  char db_path[1024] = { 0 };
  /* A v5 tune can carry its own song length table right in the file (see
   * print_tune()'s "lengths : embedded in the file"). When it does, that is
   * an authoritative playtime for this song, and song_length_ms() below
   * already prefers it over kDefaultSongMs the same way it prefers the
   * external database. So a database miss is not actually a fallback to the
   * 5 minute timer in that case, and saying so here would be wrong: the
   * warning is for when nothing at all knows how long the song runs. */
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
      /* A path given on the command line that is not there is a mistake, and
       * saying "none found" about it would hide which of the two happened. */
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

  /* Raw mode for as long as the loop runs, and put back by the destructor on
   * every ordinary exit and by on_signal on the others. Does nothing when stdin
   * is not a terminal, which is what keeps the player usable from a script. */
  RawConsole console;
  const bool keys = RawConsole::interactive();
  /* The status line and the two log lines cannot share a terminal: one redraws
   * itself with a carriage return and the others scroll. The logs win. */
  const bool status_line = keys && !us_log.timers && !us_log.memstate;

  if (keys) {
    printf("  keys     : space pause, n/p subtune, f fast forward, "
           "1/2/3 mute a voice, c next sid, s stop, q or ctrl-c quit\n");
  } else {
    printf("  press ctrl-c to stop\n");
  }

  /* Pause has to silence the chip. Holding the emulation still means no more
   * writes go out, and a board plays whatever was last latched, which is a note
   * held for as long as the pause. The register file is kept so resuming puts
   * back exactly what was sounding. */
  const auto silence = [&]() {
    if (no_device) return;
    /* Two halves, and both are wanted. The board's own MUTE stops it making a
     * sound at all, which is the immediate thing; zeroing the registers is what
     * makes the silence survive it, since a mute that is lifted with a gate
     * still set would restart the note the pause was meant to end. */
    /* physical_reg(): follow -f/-fa to the socket the tune plays on */
    for (data_t r = 0; r <= 0x18; r++) usb.write(machine.sid().physical_reg(r), 0x00, 4);
    usb.flush();
    usb.mute(true);
  };
  const auto restore_registers = [&]() {
    if (no_device) return;
    /* Unmute first, then put the registers back, so the board is listening by
     * the time the values that make the sound arrive. */
    usb.mute(false);
    for (data_t r = 0; r <= 0x18; r++)
      usb.write(machine.sid().physical_reg(r), machine.sid().peek(r), 4);
    usb.flush();
  };

  bool paused = false;
  bool fast = false;
  uint64_t frame = 0;          /* monotonic: the pacer and -t depend on it */
  uint64_t song_frame0 = 0;    /* where the current song started, for the clock */
  uint64_t status_at = 0;
  uint8_t mute_chip = 1;       /* which SID the digit keys address */

  while (!g_stop && (frame_limit == 0 || frame < frame_limit)) {
    if (keys) {
      const int k = console_key();
      switch (k) {
        case ' ':
          paused = !paused;
          player.pause(paused);
          if (paused) silence(); else restore_registers();
          break;
        /* The player wraps and re-initialises on its own, so there is no song
         * number to compute here. That is deliberate: the player is the only
         * thing that knows which song it is on and how many there are, and
         * asking three frontends to do the arithmetic is how one of them gets it
         * wrong. `restart_song()` is the absolute form and refuses an out of
         * range number rather than wrapping. */
        case 'n': case kKeyRight:
        case 'p': case kKeyLeft: {
          if (player.songs() <= 1) break;
          const uint16_t before = player.song();
          if (k == 'n' || k == kKeyRight) player.next_subtune();
          else                            player.previous_subtune();
          if (player.song() != before) {
            song_frame0 = frame;
            if (!no_device) pacer.rebase(frame);
            paused = false;
          }
          break;
        }
        case 'f':
          /* Fast forward is a **seek**, not audible fast playback: over USBSID
           * every write carries the gap that precedes it and the board sits
           * those gaps out, so it cannot be driven faster than the tune's own
           * timing regardless of frame rate. So: no pacing at all while it
           * runs, writes thrown away, chip silenced going in and caught up
           * from the register file coming out, and the pacer re-anchored so
           * the schedule afterwards is measured from where the seek ended
           * rather than from where it began. */
          fast = !fast;
          if (fast) {
            silence();
            machine.set_sid_backend(ff_null);
          } else if (active_backend != nullptr) {
            machine.set_sid_backend(*active_backend);
            pacer.rebase(frame);
            restore_registers();
          }
          break;
        /* Mute a voice of the selected chip. Twelve toggles is too many keys, so
         * the digits address one chip and `c` moves between them; a single SID
         * tune, which is most of them, never needs the selector. */
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
      /* Pacing is what keeps a paused player from spinning a core, and with no
       * device there is no pacer to do it, so sleep instead. */
      if (!no_device) pacer.wait_for_frame(frame);
      else            std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (status_line) {
        const uint32_t song_ms = song_length_ms(lengths, player.tune(), player.song(), use_songlengths);
        if (song_ms > 0) {
          printf("\r  ||  %s / %u:%02u.%u  song %u/%u        ",
                 play_time(frame - song_frame0, pacer.frame_rate()),
                 song_ms / 60000u, (song_ms / 1000u) % 60u, (song_ms / 100u) % 10u,
                 player.song(), player.songs());
        } else {
          printf("\r  ||  %s  song %u/%u        ",
                 play_time(frame - song_frame0, pacer.frame_rate()),
                 player.song(), player.songs());
        }
        fflush(stdout);
      }
      continue;
    }

    player.run_frame();
    ++frame;

    /* Move what was synthesised this frame out to the device or the file.
     *
     * The device is the thing that decides the tempo here, not the pacer: a
     * ring full means the emulation is ahead and should wait, which is the same
     * backpressure the board gives through its own queue. So push what fits,
     * and if the ring is full stop pushing rather than dropping audio, because
     * dropped samples are a click and a late frame is nothing.
     */
    if (soft_active) {
      /* take()'s own frames param and return are audio frames (1 sample
       * mono, 2 interleaved stereo - see its header comment), while
       * wav.write()/audio.push() both want the raw interleaved sample count,
       * so soft_buf's element capacity has to be divided down to a frame
       * request going in and the frame count taken back out multiplied by
       * channels() coming out. Unchanged arithmetic for mono, where
       * channels() is 1 and the two counts are the same thing. */
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
              /* Ring full: let the device drain a little. Sleeping a fraction of
               * the buffer keeps this from becoming a spin. */
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
              if (g_stop) break;
            }
          }
        }
        if (g_stop) break;
      }
    }

    if (status_line && (frame - status_at) >= 5) {
      status_at = frame;
      /* The song's own length beside the clock when the database knows it, so
       * "how far in" is answerable at a glance rather than by arithmetic. */
      const uint32_t song_ms = song_length_ms(lengths, player.tune(), player.song(), use_songlengths);
      char total[24] = { 0 };
      if (song_ms > 0) {
        snprintf(total, sizeof(total), " / %u:%02u.%u",
                 song_ms / 60000u, (song_ms / 1000u) % 60u, (song_ms / 100u) % 10u);
      }
      /* Which voices are sounding, as three characters: the voice number when
       * it is on and a dash when it is muted. Only shown once something has been
       * muted, so the ordinary line stays short. */
      const uint8_t mask = machine.sid().voice_mute(mute_chip);
      char voices[24] = { 0 };
      const uint8_t sids = (sid_config.count == 0) ? 1 : sid_config.count;
      if (mask != 0 || sids > 1) {
        snprintf(voices, sizeof(voices), "  sid %u %c%c%c", mute_chip,
                 (mask & 1) ? '-' : '1', (mask & 2) ? '-' : '2',
                 (mask & 4) ? '-' : '3');
      }
      printf("\r  %s  %s%s  song %u/%u%s  %llu frames    ",
             fast ? ">> seeking" : (paused ? "||" : " >"),
             play_time(frame - song_frame0, pacer.frame_rate()), total,
             player.song(), player.songs(), voices,
             static_cast<unsigned long long>(frame));
      fflush(stdout);
    }

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

    /* The song's own end, when the database knows it and no -t was given. A
     * tune with more songs moves on to the next rather than stopping, which is
     * what a database of every song's length is for. */
    if (use_songlengths && frame_limit == 0) {
      const uint32_t ms = song_length_ms(lengths, player.tune(), player.song(), use_songlengths);
      if (ms > 0) {
        const double played_ms =
          1000.0 * static_cast<double>(frame - song_frame0) / pacer.frame_rate();
        if (played_ms >= static_cast<double>(ms)) {
          if (player.song() < player.songs()) {
            player.next_subtune();
            song_frame0 = frame;
            if (!no_device) pacer.rebase(frame);
          } else {
            printf("\r  done     %s, the last song has played out        \n",
                   play_time(frame - song_frame0, pacer.frame_rate()));
            break;
          }
        }
      }
    }

    /* Not while seeking: the whole point is to get ahead of the clock. */
    if (!no_device && !fast) pacer.wait_for_frame(frame);
  }

  player.stop();
  printf("\n  stopped after %llu frames\n",
         static_cast<unsigned long long>(frame));

  /* Clipping and underrun are the two ways software audio goes wrong quietly.
   * Chips are summed with no headroom, so N chips can reach N times full scale
   * and a loud three SID tune will clip where the same tune on one chip does
   * not; underrun is silence the device invented because the emulation did not
   * keep up. Both are counted already and both sound like "the tune", so they
   * are reported rather than left for someone to wonder about. */
  if (soft_active) {
    printf("  audio    : %llu samples",
           static_cast<unsigned long long>(soft.samples()));
    if (soft.clipped() > 0) {
      printf(", %llu clipped", static_cast<unsigned long long>(soft.clipped()));
    }
    if (output == OutputMode::Audio && audio.underruns() > 0) {
      printf(", %llu underrun", static_cast<unsigned long long>(audio.underruns()));
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
#if US_HAVE_NETDEVICE
  net.disconnect();
#endif
  return 0;
}
