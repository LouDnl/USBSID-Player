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

#include "machine.h"
#include "pacing.h"
#include "util/logging.h"
#include "player.h"
#include "prgfile.h"
#include "sid_trace.h"
#include "sid_usbsid.h"
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
    "      --output M    usbsid (default), audio, or wav. usbsid falls back to\n"
    "                    audio when no board is found\n"
    "      --wav FILE    write a WAV instead of playing, implies --output=wav\n"
    "      --rate N      sample rate for audio and wav (default 44100). A device\n"
    "                    may impose its own, which is then what is used\n"
    "      --quality Q   fast (linear) or good (sinc, default)\n"
    "  -T, --trace FILE  write every SID register event to FILE\n"
    "      --pal         force PAL timing\n"
    "      --ntsc        force NTSC timing\n"
    "\n"
    "  hardware:\n"
    "  -rr               read the SID back from the chip, not the mirror\n"
    "  -f                force everything into socket two\n"
    "  -fa XX            force everything to physical base $XX (hex)\n"
    "      --overhead N  cycles one hardware access costs (default 1)\n"
    "      --songlengths F  HVSC Songlengths database, to stop when the song ends.\n"
    "                    Found by itself in $SONGLENGTHS, ~/Songlengths.md5,\n"
    "                    $HVSCROOT or $HVSC_BASE DOCUMENTS/Songlengths.md5, or $HVSCDB.\n"
    "      --no-songlengths  ignore it even when one is found\n"
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

void print_tune(const SidFile & t)
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
  printf("\n");

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
}

/**
 * @brief Where the sound comes out.
 *
 * `UsbSid` is the default and is always tried first. With no board it becomes
 * `Audio` rather than playing silently: a machine with no hardware still wants
 * to hear the tune, and silence that needs explaining is worse than a fallback
 * that says what it did.
 */
enum class OutputMode { UsbSid, Audio, Wav };

} /* namespace */

/* What to play when the database has never heard of a song.
 *
 * Five minutes. A tune with no entry used to play until interrupted, which
 * means an unattended run stops at the first such tune for ever and a playlist
 * never reaches the end. Five minutes is longer than most SIDs and short enough
 * that sitting through one is not a punishment; the same figure is used by the
 * browser player, so the two behave alike.
 *
 * Zero when song lengths are switched off altogether, which still means "play
 * until stopped": --no-songlengths is a request for exactly that.
 */
constexpr uint32_t kDefaultSongMs = 5u * 60u * 1000u;

static uint32_t song_length_ms(const usbsid::SongLengths & lengths,
                               uint16_t song, bool use_songlengths)
{
  if (!use_songlengths) return 0;
  const uint32_t ms = lengths.valid ? lengths.for_song(song) : 0;
  return (ms > 0) ? ms : kDefaultSongMs;
}

int main(int argc, char ** argv)
{
  const char * path = nullptr;
  const char * trace_path = nullptr;
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
  OutputMode output = OutputMode::UsbSid;
  const char * wav_path = nullptr;
  unsigned soft_rate = 44100;
  SoftSidQuality soft_quality = SoftSidQuality::Good;

  for (int i = 1; i < argc; i++) {
    const char * a = argv[i];
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
    else if (!strcmp(a, "-i") || !strcmp(a, "--info")) info_only = true;
    else if (!strcmp(a, "-n") || !strcmp(a, "--no-device")) no_device = true;
    else if (!strcmp(a, "--pal")) forced_model = VideoModel::Pal6569;
    else if (!strcmp(a, "--ntsc")) forced_model = VideoModel::Ntsc6567R8;
    else if ((!strcmp(a, "-s") || !strcmp(a, "--song")) && i + 1 < argc)
      song = static_cast<uint16_t>(atoi(argv[++i]));
    else if ((!strcmp(a, "-t") || !strcmp(a, "--seconds")) && i + 1 < argc)
      seconds = atoi(argv[++i]);
    else if ((!strcmp(a, "-T") || !strcmp(a, "--trace")) && i + 1 < argc)
      trace_path = argv[++i];
    else if (!strcmp(a, "-rr")) real_reads = true;
    else if (!strcmp(a, "-f")) force_socket_two = true;
    else if (!strcmp(a, "-fa") && i + 1 < argc) {
      force_address = true;
      forced_address = static_cast<data_t>(strtol(argv[++i], nullptr, 16));
    }
    else if (!strcmp(a, "--overhead") && i + 1 < argc) overhead = atoi(argv[++i]);
    else if (!strncmp(a, "--output", 8)) {
      /* Both spellings, because both get typed: --output=wav and --output wav */
      const char * v = nullptr;
      if (a[8] == '=') v = a + 9;
      else if (a[8] == '\0' && i + 1 < argc) v = argv[++i];
      if (v == nullptr) { printf("--output needs usbsid, audio or wav\n"); return 2; }
      if (!strcmp(v, "usbsid")) output = OutputMode::UsbSid;
      else if (!strcmp(v, "audio")) output = OutputMode::Audio;
      else if (!strcmp(v, "wav")) output = OutputMode::Wav;
      else { printf("unknown output '%s': use usbsid, audio or wav\n", v); return 2; }
    }
    else if (!strcmp(a, "--wav") && i + 1 < argc) {
      wav_path = argv[++i];
      output = OutputMode::Wav;   /* naming a file is asking for it */
    }
    else if (!strcmp(a, "--rate") && i + 1 < argc)
      soft_rate = static_cast<unsigned>(atoi(argv[++i]));
    else if (!strcmp(a, "--quality") && i + 1 < argc) {
      const char * v = argv[++i];
      if (!strcmp(v, "fast")) soft_quality = SoftSidQuality::Fast;
      else if (!strcmp(v, "good")) soft_quality = SoftSidQuality::Good;
      else { printf("unknown quality '%s': use fast or good\n", v); return 2; }
    }
    else if (!strcmp(a, "--songlengths") && i + 1 < argc) songlengths_path = argv[++i];
    else if (!strcmp(a, "--no-songlengths")) use_songlengths = false;
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
    print_tune(info);
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

  Machine machine;
  if (forced_model != VideoModel::Count) machine.set_video_model(forced_model);

  /* The trace backend records, the USBSID backend plays. Only one of them
   * can be the machine's backend, so tracing implies no hardware. */
  std::vector<TraceSidBackend::Event> trace_buffer;
  TraceSidBackend * trace = nullptr;
  UsbSidBackend usb;

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

  if (trace_path != nullptr) {
    trace_buffer.resize(4u * 1000u * 1000u);
    trace = new TraceSidBackend(trace_buffer.data(), trace_buffer.size());
    machine.set_sid_backend(*trace);
    active_backend = trace;
    no_device = true;
  } else if (output == OutputMode::UsbSid && !no_device) {
    if (usb.open()) {
      printf("  device   : USBSID-Pico, pcb v%d, %d SID%s "
             "(socket one %d, socket two %d)\n",
             usb.pcb_version(), usb.num_sids(), usb.num_sids() == 1 ? "" : "s",
             usb.sids_socket_one(), usb.sids_socket_two());
      machine.set_sid_backend(usb);
      active_backend = &usb;
    } else {
      /* The fallback, and it says so. Playing silently was the old behaviour and
       * is indistinguishable from a broken tune. */
      printf("  device   : none found, synthesising instead (--output=audio)\n");
      output = OutputMode::Audio;
    }
  }

  if (output == OutputMode::Audio || output == OutputMode::Wav) {
    /* The device gets to decide the rate. Asking a device fixed at 48000 for
     * 44100 gets a resampler for free whether or not that was wanted, so the
     * synthesis is configured for what the device actually runs at. */
    unsigned rate = soft_rate;
    if (output == OutputMode::Audio) {
      if (!audio.open(soft_rate)) {
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
                        soft_quality, model)) {
      printf("  audio    : reSIDfp would not accept %u Hz at a %u Hz clock\n",
             rate, clock_hz);
      return 1;
    }
    /* attach() is what sets access_overhead to 0, which a software SID needs and
     * a board does not. Doing it here rather than asking the caller to remember
     * is the point of it being on the backend. */
    soft.attach(machine);
    active_backend = &soft;
    soft_active = true;
    soft_buf.resize(65536);
    overhead = 0;   /* so the header below reports what is actually in force */

    if (output == OutputMode::Wav) {
      const char * out = (wav_path != nullptr) ? wav_path : "usbsid.wav";
      if (!wav.open(out, rate, 1)) {
        printf("  audio    : cannot write %s\n", out);
        return 1;
      }
      printf("  output   : %s, %u Hz, %u chip%s, %s\n", out, rate, chips,
             chips == 1 ? "" : "s",
             soft_quality == SoftSidQuality::Good ? "sinc" : "linear");
    } else {
      printf("  output   : reSIDfp to the default audio device, %u Hz, "
             "%u chip%s, %s\n", rate, chips, chips == 1 ? "" : "s",
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
  sid_config.sids_socket_one = usb.sids_socket_one();
  sid_config.sids_socket_two = usb.sids_socket_two();
  sid_config.fmopl_sid = usb.fmopl_sid();

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
    } else {
      printf("  length   : not in %s, using %u:%02u\n", db_path,
             kDefaultSongMs / 60000u, (kDefaultSongMs / 1000u) % 60u);
    }
  } else if (is_sid && use_songlengths) {
    /* A path given on the command line that is not there is a mistake, and
     * saying "none found" about it would hide which of the two happened. */
    if (songlengths_path != nullptr) {
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
    for (data_t r = 0; r <= 0x18; r++) usb.write(r, 0x00, 4);
    usb.flush();
    usb.mute(true);
  };
  const auto restore_registers = [&]() {
    if (no_device) return;
    /* Unmute first, then put the registers back, so the board is listening by
     * the time the values that make the sound arrive. */
    usb.mute(false);
    for (data_t r = 0; r <= 0x18; r++) usb.write(r, machine.sid().peek(r), 4);
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
          /* Fast forward is a **seek**, and it has to stop pacing to be one.
           *
           * It cannot be audible fast playback over USBSID: every write carries
           * the gap that should precede it and the board sits those gaps out, so
           * the board cannot be driven faster than the tune's own timing however
           * quickly the frames are produced.
           *
           * Nor is it enough to emulate more frames per wait, which is what this
           * did first. `wait_for_frame` measures the deadline from a fixed point
           * as `frame_us * (frame - base)`, so advancing the frame counter four
           * at a time moves the deadline four frames too: the same wall clock
           * rate, four times the work per frame of it. On a tune anywhere near
           * real time that overran, the lag passed the pacer's 250 ms threshold
           * and it rebased over and over. It was slower, not faster, and LouD
           * heard exactly that.
           *
           * So: no pacing at all while it runs, writes thrown away, chip
           * silenced going in and caught up from the register file coming out,
           * and the pacer re-anchored so the schedule afterwards is measured
           * from where the seek ended rather than from where it began. */
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
        const uint32_t song_ms = song_length_ms(lengths, player.song(), use_songlengths);
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
      size_t n;
      while ((n = soft.take(soft_buf.data(), soft_buf.size())) != 0) {
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
      const uint32_t song_ms = song_length_ms(lengths, player.song(), use_songlengths);
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
      const uint32_t ms = song_length_ms(lengths, player.song(), use_songlengths);
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
  return 0;
}
