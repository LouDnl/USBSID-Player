/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * player.cpp
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

#include "player.h"

#include "boot_image.h"

namespace usbsid {

/**
 * @brief Put the machine in the state the KERNAL boot leaves it in, cheaply.
 *
 * Writes back a recorded answer instead of computing it again. What the boot
 * produces depends only on the ROMs, so `src/player/boot_image.h` holds it:
 * the 1557 RAM bytes that end up differing from the power on pattern, and the
 * point in the frame the boot finishes at.
 *
 * Both halves are needed. RAM matters because tunes read the vectors, the
 * zero page and what RAMTAS leaves under the ROMs, and a machine without them
 * plays something else: of 160 tunes, only 88 play the same notes if this is
 * skipped entirely. The phase matters because a tune entered at a different
 * raster position syncs to a different one; restoring RAM alone gets 93 of
 * 160, restoring both gets 147, and the remaining thirteen are tunes that a
 * single frame of difference would have changed anyway.
 *
 * The phase is restored by running the machine, which lets the KERNAL scribble
 * on its way, so RAM is written afterwards and not before.
 *
 * It is used for every tune and not only after the first, which is the point:
 * a machine set up this way is the same machine every time, and one that boots
 * once and then remembers is not. Two tunes played in one session would
 * otherwise get different machines depending on their order.
 */
void Player::apply_boot_image(void)
{
  if (kBootImagePhase != 0) machine_.run(kBootImagePhase);
  Ram & ram = machine_.ram();
  for (uint16_t i = 0; i < kBootImageCount; i++) {
    ram.dma_write(kBootImage[i].addr, kBootImage[i].value);
  }
}

/**
 * @brief Run the KERNAL far enough that its vectors and IO are set up.
 *
 * Detected by watching for the BASIC prompt in screen memory rather than by
 * counting cycles, so it stays right whatever the video model is.
 *
 * This is expensive: 2 240 002 cycles, which on the device is 1.7 seconds of
 * silence before a tune starts. It is not skippable and it is not cheaply
 * cacheable, and TODO.md item 4 has the measurements behind both halves of
 * that. In short: tunes read the vectors, the zero page and what RAMTAS left
 * under the ROMs, so a machine that has not booted plays something different;
 * and replaying the 1557 bytes of RAM the boot changes is not enough either,
 * because the raster phase it finishes on matters too, and a machine restored
 * that way is still not the machine a boot produces. Doing it properly means
 * saving and restoring the whole machine.
 */
void Player::boot_kernal(void)
{
  static const data_t kReady[] = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2e };
  constexpr uint64_t kLimit = 20ull * 1000ull * 1000ull;

  for (uint64_t cycles = 0; cycles < kLimit; cycles += 5000) {
    machine_.run(5000);
    if (machine_.cpu().jammed()) return;
    for (addr_t base = 0x0400; base < 0x07e2; base++) {
      size_t i = 0;
      while (i < sizeof(kReady) &&
             machine_.ram().dma_read(static_cast<addr_t>(base + i)) == kReady[i]) {
        i++;
      }
      if (i == sizeof(kReady)) return;
    }
  }
}

/**
 * @brief The register state the psid driver is entered with.
 */
void Player::setup_for_driver(bool is_pal)
{
  Mmu & mmu = machine_.mmu();

  /* the CIA1 timer A values the SID file format documents for each standard */
  const data_t timer_lo = is_pal ? 0x25 : 0x42;
  const data_t timer_hi = is_pal ? 0x40 : 0x95;

  mmu.write(0xd011, 0x1b);  /* screen on, raster compare high bit clear */
  mmu.write(0xd012, 0x37);
  mmu.write(0xd01a, 0x00);  /* no raster interrupt */

  mmu.write(0xdc0d, 0x7f);  /* CIA1: all interrupts off */
  mmu.write(0xdc0e, 0x80);  /* timer A stopped */
  mmu.write(0xdc0f, 0x00);  /* timer B stopped */
  mmu.write(0xdc04, timer_lo);
  mmu.write(0xdc05, timer_hi);
  mmu.write(0xdc06, 0xff);
  mmu.write(0xdc07, 0xff);
  mmu.write(0xdc0d, 0x81);  /* timer A interrupt on */
  mmu.write(0xdc0e, 0x81);  /* timer A running */

  mmu.write(0xdd0d, 0x7f);  /* CIA2: all interrupts off */
  mmu.write(0xdd0e, 0x80);
  mmu.write(0xdd0f, 0x00);
  mmu.write(0xdd04, 0xff);
  mmu.write(0xdd05, 0xff);
  mmu.write(0xdd06, 0xff);
  mmu.write(0xdd07, 0xff);

  mmu.write(0x0001, 0x37);  /* BASIC, IO and KERNAL all in */
}

bool Player::load_sid(const data_t * bytes, size_t len, uint16_t song)
{
  loaded_ = false;
  playing_ = false;
  is_prg_ = false;
  frames_ = 0;

  if (!sidfile_parse(bytes, len, tune_)) return false;

  /* A tune that says which video standard it wants gets it. Anything else
   * keeps whatever the machine is set to. */
  if (tune_.video_known) machine_.set_video_model(tune_.video_model);

  /* Tell the SID layer where the tune's chips are */
  SidConfig & sid = machine_.sid().config();
  sid.count = tune_.sid_count;
  for (uint8_t i = 0; i < 4; i++) {
    sid.base[i] = (i < tune_.sid_count) ? tune_.sid_addr[i] : 0;
  }

  song_ = (song == 0) ? tune_.start_song : song;
  if (song_ < 1 || song_ > tune_.songs) song_ = tune_.start_song;

  loaded_ = true;
  return true;
}

bool Player::load_prg(const data_t * bytes, size_t len)
{
  loaded_ = false;
  playing_ = false;
  is_prg_ = false;
  frames_ = 0;

  if (!prgfile_parse(bytes, len, prg_)) return false;

  is_prg_ = true;
  loaded_ = true;
  return true;
}

/**
 * @brief Tell BASIC how far the program reaches.
 *
 * A real LOAD leaves the end of the program in $ae/$af and moves the three
 * pointers that say where variables start. RUN on a program whose pointers
 * still say "empty" clears it before it has a chance.
 */
void Player::set_basic_pointers(addr_t end_addr)
{
  Ram & ram = machine_.ram();
  const addr_t after = static_cast<addr_t>(end_addr + 1);

  const data_t lo = static_cast<data_t>(after & 0xff);
  const data_t hi = static_cast<data_t>(after >> 8);

  ram.dma_write(0x00ae, lo); ram.dma_write(0x00af, hi); /* end of load */
  ram.dma_write(0x002d, lo); ram.dma_write(0x002e, hi); /* variables */
  ram.dma_write(0x002f, lo); ram.dma_write(0x0030, hi); /* arrays */
  ram.dma_write(0x0031, lo); ram.dma_write(0x0032, hi); /* end of arrays */
}

bool Player::init_prg(void)
{
  if (!loaded_ || !is_prg_ || !prg_.valid) return false;

  machine_.power_on();
  machine_.sid().reset();

  /* The program is started from the BASIC prompt, so the machine has to get
   * there first. */
  boot_kernal();

  Ram & ram = machine_.ram();
  for (size_t i = 0; i < prg_.data_size; i++) {
    ram.dma_write(static_cast<addr_t>(prg_.load_addr + i), prg_.data[i]);
  }
  set_basic_pointers(prg_.end_addr);

  Keyboard & keys = machine_.keyboard();
  keys.reset();

  if (prg_.is_basic()) {
    /* Loaded where BASIC programs live: RUN starts it, whether it is BASIC
     * or a machine code program behind a SYS line. */
    keys.type("run\n");
  } else {
    /* Anywhere else there is nothing for RUN to run. The SYS line of a
     * program that has one says where it starts; without one, its load
     * address is the only sensible guess, and it is usually right. */
    const addr_t entry = prg_.has_sys_stub ? prg_.sys_addr : prg_.load_addr;
    char line[16];
    int n = 0;
    line[n++] = 's'; line[n++] = 'y'; line[n++] = 's';
    /* the address, without a printf: this runs on the device too */
    char digits[6];
    int d = 0;
    uint32_t value = entry;
    do { digits[d++] = static_cast<char>('0' + (value % 10)); value /= 10; }
    while (value != 0);
    while (d > 0) line[n++] = digits[--d];
    line[n++] = '\n';
    line[n] = '\0';
    keys.type(line);
  }

  /* Hold the keys down one at a time and let the machine find them, then give
   * BASIC a moment to act on the line. The keys go through CIA1's matrix, so
   * this is the KERNAL's own scan doing the reading: the same path a person
   * pressing RUN would take, and the same one RUN/STOP uses to interrupt what
   * this starts. The limit is only there so a machine that never gets to the
   * prompt still returns. */
  for (unsigned frame = 0; frame < 300; frame++) {
    keys.tick_frame();
    const uint64_t target = machine_.vic().frames() + 1;
    while (machine_.vic().frames() < target) machine_.tick();
    if (!keys.busy() && frame > 8) break;
  }
  for (unsigned frame = 0; frame < 10; frame++) {
    const uint64_t target = machine_.vic().frames() + 1;
    while (machine_.vic().frames() < target) machine_.tick();
  }

  machine_.sid().resync();

  frames_ = 0;
  playing_ = true;
  paused_ = false;
  return true;
}

bool Player::init_tune(uint16_t song)
{
  if (!loaded_ || is_prg_ || !tune_.valid) return false;

  if (song != 0) {
    song_ = (song < 1 || song > tune_.songs) ? tune_.start_song : song;
  }

  const bool is_pal = (machine_.video_model() == VideoModel::Pal6569 ||
                       machine_.video_model() == VideoModel::PalN6572);

  /* A fresh machine every time, so a tune that scribbled over the KERNAL
   * cannot poison the next subtune. */
  machine_.power_on();
  machine_.sid().reset();

  /* The machine has to be in the state the KERNAL leaves it in. Tunes read
   * the vectors, the zero page and the values RAMTAS puts under the ROMs, and
   * a machine that has not booted plays something different: measured, all 160
   * tunes still play without it, but a third of them play something else.
   *
   * It does not have to be reached by running the KERNAL, though, and that is
   * where the 1.7 seconds before every tune went. */
  apply_boot_image();

  if (!psiddrv_install(machine_.ram(), tune_, song_, is_pal, reloc_addr_)) {
    return false;
  }

  /* Put the machine into the state the driver expects before entering it.
   * This is the sequence old player ~ src/vsidpsid.cpp performs before its
   * CPU reset: raster interrupt off, CIA1 stopped and then set to the tune's
   * frame rate with its timer A interrupt armed, CIA2 silent, and the usual
   * bank configuration. Without it the KERNAL's own interrupt keeps running,
   * and the screen editor's cursor blink writes straight through the driver
   * when it is relocated into screen memory. */
  setup_for_driver(is_pal);

  /* Enter the driver the way a real machine would: through a reset.
   *
   * The install put a cartridge signature at $8000 pointing at the driver's
   * entry, so the KERNAL's reset routine finds it before it initialises
   * anything and jumps straight there. That is what the signature is for, and
   * it is how old player enters the driver too.
   *
   * Jumping to the entry point directly instead looks equivalent and is not:
   * a reset arrives with interrupts disabled, and the driver relies on that.
   * It does not raise the I flag itself, it lowers it with a `cli` once the
   * tune's init has returned. Entering with interrupts already enabled lets
   * the CIA timer this player just armed fire in the middle of init, and for
   * a tune living under the KERNAL that means an interrupt vector fetched
   * from the tune's own data. Krakout, Miami Vice and Bubble Bobble all died
   * that way.
   *
   * The reset routine finds the cartridge at $fd02, before it gets as far as
   * IOINIT, so this reaches the driver in a few hundred cycles rather than the
   * couple of million a boot to the BASIC prompt takes. */
  while (!machine_.cpu().instruction_done()) machine_.tick();
  machine_.cpu().reset();

  /* Time starts here. Without this the first register write of the tune would
   * carry the whole KERNAL boot as its delay, a couple of million cycles that
   * the hardware would dutifully wait out in maximum length chunks. */
  machine_.sid().resync();

  frames_ = 0;
  playing_ = true;
  paused_ = false;
  return true;
}

void Player::start(void)
{
  if (playing_) return;
  if (is_prg_) init_prg(); else init_tune(song_);
}

void Player::stop(void)
{
  playing_ = false;
  machine_.sid().backend().flush();
}

void Player::pause(bool paused)
{
  /* A tune is paused by holding the emulation still. A program cannot be:
   * stopping the machine under it would stop the interrupt it is playing
   * from, and there is nothing to resume into. RUN/STOP is what a person
   * would press, so that is what is sent, and it is a stop rather than a
   * pause. The existing player does the same. */
  if (is_prg_) {
    if (paused) run_stop();
    return;
  }
  paused_ = paused;
}

/**
 * @brief Switch subtune the way the existing player does it.
 *
 * The driver has no keyboard handling, so a subtune change is made by writing
 * the new song number into its parameter block and jumping to the driver's
 * "load another song" entry. That keeps the machine and the tune's own data
 * in place, which reinitialising from scratch would throw away.
 *
 * Taken from old player ~ src/vsidpsid.cpp next_prev_tune(). It does not work
 * for every tune, and that is a property of the driver rather than of this
 * code.
 */
void Player::select_subtune(uint16_t song)
{
  if (is_prg_ || !tune_.valid || !playing_) return;

  song_ = song;

  Ram & ram = machine_.ram();
  ram.dma_write(static_cast<addr_t>(reloc_addr_ + kPsidDrvParamOffset),
                static_cast<data_t>(song_));
  /* BASIC tunes read the song number out of the KERNAL's A/X/Y store */
  ram.dma_write(780, static_cast<data_t>(song_ - 1));
  ram.dma_write(781, static_cast<data_t>(song_ - 1));
  ram.dma_write(782, static_cast<data_t>(song_ - 1));

  while (!machine_.cpu().instruction_done()) machine_.tick();
  machine_.cpu().pc(
    static_cast<addr_t>(reloc_addr_ + kPsidDrvNextSongOffset));
}

void Player::next_subtune(void)
{
  /* A program has no subtunes to switch between, but the players that come
   * as programs almost all move on with the plus key, which is what the
   * existing player sends them too. */
  if (is_prg_) { tap_key(kKeyPlus); return; }
  if (!tune_.valid) return;
  /* A single song tune has nowhere to go, and jumping into the driver's
   * "load another song" entry to arrive back at the same song is not a no-op:
   * it restarts the tune's init from inside a running interrupt, and plenty
   * of tunes stop playing when that happens. */
  if (tune_.songs <= 1) return;
  uint16_t next = static_cast<uint16_t>(song_ + 1);
  if (next > tune_.songs) next = 1;
  if (playing_) select_subtune(next); else init_tune(next);
}

void Player::previous_subtune(void)
{
  if (is_prg_) { tap_key(kKeyMinus); return; }
  if (!tune_.valid) return;
  if (tune_.songs <= 1) return;
  uint16_t prev = (song_ <= 1) ? tune_.songs : static_cast<uint16_t>(song_ - 1);
  if (playing_) select_subtune(prev); else init_tune(prev);
}

bool Player::type(const char * text)
{
  return machine_.keyboard().type(text);
}

bool Player::tap_key(KeyPos key)
{
  return machine_.keyboard().tap(key);
}

bool Player::run_stop(void)
{
  return machine_.keyboard().tap(kKeyRunStop);
}

void Player::run_frame(void)
{
  if (!playing_ || paused_) return;

  /* Keys are held across frames, so the queue moves on once a frame, before
   * the frame the KERNAL will scan them in. */
  machine_.keyboard().tick_frame();

  /* Ask the VIC how far the frame has left to run and run exactly that.
   * Polling its frame counter every cycle would work too, and would catch the
   * chip up every cycle, which is the whole point of not clocking it thrown
   * away: it made a tune cost three times what it should. */
  machine_.run(machine_.vic().cycles_to_frame_end());
  ++frames_;
}

void Player::run_frames(uint32_t frames)
{
  for (uint32_t i = 0; i < frames; i++) run_frame();
}

} /* namespace usbsid */
