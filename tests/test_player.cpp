/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_player.cpp
 * Step 2.8 gate: PSID and RSID parsing, the driver install, and a tune that
 * actually runs and writes SID registers, headless.
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

#include <cstdio>
#include <cstring>
#include <vector>

#include "machine_harness.h"
#include "boot_image.h"
#include "player.h"
#include "sid_trace.h"
#include "sidfile.h"
#include "test_common.h"
#include "tests.h"

#ifndef US_TUNE_DIR
#define US_TUNE_DIR "/mnt/loud/DocThierry/retro/Commodore64/sidtunes/favorites"
#endif

using namespace usbsid;
using namespace us_test;

namespace {

/* ---- a hand built PSID header ------------------------------------------- */

int test_parse_synthetic(void)
{
  /* A minimal version 2 PSID: two subtunes, load $1000, init $1000,
   * play $1003, CIA driven for song 2. */
  std::vector<data_t> file(0x7c + 4, 0);
  memcpy(file.data(), "PSID", 4);
  file[0x05] = 0x02;                  /* version 2 */
  file[0x07] = 0x7c;                  /* data offset */
  file[0x08] = 0x10; file[0x09] = 0x00; /* load $1000 */
  file[0x0a] = 0x10; file[0x0b] = 0x00; /* init $1000 */
  file[0x0c] = 0x10; file[0x0d] = 0x03; /* play $1003 */
  file[0x0f] = 0x02;                  /* two songs */
  file[0x11] = 0x02;                  /* default song 2 */
  file[0x15] = 0x02;                  /* speed: song 2 uses a CIA */
  memcpy(&file[0x16], "Test Tune", 9);
  memcpy(&file[0x36], "Nobody", 6);
  memcpy(&file[0x56], "2026", 4);
  file[0x77] = 0x04;                  /* flags: built for PAL */
  file[0x78] = 0x04;                  /* driver may go at page 4 */
  file[0x79] = 0x02;
  file[0x7c] = 0x60;                  /* the "tune": rts */

  SidFile tune;
  US_CHECK(sidfile_parse(file.data(), file.size(), tune), "the header parses");
  US_CHECK(tune.valid, "and is marked valid");
  US_CHECK(tune.is_rsid == false, "it is a PSID");
  US_CHECK_EQ_U(tune.version, 2u, "version 2");
  US_CHECK_EQ_U(tune.load_addr, 0x1000u, "load address");
  US_CHECK_EQ_U(tune.init_addr, 0x1000u, "init address");
  US_CHECK_EQ_U(tune.play_addr, 0x1003u, "play address");
  US_CHECK_EQ_U(tune.songs, 2u, "two songs");
  US_CHECK_EQ_U(tune.start_song, 2u, "default song");
  US_CHECK_EQ_STR(tune.name, "Test Tune", "title");
  US_CHECK_EQ_STR(tune.author, "Nobody", "author");
  US_CHECK(tune.video_known, "the video standard is known");
  US_CHECK(tune.video_model == VideoModel::Pal6569, "and it is PAL");
  US_CHECK(tune.song_uses_cia(2), "song 2 is CIA driven");
  US_CHECK(tune.song_uses_cia(1) == false, "song 1 is raster driven");
  US_CHECK_EQ_U(tune.data_size, 4u, "the payload is what follows the header");

  /* rubbish is rejected rather than half accepted */
  std::vector<data_t> junk(200, 0x55);
  SidFile bad;
  US_CHECK(sidfile_parse(junk.data(), junk.size(), bad) == false,
           "a file with no magic is rejected");
  US_CHECK(sidfile_parse(file.data(), 4, bad) == false, "a truncated file too");

  return 0;
}

/* ---- real tunes ---------------------------------------------------------- */

struct TuneCase { const char * path; const char * label; };

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

int test_real_tunes(void)
{
  const TuneCase tunes[] = {
    { US_TUNE_DIR "/rsid/Afterburner.sid", "Afterburner (RSID)" },
    { US_TUNE_DIR "/rsid/Antarctic_Burial.sid", "Antarctic Burial (RSID)" },
    { US_TUNE_DIR "/rsid/50Kent_in_da_Club.sid", "50Kent (RSID)" },
    { US_TUNE_DIR "/psid/A2_Arcade_Memories_3SID.sid", "Arcade Memories (3SID)" },
  };

  for (const TuneCase & t : tunes) {
    std::vector<data_t> bytes;
    if (!read_file(t.path, bytes)) {
      printf("  skipped %s (not readable)\n", t.label);
      continue;
    }

    SidFile tune;
    ++us_test_checks;
    if (!sidfile_parse(bytes.data(), bytes.size(), tune)) {
      ++us_test_failures;
      printf("  FAIL %s does not parse\n", t.label);
      continue;
    }

    printf("  %-24s %s v%u, %u song%s, load $%04x-$%04x, init $%04x, "
           "play $%04x, %u SID%s, %s\n",
           t.label, tune.is_rsid ? "RSID" : "PSID", tune.version,
           tune.songs, tune.songs == 1 ? "" : "s",
           tune.load_addr, tune.load_last_addr, tune.init_addr, tune.play_addr,
           tune.sid_count, tune.sid_count == 1 ? "" : "s",
           tune.video_known
             ? vic_timing(tune.video_model).name : "video unspecified");

    US_CHECK(tune.songs >= 1, "at least one song");
    US_CHECK(tune.start_song >= 1 && tune.start_song <= tune.songs,
             "the default song is in range");
    US_CHECK(tune.load_addr >= 0x0100, "the load address is sane");
    US_CHECK(tune.data_size > 0, "there is a payload");
  }

  return 0;
}

/* ---- a tune that actually runs ------------------------------------------ */

int test_tune_runs(void)
{
  const char * path = US_TUNE_DIR "/rsid/50Kent_in_da_Club.sid";
  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("  skipped the playback test, no tune available\n");
    return 0;
  }

  TestC64 c64;
  std::vector<TraceSidBackend::Event> buffer(200000);
  TraceSidBackend trace(buffer.data(), buffer.size());
  c64.machine.set_sid_backend(trace);

  Player player(c64.machine);
  US_CHECK(player.load_sid(bytes.data(), bytes.size()), "the tune loads");
  US_CHECK(player.init_tune(0), "and initialises");

  printf("  driver installed at $%04x, playing song %u of %u\n",
         player.driver_address(), player.song(), player.songs());

  trace.reset();
  player.run_frames(50); /* one second of PAL */

  unsigned writes = 0, flushes = 0;
  data_t volume_seen = 0;
  for (size_t i = 0; i < trace.count(); i++) {
    const TraceSidBackend::Event & e = trace.at(i);
    if (e.kind == 'w') {
      ++writes;
      if (e.reg == 0x18) volume_seen = e.value;
    }
    if (e.kind == 'f') ++flushes;
  }

  printf("  50 frames produced %u SID writes and %u flushes\n", writes, flushes);
  US_CHECK(writes > 100, "the tune wrote a useful number of registers");
  US_CHECK_EQ_U(flushes, 50u, "one flush per frame");
  US_CHECK(volume_seen != 0, "the tune set a volume");

  /* the first few writes, so a change of behaviour is visible in the log */
  unsigned shown = 0;
  for (size_t i = 0; i < trace.count() && shown < 5; i++) {
    if (trace.at(i).kind != 'w') continue;
    printf("    [W]$%02x:%02x [C]%5u\n",
           trace.at(i).reg, trace.at(i).value, trace.at(i).delta);
    ++shown;
  }

  /* subtunes */
  const uint16_t before = player.song();
  player.next_subtune();
  US_CHECK(player.song() != before || player.songs() == 1,
           "next subtune moved, or there was only one");
  player.previous_subtune();
  US_CHECK_EQ_U(player.song(), before, "previous subtune came back");

  player.run_frames(5);
  US_CHECK(player.playing(), "still playing after switching subtunes");

  return 0;
}

/* ---- the sweep's own listing --------------------------------------------- */

int test_dir_listing(void)
{
  /* The sweeps are the regression net for the whole emulation, and for a while
   * the net had a hole in it: they listed the collection through
   * `popen("ls ...")`, which reported 160 tunes, then 62, then 0 on three
   * consecutive runs of an unchanged tree, and a sweep that found nothing
   * printed "skipped" and passed. This is the test that the replacement cannot
   * do that again.
   */
  namespace fs = std::filesystem;
  std::error_code ec;

  const fs::path root = fs::temp_directory_path(ec) / "usplayer-listing-test";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (ec) { printf("  skipped: no writable temp directory\n"); return 0; }

  std::vector<std::string> got;

  /* A directory that is not there is a machine without the collection. */
  US_CHECK(us_list_dir((root / "nothing-here").string().c_str(), ".sid", got)
             == UsDir::Missing, "a missing directory says so");

  /* A directory that is there and yields nothing is the case that used to pass
   * quietly. It has to be distinguishable from the one above. */
  US_CHECK(us_list_dir(root.string().c_str(), ".sid", got) == UsDir::Empty,
           "an empty directory is not the same as a missing one");
  US_CHECK_EQ_U(got.size(), 0u, "nothing was listed from an empty directory");

  /* Files, sorted, and only the extension asked for. Written out of order on
   * purpose: a sweep that fails has to name the same tune on every machine. */
  for (const char * name : { "b.sid", "a.sid", "c.SID", "notes.txt", "d.prg" }) {
    FILE * f = fopen((root / name).string().c_str(), "wb");
    if (f != nullptr) { fputc('x', f); fclose(f); }
  }
  US_CHECK(us_list_dir(root.string().c_str(), ".sid", got) == UsDir::Listed,
           "a directory with files says so");
  US_CHECK_EQ_U(got.size(), 3u, "three .sid files, and not the .txt or the .prg");
  if (got.size() == 3) {
    US_CHECK_EQ_STR(fs::path(got[0]).filename().string().c_str(), "a.sid",
                    "listed in order");
    US_CHECK_EQ_STR(fs::path(got[2]).filename().string().c_str(), "c.SID",
                    "the extension test is case insensitive");
  }

  /* Appends rather than clears, which is what lets one sweep walk two
   * directories into one list. */
  const size_t before = got.size();
  us_list_dir(root.string().c_str(), ".prg", got);
  US_CHECK_EQ_U(got.size(), before + 1, "a second call appends");

  fs::remove_all(root, ec);
  return 0;
}

/* ---- how many tunes survive a few frames -------------------------------- */

int test_tune_sweep(void)
{
  /* Load every tune in the collection, run a second of each, and report how
   * many produced SID writes. This is the regression net for everything the
   * emulation does: a tune that stops writing has hit a bug somewhere. */
  static const char * const dirs[] = { US_TUNE_DIR "/psid", US_TUNE_DIR "/rsid" };

  unsigned total = 0, parsed = 0, played = 0, silent = 0;
  unsigned present = 0, empty = 0;
  std::vector<std::string> files;

  for (const char * dir : dirs) {
    switch (us_list_dir(dir, ".sid", files)) {
      case UsDir::Missing: break;
      case UsDir::Empty:   ++present; ++empty;
                           printf("    empty: %s\n", dir); break;
      case UsDir::Listed:  ++present; break;
    }
  }

  for (const std::string & file : files) {
    const char * path = file.c_str();

    std::vector<data_t> bytes;
    if (!read_file(path, bytes)) continue;
    ++total;

    SidFile tune;
    if (!sidfile_parse(bytes.data(), bytes.size(), tune)) continue;
    ++parsed;

    TestC64 c64;
    NullSidBackend backend;
    c64.machine.set_sid_backend(backend);
    Player player(c64.machine);
    if (!player.load_sid(bytes.data(), bytes.size())) continue;
    if (!player.init_tune(0)) continue;

    backend.reset();
    /* Six seconds, not one: several tunes unpack or wait for a couple of
     * seconds before their first register write, and judging them after one
     * second calls a working tune silent. */
    player.run_frames(300);

    if (backend.writes > 50) ++played;
    else { ++silent; printf("    silent: %s\n", path); }
  }

  printf("  %u tunes, %u parsed, %u played, %u silent\n",
         total, parsed, played, silent);

  /* No collection on this machine at all: the suite still has to run, and a
   * skip is honest. A collection that is *there* and yielded nothing is not:
   * that is a broken harness reporting a pass, which is what this whole
   * arrangement replaced. See us_list_dir(). */
  if (present == 0) {
    ++us_test_checks;
    printf("  skipped the sweep, no tune directories on this machine\n");
    return 0;
  }
  US_CHECK_EQ_U(empty, 0u, "every tune directory that exists has tunes in it");
  US_CHECK(total > 0, "the sweep found tunes to run");
  if (total == 0) return us_test_failures;
  US_CHECK_EQ_U(parsed, total, "every tune in the collection parses");
  /* Every tune in the collection plays. The bar used to be eight in ten,
   * because fourteen of them were silent; the three faults behind that are
   * fixed (the CBM80 backup, entering the driver through a reset, and voice
   * three being readable), so the bar is where it belongs. A single tune
   * dropping out of this is a regression worth stopping for. */
  US_CHECK_EQ_U(played, total, "every tune plays");

  return 0;
}

} /* namespace */

/* ---- the recorded boot state still matches the ROMs -------------------- *
 *
 * `src/player/boot_image.h` is what a C64 looks like once the KERNAL has
 * reached the BASIC prompt, recorded so that starting a tune does not have to
 * spend 2 240 002 cycles working it out again. It is generated, and generated
 * files rot: if the ROMs in src/mem/roms are ever replaced, the table goes on
 * describing the old ones and every tune starts on a machine that never
 * existed. So it is checked against the real thing here.
 *
 * Regenerate with temp/tools/gen_boot_image.cpp if this fails after a
 * deliberate ROM change.
 */
int test_boot_image_matches_roms(void)
{
  TestC64 c64;
  NullSidBackend sink;
  c64.machine.set_sid_backend(sink);
  c64.machine.power_on();

  /* the boot, watched for in screen memory the way Player does it */
  static const data_t kReady[] = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2e };
  bool reached = false;
  for (uint64_t cycles = 0; cycles < 20000000ull && !reached; cycles += 5000) {
    c64.machine.run(5000);
    for (addr_t base = 0x0400; base < 0x07e2 && !reached; base++) {
      size_t i = 0;
      while (i < sizeof(kReady) &&
             c64.machine.ram().dma_read(static_cast<addr_t>(base + i)) == kReady[i]) {
        i++;
      }
      if (i == sizeof(kReady)) reached = true;
    }
  }
  US_CHECK(reached, "the KERNAL still boots to the BASIC prompt");
  if (!reached) return 0;

  const uint32_t line = c64.machine.vic().timing().cycles_per_line;
  const uint32_t phase = c64.machine.vic().raster() * line +
                         c64.machine.vic().cycle_in_line();
  US_CHECK_EQ_U(phase, kBootImagePhase,
                "and finishes at the point in the frame the table records");

  /* every byte, both ways round: nothing recorded that is not there, and
   * nothing there that is not recorded */
  unsigned wrong = 0, missing = 0, count = 0;
  uint16_t next = 0;
  for (uint32_t addr = 0; addr < 0x10000; addr++) {
    const data_t value = c64.machine.ram().dma_read(static_cast<addr_t>(addr));
    const data_t blank = ((addr >> 6) & 1) ? 0xff : 0x00;
    const bool listed = (next < kBootImageCount) &&
                        (kBootImage[next].addr == addr);
    if (value != blank) {
      ++count;
      if (!listed) ++missing;
      else if (kBootImage[next].value != value) ++wrong;
    } else if (listed) {
      ++wrong; /* recorded a change that the boot does not make */
    }
    if (listed) ++next;
  }

  printf("  boot image: %u bytes of RAM, phase %u\n", count, phase);
  US_CHECK_EQ_U(missing, 0u, "every byte the boot changes is in the table");
  US_CHECK_EQ_U(wrong, 0u, "and every byte in the table has the right value");
  US_CHECK_EQ_U(next, kBootImageCount, "with nothing left over");

  return 0;
}

int us_test_player(void)
{
  US_TEST_BEGIN("player");

  test_boot_image_matches_roms();
  test_parse_synthetic();
  test_real_tunes();
  test_tune_runs();
  test_dir_listing();
  test_tune_sweep();

  US_TEST_END("player");
}

US_TEST_MAIN(us_test_player)
