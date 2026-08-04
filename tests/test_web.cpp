/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_web.cpp
 * The browser backend, tested without a browser.
 *
 * The web build is the same emulation as every other build with one thing
 * swapped: register writes go into a ring in the WASM heap instead of down a
 * USB cable, and a page drains it. The ring is plain C++, so all of it can be
 * checked here, natively, at the speed of the rest of the suite: the entry
 * layout the page reads, what happens when the page stops draining, and, the
 * one that matters, that a tune drained frame by frame comes out as the same
 * stream of registers and gaps a traced run produces.
 *
 * What cannot be checked here is the JavaScript and the WASM link, which need
 * emscripten and node. That is temp/tools/web_smoke.mjs.
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
#include <cstdlib>
#include <vector>

#include "machine_harness.h"
#include "player.h"
#include "sid_trace.h"
#include "sid_web.h"
#include "test_common.h"
#include "tests.h"
#include "usplayer.h"

#ifndef US_TUNE_DIR
#define US_TUNE_DIR "/mnt/loud/DocThierry/retro/Commodore64/sidtunes/favorites"
#endif

using namespace usbsid;
using namespace us_test;

namespace {

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

/* ---- what the page sees ------------------------------------------------- *
 *
 * The drain loop, written the way usplayer-web.js writes it: take everything
 * between tail and head, then put the new tail back. Reading the entries out
 * of the byte array rather than through a helper is deliberate, because the
 * byte layout is the contract with the JavaScript.
 */

struct DrainedWrite { data_t reg; data_t value; uint16_t cycles; };

/**
 * @brief FNV-1a over a drained stream.
 *
 * The same hash the node smoke test computes over the same tune, so the
 * WebAssembly build can be checked against this one with a single number. If
 * they differ, the emulation compiled to wasm is not the emulation that runs
 * here, which is the one thing about the web build that cannot be checked
 * natively.
 */
uint32_t stream_hash(const std::vector<DrainedWrite> & writes)
{
  uint32_t h = 2166136261u;
  for (const DrainedWrite & w : writes) {
    const uint8_t bytes[4] = { w.reg, w.value,
                               static_cast<uint8_t>(w.cycles >> 8),
                               static_cast<uint8_t>(w.cycles & 0xff) };
    for (uint8_t b : bytes) { h = (h ^ b) * 16777619u; }
  }
  return h;
}

uint32_t drain(WebSidBackend & backend, std::vector<DrainedWrite> & out)
{
  const uint32_t head = backend.head();
  const uint32_t tail = backend.tail();
  const uint8_t * ring = backend.ring_ptr();
  const uint32_t entries = backend.ring_entries();

  for (uint32_t i = tail; i != head; i++) {
    const uint8_t * e = &ring[(i % entries) * WebSidBackend::kEntryBytes];
    out.push_back({ e[0], e[1],
                    static_cast<uint16_t>((e[2] << 8) | e[3]) });
  }
  backend.set_tail(head);
  return head - tail;
}

/* ---- the ring on its own ------------------------------------------------ */

int test_ring_shape(void)
{
  WebSidBackend backend;

  US_CHECK_EQ_U(backend.head(), 0u, "a fresh ring is empty");
  US_CHECK_EQ_U(backend.pending(), 0u, "and nothing is pending");

  /* The gap sent is one short of the gap measured: performing the access
   * costs the board a cycle of its own. Zero and one both send zero, because
   * this is unsigned and wrapping would ask for a sixty five millisecond
   * pause in the middle of a tune. */
  backend.write(0x18, 0x0f, 100);
  backend.write(0x01, 0x22, 1);
  backend.write(0x00, 0x33, 0);
  backend.write(0x24, 0x44, 0xffff);

  US_CHECK_EQ_U(backend.pending(), 4u, "four writes are pending");
  US_CHECK_EQ_U(backend.writes(), 4u, "and four were counted");

  std::vector<DrainedWrite> got;
  US_CHECK_EQ_U(drain(backend, got), 4u, "the page took four");
  US_CHECK_EQ_U(backend.pending(), 0u, "which emptied the ring");

  US_CHECK_EQ_U(got[0].reg, 0x18u, "register of the first write");
  US_CHECK_EQ_U(got[0].value, 0x0fu, "value of the first write");
  US_CHECK_EQ_U(got[0].cycles, 100u, "a gap of 100 is sent as 100");
  US_CHECK_EQ_U(got[1].cycles, 1u, "a gap of 1 is sent as 1");
  US_CHECK_EQ_U(got[2].cycles, 0u, "a gap of 0 does not wrap");
  US_CHECK_EQ_U(got[3].reg, 0x24u, "the second chip keeps its register");
  US_CHECK_EQ_U(got[3].cycles, 0xffffu, "the widest gap survives");

  /* A frame boundary is a flush, and that is all it is: it moves no writes. */
  const uint32_t head_before = backend.head();
  backend.flush();
  US_CHECK_EQ_U(backend.flushes(), 1u, "the flush was counted");
  US_CHECK_EQ_U(backend.head(), head_before, "and queued nothing");

  /* Silencing drops what is queued, so the reset the page sends is not stuck
   * behind a burst from a tune that has already stopped. */
  backend.write(0x04, 0x21, 10);
  backend.reset_hardware();
  US_CHECK_EQ_U(backend.pending(), 0u, "silencing dropped the backlog");
  US_CHECK_EQ_U(backend.resets(), 1u, "and asked the page to reset");

  /* Gaps too wide to carry with a write are counted and dropped, the same as
   * the desktop backend does: the page is what keeps real time. */
  backend.wait(50000);
  US_CHECK_EQ_U(backend.cycles_waited(), 50000u, "the wait was counted");
  US_CHECK_EQ_U(backend.pending(), 0u, "and queued nothing");

  /* Reads cannot be answered: the ring only goes one way. */
  US_CHECK_EQ_U(backend.read(0x1b, 10), 0xffu, "a read answers open bus");
  US_CHECK_EQ_U(backend.reads(), 1u, "and is counted");

  return 0;
}

int test_ring_overflow(void)
{
  WebSidBackend backend;
  const uint32_t cap = backend.ring_entries();

  for (uint32_t i = 0; i < cap + 100; i++) {
    backend.write(static_cast<data_t>(i & 0x1f), static_cast<data_t>(i), 4);
  }

  US_CHECK_EQ_U(backend.pending(), cap, "the ring holds its capacity");
  US_CHECK_EQ_U(backend.drops(), 100u, "and counted every write it lost");
  US_CHECK_EQ_U(backend.writes(), cap + 100, "all of them were seen");

  /* What survived is the *oldest* run, unbroken. Dropping the newest is what
   * keeps the stream in order: overwriting the oldest would leave the page
   * draining entries from two different parts of the tune. */
  std::vector<DrainedWrite> got;
  drain(backend, got);
  US_CHECK_EQ_U(got.size(), cap, "the page took the whole ring");
  bool ordered = true;
  for (uint32_t i = 0; i < cap; i++) {
    if (got[i].value != static_cast<data_t>(i)) { ordered = false; break; }
  }
  US_CHECK(ordered, "and they came out in the order they were written");

  return 0;
}

int test_ring_wraps(void)
{
  WebSidBackend backend;
  const uint32_t cap = backend.ring_entries();
  const uint32_t total = cap * 3 + 17; /* past the index wrap, three times */

  std::vector<DrainedWrite> got;
  for (uint32_t i = 0; i < total; i++) {
    backend.write(static_cast<data_t>(i & 0x1f), static_cast<data_t>(i & 0xff),
                  static_cast<uint16_t>((i % 300) + 1));
    if ((i % 97) == 0) drain(backend, got); /* the page, draining as it goes */
  }
  drain(backend, got);

  US_CHECK_EQ_U(got.size(), total, "every write came through");
  US_CHECK_EQ_U(backend.drops(), 0u, "a page that drains loses nothing");

  bool intact = true;
  for (uint32_t i = 0; i < total; i++) {
    const uint16_t want = static_cast<uint16_t>((i % 300) + 1); /* as given */
    if (got[i].reg != static_cast<data_t>(i & 0x1f) ||
        got[i].value != static_cast<data_t>(i & 0xff) ||
        got[i].cycles != want) {
      intact = false;
      printf("  entry %u: $%02x:%02x [C]%u\n", i, got[i].reg, got[i].value,
             got[i].cycles);
      break;
    }
  }
  US_CHECK(intact, "registers, values and gaps survive the index wrapping");

  return 0;
}

/* ---- a real tune, drained frame by frame -------------------------------- */

/**
 * @brief The web path against the reference path, event for event.
 *
 * Two identically built machines play the same tune, one recording into a
 * trace and one filling the ring, which the test drains once a frame the way
 * the page does. Anything the ring loses, reorders or mistimes shows up as a
 * mismatch. This is the test that would catch the ring being drained wrongly,
 * an entry being packed in the wrong byte order, or the access cycle being
 * subtracted twice.
 */
int test_tune_through_the_ring(void)
{
  const char * path = US_TUNE_DIR "/psid/Aint_Somebody.sid";
  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("  skipped the tune comparison, no tune available\n");
    return 0;
  }

  const uint32_t frames = 50; /* a second of PAL */

  /* the reference: what the emulation produced */
  std::vector<TraceSidBackend::Event> buffer(200000);
  TraceSidBackend trace(buffer.data(), buffer.size());
  TestC64 traced;
  traced.machine.set_sid_backend(trace);
  Player reference(traced.machine);
  US_CHECK(reference.load_sid(bytes.data(), bytes.size()), "the tune loads");
  US_CHECK(reference.init_tune(0), "and initialises");
  trace.reset();
  reference.run_frames(frames);

  /* the web path: the same tune, drained once a frame */
  WebSidBackend backend;
  TestC64 web;
  web.machine.set_sid_backend(backend);
  Player player(web.machine);
  US_CHECK(player.load_sid(bytes.data(), bytes.size()), "it loads again");
  US_CHECK(player.init_tune(0), "and initialises again");
  backend.reset();

  std::vector<DrainedWrite> got;
  uint32_t drains = 0;
  for (uint32_t f = 0; f < frames; f++) {
    player.run_frame();
    drain(backend, got);
    ++drains;
  }

  US_CHECK_EQ_U(drains, frames, "one drain per frame");
  US_CHECK_EQ_U(backend.flushes(), frames, "one flush per frame");
  US_CHECK_EQ_U(backend.drops(), 0u, "nothing was dropped");

  size_t expected = 0;
  for (size_t i = 0; i < trace.count(); i++) {
    if (trace.at(i).kind == 'w') ++expected;
  }
  printf("  %u frames: %zu writes traced, %zu drained, %u waits, hash %08x\n",
         frames, expected, got.size(),
         static_cast<unsigned>(backend.cycles_waited()), stream_hash(got));

  US_CHECK(expected > 100, "the tune wrote a useful number of registers");
  US_CHECK_EQ_U(got.size(), expected, "the ring carried every write");

  size_t n = 0;
  bool same = true;
  for (size_t i = 0; i < trace.count() && n < got.size(); i++) {
    const TraceSidBackend::Event & e = trace.at(i);
    if (e.kind != 'w') continue;
    /* Straight through: the access cycle came off upstream, in
     * cycles_since_last_event(), so the ring carries what the trace saw */
    const uint16_t want = e.delta;
    if (got[n].reg != e.reg || got[n].value != e.value ||
        got[n].cycles != want) {
      same = false;
      printf("  write %zu: ring $%02x:%02x [C]%u, trace $%02x:%02x [C]%u\n",
             n, got[n].reg, got[n].value, got[n].cycles, e.reg, e.value, want);
      break;
    }
    ++n;
  }
  US_CHECK(same, "every drained write matches the traced one");

  return 0;
}

/* ---- what the page asks the C API for ------------------------------------ */

/**
 * @brief The queries the wall clock pump is paced by.
 *
 * The page steps whole frames against real time, so it needs the tune's frame
 * rate, and it sets the device's SID clock from the tune's region. Both are
 * wrong in a way that is easy to miss: PAL is 50.125 frames a second, not 50,
 * and a page that rounds it drifts a whole frame every eight seconds.
 */
int test_api_queries(void)
{
  const char * path = US_TUNE_DIR "/psid/Aint_Somebody.sid";
  std::vector<data_t> bytes;
  if (!read_file(path, bytes)) {
    printf("  skipped the API queries, no tune available\n");
    return 0;
  }

  load_sidtune(bytes.data(), static_cast<int>(bytes.size()), 0);
  US_CHECK(usplayer_loaded(), "the tune loaded through the C API");
  US_CHECK(usplayer_is_prg() == false, "and it is not a program");
  US_CHECK(usplayer_songs() >= 1, "it has at least one song");

  const double hz = usplayer_refresh_hz();
  const uint32_t clock = usplayer_clock_hz();
  printf("  %s, %u song%s, %u Hz clock, %.3f frames/s\n",
         usplayer_tune_name(), usplayer_songs(),
         usplayer_songs() == 1 ? "" : "s", clock, hz);

  if (usplayer_is_pal()) {
    US_CHECK_EQ_U(clock, 985248u, "a PAL tune asks for the PAL clock");
    US_CHECK(hz > 50.1 && hz < 50.2, "PAL is 50.125 frames a second, not 50");
  } else {
    US_CHECK(clock > 1000000u, "an NTSC tune asks for an NTSC clock");
    US_CHECK(hz > 59.7 && hz < 60.0, "NTSC is 59.83 frames a second, not 60");
  }

  /* Rubbish does not leave a stale tune looking loaded, which is what the
   * page's "not a SID, try it as a program" fallback depends on. */
  std::vector<data_t> junk(400, 0x5a);
  load_sidtune(junk.data(), static_cast<int>(junk.size()), 0);
  US_CHECK(usplayer_loaded() == false, "a file that is not a SID is refused");

  stop_sidplayer();
  return 0;
}

} /* namespace */

int us_test_web(void)
{
  us_test_failures = 0;
  us_test_checks = 0;
  US_TEST_BEGIN("web backend");

  test_ring_shape();
  test_ring_overflow();
  test_ring_wraps();
  test_tune_through_the_ring();
  test_api_queries();

  US_TEST_END("web backend");
}

US_TEST_MAIN(us_test_web)
