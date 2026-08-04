/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * test_keyboard.cpp
 * Step 2.12 gate: a key held in the matrix is found by the KERNAL's own scan,
 * and RUN/STOP breaks a running program.
 *
 * Nothing here writes to the keyboard buffer. A key is held down across CIA1's
 * two ports and everything after that is the machine's own doing: the
 * interrupt scan, the debounce, the shift handling, the editor, BASIC. If
 * "print1+1" comes back as 2 on the screen, all of it works.
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

#include "keyboard.h"
#include "machine_harness.h"
#include "player.h"
#include "test_common.h"
#include "tests.h"

using namespace usbsid;
using namespace us_test;

namespace {

/* ---- the matrix ---------------------------------------------------------- */

int test_matrix(void)
{
  Bus bus;
  Mos6526 cia(bus, CiaLine::Irq);
  cia.reset();

  /* port A drives, port B reads, both open collector: everything is high
   * until something pulls it low */
  cia.io_write(0x02, 0xff); /* DDRA: all outputs */
  cia.io_write(0x03, 0x00); /* DDRB: all inputs */
  cia.io_write(0x00, 0xff); /* no row driven */

  US_CHECK_EQ_U(cia.port_b(), 0xffu, "nothing is pressed, nothing reads low");
  US_CHECK(cia.any_key_pressed() == false, "and the CIA agrees");

  /* Q is row 7, column 6 */
  cia.set_key(7, 6, true);
  US_CHECK(cia.any_key_pressed(), "a key is held");
  US_CHECK_EQ_U(cia.port_b(), 0xffu, "but its row is not being driven");

  cia.io_write(0x00, 0x7f); /* drive row 7 low */
  US_CHECK_EQ_U(cia.port_b(), 0xbfu, "driving its row pulls column 6 low");

  cia.io_write(0x00, 0xfe); /* row 0 instead */
  US_CHECK_EQ_U(cia.port_b(), 0xffu, "another row shows nothing");

  /* two keys on the same row */
  cia.set_key(7, 4, true);  /* space */
  cia.io_write(0x00, 0x7f);
  US_CHECK_EQ_U(cia.port_b(), 0xafu, "two keys on a row pull two columns low");

  /* driving every row at once is how a program asks "is anything held" */
  cia.io_write(0x00, 0x00);
  US_CHECK_EQ_U(cia.port_b(), 0xafu, "driving every row finds them too");

  /* the matrix reads the other way round as well: driving a column from port
   * B pulls the rows of the keys held on it low */
  cia.io_write(0x02, 0x00); /* DDRA: inputs */
  cia.io_write(0x03, 0xff); /* DDRB: outputs */
  cia.io_write(0x01, 0xbf); /* drive column 6 low */
  US_CHECK_EQ_U(cia.port_a(), 0x7fu, "the reverse read finds row 7");
  cia.io_write(0x01, 0xff);
  US_CHECK_EQ_U(cia.port_a(), 0xffu, "and nothing when no column is driven");

  cia.clear_keys();
  cia.io_write(0x02, 0xff);
  cia.io_write(0x03, 0x00);
  cia.io_write(0x00, 0x00);
  US_CHECK_EQ_U(cia.port_b(), 0xffu, "letting go clears the matrix");
  US_CHECK(cia.any_key_pressed() == false, "and the fast path with it");

  return 0;
}

/* ---- where the characters are -------------------------------------------- */

int test_char_table(void)
{
  KeyPos k;

  US_CHECK(key_for_char('a', k), "a has a key");
  US_CHECK(k.row == 1 && k.col == 2 && !k.shift, "and it is row 1 column 2");

  US_CHECK(key_for_char('A', k), "upper case is the same key");
  US_CHECK(k.row == 1 && k.col == 2, "in the same place");

  US_CHECK(key_for_char('1', k), "digits are there");
  US_CHECK(k.row == 7 && k.col == 0, "1 is row 7 column 0");

  US_CHECK(key_for_char('\r', k), "return is there");
  US_CHECK(k.row == kKeyReturn.row && k.col == kKeyReturn.col, "in row 0");
  US_CHECK(key_for_char('\n', k), "and a newline is a return");
  US_CHECK(k.row == kKeyReturn.row && k.col == kKeyReturn.col, "same key");

  US_CHECK(key_for_char('+', k), "plus is there");
  US_CHECK(k.row == kKeyPlus.row && k.col == kKeyPlus.col, "where the constants say");

  US_CHECK(key_for_char('"', k), "a quote is there");
  US_CHECK(k.shift, "and it needs shift");

  US_CHECK(key_for_char('~', k) == false, "a key the C64 has not is refused");

  return 0;
}

/* ---- the typing queue ---------------------------------------------------- */

int test_queue(void)
{
  Bus bus;
  Mos6526 cia(bus, CiaLine::Irq);
  cia.reset();
  Keyboard kb(cia);

  US_CHECK(kb.busy() == false, "an idle keyboard is not busy");
  US_CHECK(kb.type("ab"), "two characters queue");
  US_CHECK_EQ_U(kb.queued(), 2u, "and both are waiting");
  US_CHECK(cia.any_key_pressed() == false,
           "queueing does not press anything by itself");

  kb.tick_frame();
  US_CHECK(cia.any_key_pressed(), "the first key goes down on the next frame");

  /* It stays down for two frames, which is longer than the KERNAL's scan, so
   * a keystroke cannot fall between two scans and be missed. */
  kb.tick_frame();
  US_CHECK(cia.any_key_pressed(), "and is still held a frame later");

  kb.tick_frame();
  US_CHECK(cia.any_key_pressed() == false, "then it comes up");
  US_CHECK_EQ_U(kb.queued(), 1u, "with one left to type");

  /* a gap, so the editor sees a new keystroke rather than a held one */
  kb.tick_frame();
  US_CHECK(cia.any_key_pressed() == false, "there is a gap between keys");

  while (kb.busy()) kb.tick_frame();
  US_CHECK_EQ_U(kb.queued(), 0u, "the queue empties");
  US_CHECK(cia.any_key_pressed() == false, "and nothing is left held down");

  /* a shifted character holds shift with it */
  kb.reset();
  kb.type("\"");
  kb.tick_frame();
  cia.io_write(0x02, 0xff);
  cia.io_write(0x00, 0xfd); /* drive row 1, where the left shift is */
  US_CHECK_EQ_U(cia.port_b(), 0x7fu, "a shifted character holds shift too");

  kb.reset();
  US_CHECK(cia.any_key_pressed() == false, "reset lets everything up");

  return 0;
}

/* ---- through the KERNAL -------------------------------------------------- */

void run_frames(TestC64 & c64, unsigned n)
{
  Keyboard & kb = c64.machine.keyboard();
  for (unsigned i = 0; i < n; i++) {
    kb.tick_frame();
    const uint64_t target = c64.machine.vic().frames() + 1;
    while (c64.machine.vic().frames() < target) c64.machine.tick();
  }
}

bool screen_has(TestC64 & c64, const char * text)
{
  char screen[1400];
  c64.screen_text(screen, sizeof(screen));
  return strstr(screen, text) != nullptr;
}

int test_typing_reaches_basic(void)
{
  TestC64 c64;
  ++us_test_checks;
  if (!c64.boot()) {
    ++us_test_failures;
    printf("  FAIL the machine did not boot\n");
    return 1;
  }

  c64.machine.keyboard().type("print1+1\r");
  run_frames(c64, 100);

  US_CHECK(c64.machine.keyboard().busy() == false, "everything was typed");
  /* the editor echoed it, so the scan, the buffer and the editor all work */
  US_CHECK(screen_has(c64, "print1"), "the line was echoed on the screen");
  /* and BASIC ran it */
  US_CHECK(screen_has(c64, " 2"), "and BASIC answered 2");

  if (us_test_failures != 0) {
    char screen[1400];
    c64.screen_text(screen, sizeof(screen));
    printf("%s\n", screen);
  }

  return 0;
}

int test_runstop_breaks_a_program(void)
{
  TestC64 c64;
  ++us_test_checks;
  if (!c64.boot()) {
    ++us_test_failures;
    printf("  FAIL the machine did not boot\n");
    return 1;
  }

  Keyboard & kb = c64.machine.keyboard();

  /* a program with nothing to do but keep going */
  kb.type("10goto10\r");
  run_frames(c64, 90);
  kb.type("run\r");
  run_frames(c64, 50);

  US_CHECK(screen_has(c64, "break") == false, "the program is running");

  kb.tap(kKeyRunStop);
  run_frames(c64, 40);

  US_CHECK(screen_has(c64, "break in 10"),
           "RUN/STOP broke it, and BASIC said where");

  if (us_test_failures != 0) {
    char screen[1400];
    c64.screen_text(screen, sizeof(screen));
    printf("%s\n", screen);
  }

  return 0;
}

/* ---- and through the player ---------------------------------------------- */

int test_player_control(void)
{
  TestC64 c64;
  NullSidBackend backend;
  c64.machine.set_sid_backend(backend);
  if (!c64.boot()) {
    ++us_test_checks; ++us_test_failures;
    printf("  FAIL the machine did not boot\n");
    return 1;
  }

  Player player(c64.machine);

  /* The player's own entry points reach the same matrix. There is no tune
   * loaded, so nothing runs the frames, but the queue is what is being
   * checked here. */
  US_CHECK(player.type("run\r"), "the player can type");
  US_CHECK(player.typing(), "and says it is typing");
  US_CHECK(player.run_stop(), "and can press RUN/STOP");

  c64.machine.keyboard().reset();
  US_CHECK(player.typing() == false, "a reset clears the queue");

  /* A program's next and previous subtune are the plus and minus keys, which
   * is what the players that come as programs listen for. */
  const data_t prg[] = {
    0x01, 0x08, 0x0b, 0x08, 0x0a, 0x00, 0x9e, '2', '0', '6', '1', 0x00,
    0x00, 0x00, 0x60
  };
  US_CHECK(player.load_prg(prg, sizeof(prg)), "a program loads");
  player.next_subtune();
  US_CHECK_EQ_U(c64.machine.keyboard().queued(), 1u,
                "next subtune queues a key for a program");
  c64.machine.keyboard().reset();
  player.previous_subtune();
  US_CHECK_EQ_U(c64.machine.keyboard().queued(), 1u,
                "and so does the previous one");

  return 0;
}

} /* namespace */

int us_test_keyboard(void)
{
  US_TEST_BEGIN("keyboard");

  test_matrix();
  test_char_table();
  test_queue();
  test_typing_reaches_basic();
  test_runstop_breaks_a_program();
  test_player_control();

  US_TEST_END("keyboard");
}

US_TEST_MAIN(us_test_keyboard)
