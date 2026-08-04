/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_sid.cpp
 * Step 2.7 gate: the address translation for one to four chips, and the cycle
 * deltas that go out with every write.
 *
 * The deltas are checked against a 6502 program whose cycle counts are known
 * exactly, which is a stronger check than a tune trace would be at this point:
 * the SID file loader only arrives at step 2.8, and a trace you cannot predict
 * can only be compared, not verified.
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
#include <vector>

#include "machine_harness.h"
#include "mos6581_8580.h"
#include "sid_trace.h"
#include "sid_voice3.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;
using namespace us_test;

namespace {

/* ---- address translation ------------------------------------------------ */

int test_translation(void)
{
  Bus bus;
  NullSidBackend null_backend;
  Mos6581_8580 sid(bus, null_backend);
  uint8_t chip = 0;

  {
    /* one chip at the usual place */
    SidConfig & c = sid.config();
    c.count = 1;
    c.base[0] = 0xd400;

    US_CHECK_EQ_U(sid.translate(0xd400, chip), 0x00u, "$d400 is register 0");
    US_CHECK_EQ_U(chip, 1u, "and it is the first chip");
    US_CHECK_EQ_U(sid.translate(0xd418, chip), 0x18u, "$d418 is the volume register");
    US_CHECK_EQ_U(sid.translate(0xd41f, chip), 0x1fu, "$d41f is the last one");

    /* a single chip machine still answers the spare addresses, because tunes
     * write to them even when only one chip is fitted */
    US_CHECK_EQ_U(sid.translate(0xd420, chip), 0x00u, "$d420 folds onto the one chip");
    US_CHECK_EQ_U(sid.translate(0xd5c0, chip), 0x00u, "$d5c0 folds too");

    /* the PLA decodes the whole of $d400-$d7ff to the chip, so the last
     * mirror is register $1f, which is what the test suites write to */
    US_CHECK_EQ_U(sid.translate(0xd7ff, chip), 0x1fu, "$d7ff is register $1f");
    US_CHECK_EQ_U(sid.translate(0xd600, chip), 0x00u, "and $d600 is register 0");

    /* and nothing outside the page does */
    US_CHECK_EQ_U(sid.translate(0xd800, chip), kSidNotMapped, "colour RAM is not a SID");
    US_CHECK_EQ_U(sid.translate(0xdc00, chip), kSidNotMapped, "nor is the CIA");
  }

  {
    /* two chips, the second at $d420 */
    SidConfig & c = sid.config();
    c.count = 2;
    c.base[0] = 0xd400;
    c.base[1] = 0xd420;

    US_CHECK_EQ_U(sid.translate(0xd400, chip), 0x00u, "first chip");
    US_CHECK_EQ_U(chip, 1u, "chip 1");
    US_CHECK_EQ_U(sid.translate(0xd420, chip), 0x20u, "second chip starts at $20");
    US_CHECK_EQ_U(chip, 2u, "chip 2");
    US_CHECK_EQ_U(sid.translate(0xd43f, chip), 0x3fu, "and ends at $3f");
  }

  {
    /* four chips */
    SidConfig & c = sid.config();
    c.count = 4;
    c.base[0] = 0xd400;
    c.base[1] = 0xd420;
    c.base[2] = 0xd440;
    c.base[3] = 0xd460;

    US_CHECK_EQ_U(sid.translate(0xd440, chip), 0x40u, "third chip at $40");
    US_CHECK_EQ_U(chip, 3u, "chip 3");
    US_CHECK_EQ_U(sid.translate(0xd460, chip), 0x60u, "fourth chip at $60");
    US_CHECK_EQ_U(chip, 4u, "chip 4");
  }

  {
    /* forcing playback into socket two shifts the first chip along */
    SidConfig & c = sid.config();
    c.count = 1;
    c.base[0] = 0xd400;
    c.base[1] = 0;
    c.force_socket_two = true;
    c.sids_socket_one = 1;
    US_CHECK_EQ_U(sid.translate(0xd400, chip), 0x20u,
                  "one SID in socket one moves the tune to $20");
    c.sids_socket_one = 2;
    US_CHECK_EQ_U(sid.translate(0xd400, chip), 0x40u,
                  "two SIDs in socket one move it to $40");
    c.force_socket_two = false;

    /* or an explicit address wins over everything */
    c.force_address = true;
    c.forced_address = 0x60;
    US_CHECK_EQ_U(sid.translate(0xd407, chip), 0x67u, "a forced address is used as is");
    c.force_address = false;
  }

  {
    /* the FM/OPL address goes to whichever chip claims it */
    SidConfig & c = sid.config();
    c.count = 2;
    c.base[0] = 0xd400;
    c.base[1] = 0xd420;
    c.fmopl_sid = 2;
    US_CHECK_EQ_U(sid.translate(0xdf40, chip), 0x20u, "FM/OPL lands on chip 2");
    US_CHECK_EQ_U(chip, 2u, "and reports chip 2");

    c.fmopl_sid = -1;
    const data_t parked = sid.translate(0xdf40, chip);
    US_CHECK(parked >= 0x80, "with no FM/OPL chip the write is parked out of range");
    US_CHECK(chip > 4, "and no real chip claims it");
  }

  return 0;
}

/* ---- the cycle deltas --------------------------------------------------- */

/*
 * A program with cycle counts that can be worked out on paper:
 *
 *   lda #$11        2
 * loop:
 *   sta $d400       4   <- write, 12 cycles after the previous one
 *   nop             2
 *   nop             2
 *   sta $d401       4   <- write, 8 cycles later
 *   jmp loop        3
 */
int test_cycle_deltas(void)
{
  TestC64 c64;
  if (!c64.boot()) {
    ++us_test_failures; ++us_test_checks;
    printf("  FAIL cycle deltas: the machine did not boot\n");
    return 1;
  }

  std::vector<TraceSidBackend::Event> buffer(4096);
  TraceSidBackend trace(buffer.data(), buffer.size());
  c64.machine.set_sid_backend(trace);
  trace.reset();

  /* blank the screen so no bad line steals a cycle in the middle of it, and
   * stop the KERNAL interrupt so nothing else runs */
  c64.machine.mmu().write(0xd011, 0x0b);
  c64.machine.mmu().write(0xdc0d, 0x7f);
  (void)c64.machine.mmu().read(0xdc0d);

  const data_t program[] = {
    0xa9, 0x11,             /* lda #$11        */
    0x8d, 0x00, 0xd4,       /* sta $d400       */
    0xea,                   /* nop             */
    0xea,                   /* nop             */
    0x8d, 0x01, 0xd4,       /* sta $d401       */
    0x4c, 0x02, 0x03,       /* jmp $0302       */
  };
  for (size_t i = 0; i < sizeof(program); i++) {
    c64.machine.ram().dma_write(static_cast<addr_t>(0x0300 + i), program[i]);
  }

  c64.sync_to_instruction();
  c64.machine.cpu().sp(0xfd);
  c64.machine.cpu().p(0x04); /* interrupts off */
  c64.machine.cpu().pc(0x0300);

  /* Deltas are measured from the previous access and nothing resets that on a
   * frame boundary, so without this the first write of the program would carry
   * the whole KERNAL boot. This is exactly what the player does when a tune
   * starts. */
  c64.machine.sid().resync();

  /* run a few loops */
  for (unsigned i = 0; i < 200; i++) c64.machine.tick();

  US_CHECK(trace.count() >= 6, "the trace recorded some writes");

  /* the first delta is measured from whenever the last event was, so start
   * looking from the second write onwards */
  unsigned checked = 0;
  for (size_t i = 2; i + 1 < trace.count() && checked < 6; i += 2) {
    const TraceSidBackend::Event & a = trace.at(i);
    const TraceSidBackend::Event & b = trace.at(i + 1);
    if (a.kind != 'w' || b.kind != 'w') continue;

    US_CHECK_EQ_U(a.reg, 0x00u, "first write of the loop is register 0");
    US_CHECK_EQ_U(a.value, 0x11u, "and carries the accumulator");
    US_CHECK_EQ_U(b.reg, 0x01u, "second write of the loop is register 1");
    /* The gaps are eight and seven cycles; one is taken off each because
     * the access itself costs the hardware a cycle. */
    US_CHECK_EQ_U(b.delta, 7u, "sta, nop, nop, sta is eight cycles, less one");
    US_CHECK_EQ_U(a.delta, 6u, "sta, jmp, sta round the loop is seven, less one");
    ++checked;
  }
  US_CHECK(checked > 0, "found loop iterations to check");

  return 0;
}

/* ---- long gaps and the frame flush -------------------------------------- */

int test_wait_and_flush(void)
{
  {
    /* a gap longer than sixteen bits is split into waits plus a remainder */
    Bus bus;
    std::vector<TraceSidBackend::Event> buffer(64);
    TraceSidBackend trace(buffer.data(), buffer.size());
    Mos6581_8580 sid(bus, trace);
    sid.config().count = 1;
    sid.config().base[0] = 0xd400;
    trace.reset();

    bus.run(1);
    sid.io_write(0xd400, 0x01);       /* sets the baseline */
    bus.run(0x1ffff);                 /* two full chunks and a bit */
    sid.io_write(0xd401, 0x02);

    unsigned waits = 0;
    uint16_t last_delta = 0;
    for (size_t i = 0; i < trace.count(); i++) {
      if (trace.at(i).kind == 'i') waits++;
      if (trace.at(i).kind == 'w') last_delta = trace.at(i).delta;
    }
    US_CHECK_EQ_U(waits, 2u, "two full chunks were sent as waits");
    US_CHECK_EQ_U(last_delta, 0x1ffffu - 2u * 0xffffu - 1u,
                  "and the remainder, less the access cycle, rode along");
  }

  {
    /* the VIC flushes the backend at the end of every frame */
    TestC64 c64;
    std::vector<TraceSidBackend::Event> buffer(4096);
    TraceSidBackend trace(buffer.data(), buffer.size());
    c64.machine.set_sid_backend(trace);
    trace.reset();

    const uint64_t frames_before = c64.machine.vic().frames();
    while (c64.machine.vic().frames() < frames_before + 2) c64.machine.tick();

    unsigned flushes = 0;
    for (size_t i = 0; i < trace.count(); i++) {
      if (trace.at(i).kind == 'f') flushes++;
    }
    US_CHECK_EQ_U(flushes, 2u, "one flush per frame");
  }

  return 0;
}

/* ---- what the machine does with it -------------------------------------- */

int test_machine_integration(void)
{
  TestC64 c64;
  if (!c64.boot()) {
    ++us_test_failures; ++us_test_checks;
    printf("  FAIL integration: the machine did not boot\n");
    return 1;
  }

  /* a SID is fitted, so a write to its page goes to the chip and not to the
   * RAM underneath, which is what a real machine does */
  c64.machine.ram().dma_write(0xd400, 0x00);
  c64.machine.mmu().write(0xd400, 0x7e);
  US_CHECK_EQ_U(c64.machine.sid().peek(0x00), 0x7eu, "the write reached the chip");
  US_CHECK_EQ_U(c64.machine.ram().dma_read(0xd400), 0x00u,
                "and not the RAM underneath");

  /* reads come back from the register mirror while real reads are off */
  US_CHECK_EQ_U(c64.machine.mmu().read(0xd400), 0x7eu, "and reads back");

  /* the KERNAL cleared the SID at boot, so something did write to it */
  US_CHECK(c64.machine.sid().writes() > 0, "the KERNAL wrote to the SID during boot");

  return 0;
}

/* ---- a trace of a real program ------------------------------------------ */

int test_trace_dump(void)
{
  /* Not a tune yet, that needs the loader from step 2.8. This is the SID
   * initialisation the KERNAL does at boot, which is a real program writing
   * real registers, and it shows the trace format. */
  TestC64 c64;
  std::vector<TraceSidBackend::Event> buffer(256);
  TraceSidBackend trace(buffer.data(), buffer.size());
  c64.machine.set_sid_backend(trace);
  trace.reset();

  if (!c64.boot()) {
    ++us_test_failures; ++us_test_checks;
    printf("  FAIL trace: the machine did not boot\n");
    return 1;
  }

  US_CHECK(trace.count() > 0, "the boot produced SID events");
  printf("  boot wrote %u SID registers, first few events:\n",
         c64.machine.sid().writes());

  unsigned shown = 0;
  for (size_t i = 0; i < trace.count() && shown < 6; i++) {
    if (trace.at(i).kind != 'w') continue;
    printf("    [W]$%02x:%02x [C]%5u\n", trace.at(i).reg, trace.at(i).value,
           trace.at(i).delta);
    ++shown;
  }

  return 0;
}


/* ---- voice three, the only part of a SID that can be read --------------- */

int test_voice3(void)
{
  /* A sawtooth ramps. This is the shape tunes rely on when they wait for the
   * oscillator to move, and Ring Ring Ring's loop is literally
   *   ldy $d41b : iny : cpy $d41b : bne -3
   * which never ends unless $d41b climbs by one at some point. */
  {
    SidVoice3 v;
    v.write(kSidRegV3FreqLo, 0x00, 0);
    v.write(kSidRegV3FreqHi, 0x10, 0);   /* $1000 per cycle */
    v.write(kSidRegV3Ctrl, 0x20, 0);     /* sawtooth, gate off */

    /* $1000 a cycle needs sixteen cycles to carry into the top byte */
    US_CHECK_EQ_U(v.osc3(0), 0x00u, "the oscillator starts at zero");
    US_CHECK_EQ_U(v.osc3(16), 0x01u, "and has moved one step 16 cycles later");
    US_CHECK_EQ_U(v.osc3(32), 0x02u, "and another");

    /* it wraps at 24 bits, not at 16 */
    US_CHECK_EQ_U(v.osc3(16 * 255), 0xffu, "it climbs to full scale");
    US_CHECK_EQ_U(v.osc3(16 * 256), 0x00u, "and wraps round");
  }

  /* Reading it often and reading it once have to give the same answer. The
   * whole design rests on this: voice three is caught up at the moment it is
   * read rather than clocked every cycle, and that is only allowed if the two
   * cannot be told apart. */
  {
    SidVoice3 often, once;
    for (SidVoice3 * v : { &often, &once }) {
      v->write(kSidRegV3FreqLo, 0x37, 0);
      v->write(kSidRegV3FreqHi, 0x22, 0);
      v->write(kSidRegV3Ad, 0x0a, 0);
      v->write(kSidRegV3Sr, 0x80, 0);
      v->write(kSidRegV3Ctrl, 0x21, 0);  /* sawtooth, gate on */
    }

    for (cycle_t c = 1; c <= 20000; c++) { (void)often.osc3(c); (void)often.env3(c); }

    US_CHECK_EQ_U(once.osc3(20000), often.osc3(20000),
                  "the oscillator is the same whether it is read once or often");
    US_CHECK_EQ_U(once.env3(20000), often.env3(20000),
                  "and so is the envelope");
  }

  /* Noise moves, and it is not the same value twice in a row for ever */
  {
    SidVoice3 v;
    v.write(kSidRegV3FreqLo, 0xff, 0);
    v.write(kSidRegV3FreqHi, 0x0f, 0);
    v.write(kSidRegV3Ctrl, 0x80, 0);     /* noise */

    const data_t first = v.osc3(1000);
    bool moved = false;
    for (cycle_t c = 2000; c <= 60000 && !moved; c += 1000) {
      if (v.osc3(c) != first) moved = true;
    }
    US_CHECK(moved, "the noise register produces different values over time");
  }

  /* The test bit holds the oscillator at zero, which is how tunes reset it */
  {
    SidVoice3 v;
    v.write(kSidRegV3FreqHi, 0x40, 0);
    v.write(kSidRegV3Ctrl, 0x20, 0);
    US_CHECK(v.osc3(100) != 0, "the oscillator is running");
    v.write(kSidRegV3Ctrl, 0x28, 200);   /* sawtooth + test */
    US_CHECK_EQ_U(v.osc3(5000), 0x00u, "and the test bit pins it at zero");
  }

  /* The envelope: attack climbs, release falls back to nothing */
  {
    SidVoice3 v;
    v.write(kSidRegV3Ad, 0x00, 0);       /* fastest attack and decay */
    v.write(kSidRegV3Sr, 0xf0, 0);       /* full sustain, fastest release */
    US_CHECK_EQ_U(v.env3(0), 0x00u, "the envelope starts closed");

    v.write(kSidRegV3Ctrl, 0x11, 0);     /* triangle, gate on */
    /* the fastest attack is 9 cycles a step, so 255 steps is about 2300 */
    US_CHECK(v.env3(1000) > 0, "gating it opens the envelope");
    US_CHECK_EQ_U(v.env3(4000), 0xffu, "a fast attack reaches full scale");

    v.write(kSidRegV3Ctrl, 0x10, 4000);  /* gate off */
    US_CHECK(v.env3(4500) < 0xff, "releasing it starts to close");
    US_CHECK_EQ_U(v.env3(3000000), 0x00u, "and it closes completely");
  }

  /* Sustain is where decay stops */
  {
    SidVoice3 v;
    v.write(kSidRegV3Ad, 0x00, 0);
    v.write(kSidRegV3Sr, 0x80, 0);       /* sustain at 8 * $11 = $88 */
    v.write(kSidRegV3Ctrl, 0x11, 0);
    (void)v.env3(4000);                  /* through the attack */
    const data_t held = v.env3(2000000);
    US_CHECK_EQ_U(held, 0x88u, "decay stops at the sustain level");
  }

  /* And through the chip, at the address a tune uses */
  {
    Bus bus;
    NullSidBackend backend;
    Mos6581_8580 sid(bus, backend);
    sid.config().count = 1;
    sid.config().base[0] = 0xd400;

    sid.io_write(0xd40f, 0x20);          /* frequency high */
    sid.io_write(0xd412, 0x20);          /* sawtooth */
    const data_t before = sid.io_read(0xd41b);
    bus.run(2000);
    const data_t after = sid.io_read(0xd41b);
    US_CHECK(before != after, "$d41b moves as the machine runs");
    US_CHECK_EQ_U(sid.io_read(0xd41f), 0x00u,
                  "an unreadable register still comes back from the mirror");
  }

  return 0;
}

} /* namespace */

int us_test_sid(void)
{
  US_TEST_BEGIN("sid/mos6581_8580");

  test_translation();
  test_cycle_deltas();
  test_wait_and_flush();
  test_machine_integration();
  test_trace_dump();
  test_voice3();

  US_TEST_END("sid/mos6581_8580");
}

US_TEST_MAIN(us_test_sid)
