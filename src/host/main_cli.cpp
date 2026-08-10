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
#include <vector>

#include "machine.h"
#include "pacing.h"
#include "util/logging.h"
#include "player.h"
#include "prgfile.h"
#include "sid_trace.h"
#include "sid_usbsid.h"
#include "sidfile.h"

using namespace usbsid;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

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
    "  -T, --trace FILE  write every SID register event to FILE\n"
    "      --pal         force PAL timing\n"
    "      --ntsc        force NTSC timing\n"
    "\n"
    "  hardware:\n"
    "  -rr               read the SID back from the chip, not the mirror\n"
    "  -f                force everything into socket two\n"
    "  -fa XX            force everything to physical base $XX (hex)\n"
    "      --overhead N  cycles one hardware access costs (default 1)\n"
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
}

} /* namespace */

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
  VideoModel forced_model = VideoModel::Count; /* means "not forced" */

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

  if (trace_path != nullptr) {
    trace_buffer.resize(4u * 1000u * 1000u);
    trace = new TraceSidBackend(trace_buffer.data(), trace_buffer.size());
    machine.set_sid_backend(*trace);
    no_device = true;
  } else if (!no_device) {
    if (usb.open()) {
      printf("  device   : USBSID-Pico, pcb v%d, %d SID%s "
             "(socket one %d, socket two %d)\n",
             usb.pcb_version(), usb.num_sids(), usb.num_sids() == 1 ? "" : "s",
             usb.sids_socket_one(), usb.sids_socket_two());
      machine.set_sid_backend(usb);
    } else {
      printf("  device   : none found, playing silently\n");
      no_device = true;
    }
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
    if (!player.init_tune(song)) {
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
  printf("  press ctrl-c to stop\n");

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  const uint64_t frame_limit = (seconds > 0)
    ? static_cast<uint64_t>(static_cast<double>(seconds) * pacer.frame_rate())
    : 0;

  uint64_t frame = 0;
  while (!g_stop && (frame_limit == 0 || frame < frame_limit)) {
    player.run_frame();
    ++frame;

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

    if (!no_device) pacer.wait_for_frame(frame);
  }

  player.stop();
  printf("\n  stopped after %llu frames\n",
         static_cast<unsigned long long>(frame));

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
