/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * player.cpp
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

#include "player.h"

#include "boot_image.h"

namespace usbsid {

/**
 * @brief Write back the recorded KERNAL boot state
 *
 * boot_image.h holds 1557 RAM bytes that differ from the power on pattern
 * and the frame phase the boot ends at. Phase is restored by running the machine, RAM is written after
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
 * @brief Run the KERNAL until the BASIC prompt shows in screen memory
 *
 * Costs 2 240 002 cycles (1.7 seconds of silence on the device)
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

/** @brief Set the register state the psid driver is entered with */
void Player::setup_for_driver(bool is_pal)
{
  Mmu & mmu = machine_.mmu();

  /* CIA1 timer A values per video standard from the SID file format */
  const data_t timer_lo = is_pal ? 0x25 : 0x95;
  const data_t timer_hi = is_pal ? 0x40 : 0x42;

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

  /* Cursor blink off, the KERNAL IRQ would corrupt a driver relocated into the screen page */
  mmu.write(0x00cc, 0x01);
}

bool Player::load_sid(const data_t * bytes, size_t len, uint16_t song)
{
  loaded_ = false;
  playing_ = false;
  is_prg_ = false;
  frames_ = 0;

  if (!sidfile_parse(bytes, len, tune_)) return false;

  /* Use the video standard the tune asks for */
  if (tune_.video_known) machine_.set_video_model(tune_.video_model);

  /* Wire up the tune's SID addresses (up to kMaxSids). Boards only have 4 sockets and drop the rest */
  SidConfig & sid = machine_.sid().config();
  sid.count = (tune_.sid_count > kMaxSids) ? kMaxSids : tune_.sid_count;
  for (uint8_t i = 0; i < kMaxSids; i++) {
    sid.base[i] = (i < sid.count) ? tune_.sid_addr[i] : 0;
  }

  song_ = (song == 0) ? tune_.start_song : song;
  if (song_ < 1 || song_ > tune_.songs) song_ = tune_.start_song;

  /* RSID BASIC program is started with RUN, no driver */
  if (tune_.is_basic) {
    prg_ = PrgFile{};
    prg_.valid = true;
    prg_.load_addr = tune_.load_addr;
    prg_.end_addr = tune_.load_last_addr;
    prg_.data = tune_.data;
    prg_.data_size = tune_.data_size;
    /* RUN starts it either way */
    prg_.has_sys_stub = false;
    is_prg_ = true;
    loaded_ = true;
    return true;
  }

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

/** @brief Tell BASIC where the program ends, $ae/$af plus the variable pointers */
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

/**
 * @brief Start a program on the stub KERNAL without BASIC, return false if BASIC is needed
 *
 * RTS from the program lands in the stub idle loop
 */
bool Player::init_prg_without_roms(void)
{
  if (needs_roms()) return false;

  const addr_t entry = prg_.has_sys_stub ? prg_.sys_addr : prg_.load_addr;
  const bool is_pal = (machine_.video_model() == VideoModel::Pal6569 ||
                       machine_.video_model() == VideoModel::PalN6572);

  machine_.power_on();
  machine_.sid().reset();
  apply_boot_image();

  Ram & ram = machine_.ram();
  for (size_t i = 0; i < prg_.data_size; i++) {
    ram.dma_write(static_cast<addr_t>(prg_.load_addr + i), prg_.data[i]);
  }
  set_basic_pointers(prg_.end_addr);
  machine_.keyboard().reset();

  /* CIA1 timer A at the KERNAL rate */
  setup_for_driver(is_pal);

  while (!machine_.cpu().instruction_done()) machine_.tick();
  Mos6510 & cpu = machine_.cpu();
  cpu.hot_reset();
  /* SP as BASIC's SYS leaves it ($f6): decrunchers count with TSX */
  const addr_t ret = static_cast<addr_t>(kStubIdleLoop - 1);
  ram.dma_write(0x01f8, static_cast<data_t>(ret >> 8));
  ram.dma_write(0x01f7, static_cast<data_t>(ret & 0xff));
  cpu.sp(0xf6);
  cpu.pc(entry);

  machine_.sid().resync();

  frames_ = 0;
  playing_ = true;
  paused_ = false;
  return true;
}

bool Player::init_prg(void)
{
  if (!loaded_ || !is_prg_ || !prg_.valid) return false;
  if (!machine_.mmu().roms.complete()) return init_prg_without_roms();

  machine_.power_on();
  machine_.sid().reset();

  /* Boot to the BASIC prompt first */
  boot_kernal();

  Ram & ram = machine_.ram();
  for (size_t i = 0; i < prg_.data_size; i++) {
    ram.dma_write(static_cast<addr_t>(prg_.load_addr + i), prg_.data[i]);
  }
  set_basic_pointers(prg_.end_addr);

  Keyboard & keys = machine_.keyboard();
  keys.reset();

  if (prg_.is_basic()) {
    /* Loaded at the BASIC area, RUN starts it */
    keys.type("run\n");
  } else {
    /* Use the SYS address if there is one, else the load address */
    const addr_t entry = prg_.has_sys_stub ? prg_.sys_addr : prg_.load_addr;
    char line[16];
    int n = 0;
    line[n++] = 's'; line[n++] = 'y'; line[n++] = 's';
    /* Convert the address without printf, this runs on the device too */
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

  /* Feed the keys through the CIA1 matrix for the KERNAL scan, frame limit stops a stuck boot */
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

  /* Fresh machine every time */
  machine_.power_on();
  machine_.sid().reset();

  /* Apply the boot image (tunes read vectors, zero page and RAMTAS values) */
  apply_boot_image();

  if (!psiddrv_install(machine_.ram(), tune_, song_, is_pal, reloc_addr_)) {
    return false;
  }

  /* Put the machine in the state the driver expects (raster IRQ off, CIA1 timer A at the tune frame rate, CIA2 silent, bank config) */
  setup_for_driver(is_pal);

  /* Enter the driver through a reset, the KERNAL finds the cartridge signature at $8000 and jumps to the driver.
   * Interrupts must be disabled on entry (Krakout, Miami Vice and Bubble Bobble crash otherwise) */
  while (!machine_.cpu().instruction_done()) machine_.tick();
  machine_.cpu().reset();

  /* Time starts at this point */
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
  /* A program is paused with RUN/STOP, a tune by holding the emulation */
  if (is_prg_) {
    if (paused) run_stop();
    return;
  }
  paused_ = paused;
}

/**
 * @brief Switch subtune through the driver "load another song" entry
 *
 * Fails on some tunes
 */
void Player::select_subtune(uint16_t song)
{
  if (is_prg_ || !tune_.valid || !playing_) return;

  song_ = song;

  Ram & ram = machine_.ram();
  ram.dma_write(static_cast<addr_t>(reloc_addr_ + kPsidDrvParamOffset),
                static_cast<data_t>(song_));
  /* BASIC tunes read the song number from the A/X/Y store */
  ram.dma_write(780, static_cast<data_t>(song_ - 1));
  ram.dma_write(781, static_cast<data_t>(song_ - 1));
  ram.dma_write(782, static_cast<data_t>(song_ - 1));

  while (!machine_.cpu().instruction_done()) machine_.tick();
  machine_.cpu().pc(
    static_cast<addr_t>(reloc_addr_ + kPsidDrvNextSongOffset));
  /* hot_reset clears stack and interrupt disable inherited from the old song */
  machine_.cpu().hot_reset();
}

void Player::next_subtune(void)
{
  /* Programs move on with the plus key */
  if (is_prg_) { tap_key(kKeyPlus); return; }
  if (!tune_.valid) return;
  /* Single song tune, a driver jump would restart init inside the interrupt */
  if (tune_.songs <= 1) return;
  uint16_t next = static_cast<uint16_t>(song_ + 1);
  if (next > tune_.songs) next = 1;
  /* Re-initialise, select_subtune() jumps break some tunes */
  init_tune(next);
}

bool Player::restart_song(uint16_t song)
{
  if (is_prg_ || !tune_.valid) return false;
  /* Absolute song numbers are not wrapped */
  if (song < 1 || song > tune_.songs) return false;
  return init_tune(song);
}

void Player::previous_subtune(void)
{
  if (is_prg_) { tap_key(kKeyMinus); return; }
  if (!tune_.valid) return;
  if (tune_.songs <= 1) return;
  uint16_t prev = (song_ <= 1) ? tune_.songs : static_cast<uint16_t>(song_ - 1);
  init_tune(prev);
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

  /* Queue moves on once a frame, before the KERNAL scan */
  machine_.keyboard().tick_frame();

  /* Run exactly the cycles left in the frame */
  machine_.run(machine_.vic().cycles_to_frame_end());
  ++frames_;
}

void Player::run_frames(uint32_t frames)
{
  for (uint32_t i = 0; i < frames; i++) run_frame();
}

} /* namespace usbsid */
