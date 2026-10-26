/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * test_asid.cpp
 * ASID MIDI backend tests, SysEx encoding checked against the firmware decoder
 * (USBSID-Pico firmare: asid.c)
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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "test_common.h"
#include "tests.h"

#if defined(US_TEST_HAVE_ASID)

/* Tests read and seed backend state directly to put a capture sink behind RtMidiOut */
#define private public
#include "sid_asid.h"
#undef private

using namespace usbsid;

namespace {

typedef std::vector<unsigned char> Bytes;

/* Messages sent by the backend, in order */
std::vector<Bytes> g_sent;
/* Fake MIDI ports ~ count and the one the backend opened */
unsigned g_ports = 0;
int g_opened = -1;
/* RtMidiOut constructions and destructions, leak check */
int g_midi_new = 0;
int g_midi_del = 0;

} /* namespace */

/* Test supplies the RtMidi symbols the backend uses, with a MIDI API that records messages */
class CaptureApi : public MidiOutApi
{
 public:
  CaptureApi(void) { connected_ = false; }
  RtMidi::Api getCurrentApi(void) { return RtMidi::RTMIDI_DUMMY; }
  void openPort(unsigned int port, const std::string &)
  {
    g_opened = (int)port;
    connected_ = true;
  }
  void openVirtualPort(const std::string &) {}
  void closePort(void) { connected_ = false; }
  void setClientName(const std::string &) {}
  void setPortName(const std::string &) {}
  unsigned int getPortCount(void) { return g_ports; }
  std::string getPortName(unsigned int port) { return "capture" + std::to_string(port); }
  void sendMessage(const unsigned char * message, size_t size)
  {
    g_sent.push_back(Bytes(message, message + size));
  }
 protected:
  void initialize(const std::string &) {}
};

RtMidi::RtMidi() : rtapi_(0) {}
RtMidi::~RtMidi() {}
MidiApi::MidiApi() : apiData_(0), connected_(false), errorCallback_(0),
                     firstErrorOccurred_(false), errorCallbackUserData_(0) {}
MidiApi::~MidiApi() {}
void MidiApi::setErrorCallback(RtMidiErrorCallback cb, void * ud)
{
  errorCallback_ = cb;
  errorCallbackUserData_ = ud;
}
MidiOutApi::MidiOutApi() {}
MidiOutApi::~MidiOutApi() {}
RtMidiOut::RtMidiOut(RtMidi::Api, const std::string &) { rtapi_ = new CaptureApi(); g_midi_new++; }
RtMidiOut::~RtMidiOut() throw() { delete rtapi_; g_midi_del++; }

namespace {

/* Backend with its MIDI port open on the capture sink */
struct Rig {
  ASIDBackend be;
  explicit Rig(int no_sids = 1)
  {
    g_sent.clear();
    g_ports = 1;
    be.nosids = no_sids;
    be.midiout = new RtMidiOut();
    be.midiout->openPort(0);
    be.port_is_open = true;
  }
  /* Clear the sent messages */
  void clear(void) { g_sent.clear(); }
};

/* Register order of an ASID message, from asid_sid_registers in repo/src/asid.h ~ bits 22-27 = control and shadow slots */
const uint8_t kFwRegs[28] = {
  0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d,
  0x0e, 0x0f, 0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x04, 0x0b,
  0x12, 0x04, 0x0b, 0x12,
};

/* Build the expected SID message from (ASID bit, value) pairs sorted by bit. */
Bytes sid_msg(unsigned char addr, const std::vector<std::pair<int, int> > & w)
{
  unsigned mask = 0, msb = 0;
  Bytes data;
  for (size_t i = 0; i < w.size(); i++) {
    mask |= 1u << w[i].first;
    if (w[i].second > 0x7f) msb |= 1u << w[i].first;
    data.push_back((unsigned char)(w[i].second & 0x7f));
  }
  Bytes m = { 0xf0, 0x2d, addr };
  for (int i = 0; i < 4; i++) m.push_back((unsigned char)((mask >> (7 * i)) & 0x7f));
  for (int i = 0; i < 4; i++) m.push_back((unsigned char)((msb >> (7 * i)) & 0x7f));
  m.insert(m.end(), data.begin(), data.end());
  m.push_back(0xf7);
  return m;
}

/* Firmware decode of a SID message (handle_writeordered_asid_message) into (register, value) writes ~ payload starts after F0 2D xx */
std::vector<std::pair<int, int> > fw_decode_sid(const Bytes & m)
{
  std::vector<std::pair<int, int> > out;
  const unsigned char * b = &m[3];
  unsigned reg = 0;
  for (int mask = 0; mask < 4; mask++) {
    for (int bit = 0; bit < 7; bit++) {
      if (b[mask] & (1 << bit)) {
        int v = b[reg + 8];
        if (b[mask + 4] & (1 << bit)) v |= 0x80;
        out.push_back(std::make_pair((int)kFwRegs[mask * 7 + bit], v));
        reg++;
      }
    }
  }
  return out;
}

/* Firmware decode of an FM message (handle_asid_fmoplmessage) into a flat index/data byte list ~ returns false when dropped */
bool fw_decode_fm(const Bytes & m, Bytes & regs)
{
  const unsigned char * buf = &m[3];
  const int len = (int)m.size() - 4; /* payload without F0 2D 60 and F7 */
  if (len < 2) return false;
  int npairs = buf[0] + 1;
  if (npairs > 16) return false;
  int ndata = npairs << 1;
  int nmask = (ndata - 1) / 7 + 1;
  if (len < 1 + nmask + ndata) return false;
  int di = nmask + 1;
  regs.clear();
  for (int mask = 0; mask < nmask; mask++) {
    unsigned char field = 0x01;
    for (int bit = 0; bit < 7 && (int)regs.size() < ndata; bit++) {
      int d = buf[di++];
      if ((buf[1 + mask] & field) == field) d += 0x80;
      regs.push_back((unsigned char)d);
      field <<= 1;
    }
  }
  return true;
}

/* Parked FM/OPL ports as addressed by the player core: kMaxSids * 0x20 */
const addr_t kFmIndex = kMaxSids * 0x20;
const addr_t kFmData  = kMaxSids * 0x20 + 0x10;

void fm_write(ASIDBackend & be, int index, int value)
{
  be.write(kFmIndex, (data_t)index, 0);
  be.write(kFmData, (data_t)value, 0);
}

/* ---- tests ---- */

int test_closed_port(void)
{
  g_sent.clear();
  {
    ASIDBackend be;
    be.write(0x18, 0x0f, 0);
    fm_write(be, 0x20, 0x01);
    US_CHECK_EQ_U(be.asid_flush(), 0, "flush without port returns 0");
    be.flush();
    be.asid_close(); /* no-op, no stop message */
  }
  US_CHECK_EQ_U(g_sent.size(), 0, "closed port sends nothing");
  return 0;
}

int test_init_port_unavailable(void)
{
  g_sent.clear();
  g_ports = 0;
  ASIDBackend be;
  US_CHECK(!be.asid_init(0, 1), "init fails on port 0 with no ports");
  US_CHECK(!be.port_is_open, "port stays closed after failed init");
  be.write(0x00, 0x11, 0);
  be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "nothing sent after failed init");
  g_ports = 2;
  US_CHECK(!be.asid_init(2, 3), "init fails on port == port count");
  US_CHECK_EQ_U(be.nosids, 3, "nosids stored even when init fails");
  US_CHECK_EQ_U(g_sent.size(), 0, "no start message on failed init");
  return 0;
}

int test_failed_init_no_leak(void)
{
  g_sent.clear();
  g_ports = 0;
  g_midi_new = 0;
  g_midi_del = 0;
  {
    ASIDBackend be;
    US_CHECK(!be.asid_init(0, 1), "init fails with no ports");
    US_CHECK(be.midiout == nullptr, "midiout null after failed init");
    US_CHECK_EQ_U(g_midi_new, 1, "one RtMidiOut built");
    US_CHECK_EQ_U(g_midi_del, 1, "failed init freed it");
    US_CHECK(!be.asid_init(1, 1), "second failed init");
    US_CHECK_EQ_U(g_midi_new, g_midi_del, "no leak across repeated failed init");
    be.asid_close();
    US_CHECK_EQ_U(g_sent.size(), 0, "close after failed init sends nothing");
  }
  US_CHECK_EQ_U(g_midi_new, g_midi_del, "destructor after failed init leaks nothing");
  US_CHECK_EQ_U(g_sent.size(), 0, "no stop message for a port never opened");
  /* Handle that never opened is freed without a stop message */
  g_midi_new = 0;
  g_midi_del = 0;
  {
    ASIDBackend be;
    be.midiout = new RtMidiOut();
  }
  US_CHECK_EQ_U(g_midi_del, 1, "destructor frees an unopened handle");
  US_CHECK_EQ_U(g_sent.size(), 0, "no stop message for an unopened handle");
  return 0;
}

int test_env_type_without_port(void)
{
  g_sent.clear();
  g_ports = 0;
  {
    ASIDBackend be;
    be.sendSIDEnvironment(true);
    be.sendSIDType(true);
    US_CHECK_EQ_U(g_sent.size(), 0, "env and type before init send nothing");
    US_CHECK(!be.asid_init(0, 1), "init fails");
    be.sendSIDEnvironment(false);
    be.sendSIDType(false);
    US_CHECK_EQ_U(g_sent.size(), 0, "env and type after failed init send nothing");
  }
  /* Handle present, port not open. */
  {
    ASIDBackend be;
    be.midiout = new RtMidiOut();
    be.sendSIDEnvironment(true);
    be.sendSIDType(true);
    US_CHECK_EQ_U(g_sent.size(), 0, "env and type with unopened port send nothing");
  }
  return 0;
}

int test_init_start_message(void)
{
  g_sent.clear();
  g_ports = 3;
  g_opened = -1;
  {
    ASIDBackend be;
    US_CHECK(be.asid_init(1, 2), "init succeeds on port 1 of 3");
    US_CHECK_EQ_U(g_opened, 1, "requested port opened");
    US_CHECK(be.port_is_open, "port open");
    US_CHECK_EQ_U(be.nosids, 2, "nosids stored");
    US_CHECK_EQ_U(g_sent.size(), 1, "start message sent");
    if (g_sent.size() == 1) {
      US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x4c, 0xf7 }), "play start F0 2D 4C F7");
    }
    be.write(0x18, 0x0f, 0);
    be.flush();
    US_CHECK_EQ_U(g_sent.size(), 2, "writes flow after init");
  }
  /* destructor closes an open port with a stop message */
  US_CHECK_EQ_U(g_sent.size(), 3, "destructor sends stop");
  if (g_sent.size() == 3) {
    US_CHECK(g_sent[2] == (Bytes{ 0xf0, 0x2d, 0x4d, 0xf7 }), "play stop F0 2D 4D F7");
  }
  return 0;
}

int test_close_stop_message(void)
{
  Rig r;
  r.be.write(0x00, 0x11, 0);
  fm_write(r.be, 0x20, 0x01);
  r.be.asid_close();
  US_CHECK_EQ_U(g_sent.size(), 1, "close sends exactly one message");
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x4d, 0xf7 }), "play stop F0 2D 4D F7");
  }
  US_CHECK(!r.be.port_is_open, "port closed");
  US_CHECK(r.be.midiout == nullptr, "midiout released");
  r.clear();
  r.be.write(0x00, 0x22, 0);
  r.be.flush();
  r.be.asid_close();
  US_CHECK_EQ_U(g_sent.size(), 0, "no traffic after close, second close no-op");
  return 0;
}

int test_environment_message(void)
{
  {
    Rig r(1);
    r.be.sendSIDEnvironment(true);
    US_CHECK_EQ_U(g_sent.size(), 1, "PAL env sent");
    if (g_sent.size() == 1) {
      /* settings 0x00 = PAL, speed 1x. framedelta 19950 us = 0x6e, 0x1b, 0x01 in 7+7+2 bit groups */
      US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x31, 0x00, 0x6e, 0x1b, 0x01, 0xf7 }),
               "PAL environment bytes");
      Bytes & m = g_sent[0];
      unsigned fd = (m[4] & 0x7f) | ((m[5] & 0x7f) << 7) | ((m[6] & 0x03) << 14);
      US_CHECK_EQ_U(fd, 19950, "firmware framedelta decode PAL");
    }
  }
  {
    Rig r(1);
    r.be.sendSIDEnvironment(false);
    if (g_sent.size() == 1) {
      /* NTSC sets bit0, speed field 0. framedelta 16715 us = 0x4b, 0x02, 0x01 */
      US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x31, 0x01, 0x4b, 0x02, 0x01, 0xf7 }),
               "NTSC environment bytes");
    } else {
      US_CHECK(false, "NTSC env sent");
    }
  }
  {
    Rig r(3);
    r.be.sendSIDEnvironment(true);
    if (g_sent.size() == 1) {
      US_CHECK_EQ_U(g_sent[0][3], 0x00, "speed field ignores the SID count (1x)");
    }
  }
  {
    Rig r(15);
    r.be.sendSIDEnvironment(false);
    if (g_sent.size() == 1) {
      US_CHECK_EQ_U(g_sent[0][3], 0x01, "speed field ignores the SID count, NTSC bit set");
    }
  }
  /* speed field = multiplier - 1 in bits 4-1, clamped to 1..16, PAL/NTSC in bit 0 */
  {
    const int mults[]   = { 1, 2, 4, 16, 0, -3, 17, 99 };
    const int fields[]  = { 0, 1, 3, 15, 0, 0, 15, 15 };
    for (size_t i = 0; i < sizeof(mults) / sizeof(mults[0]); i++) {
      for (int pal = 0; pal < 2; pal++) {
        Rig r(1);
        r.be.sendSIDEnvironment(pal != 0, mults[i]);
        if (g_sent.size() == 1) {
          const unsigned want = static_cast<unsigned>((fields[i] << 1) | (pal ? 0 : 1));
          US_CHECK_EQ_U(g_sent[0][3], want, "speed field and standard bit");
          US_CHECK_EQ_U(g_sent[0].size(), 8, "environment length unchanged");
        } else {
          US_CHECK(false, "env with multiplier sent");
        }
      }
    }
  }
  return 0;
}

int test_speed_multiplier(void)
{
  const uint32_t pal = 19656, ntsc = 17095;
  US_CHECK_EQ_U(asid_speed_multiplier(false, 0x0010, pal), 1, "raster tune is 1x whatever the latch");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 0x4025, pal), 1, "CIA at PAL 50 Hz default is 1x");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 0x4294, ntsc), 1, "CIA at NTSC 60 Hz default is 1x");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 9827, pal), 2, "CIA latch for 2x PAL");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 4913, pal), 4, "CIA latch for 4x PAL");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 4273, ntsc), 4, "CIA latch for 4x NTSC");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 1228, pal), 16, "CIA latch for 16x PAL");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 100, pal), 16, "faster than 16x clamps to 16");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 0, pal), 16, "latch 0 clamps to 16");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 0xffff, pal), 1, "slow timer clamps to 1");
  /* 19656 / 6553 = 3.0 exactly. 19656 / 7000 = 2.8 rounds up. 19656 / 8000 = 2.45 rounds down */
  US_CHECK_EQ_U(asid_speed_multiplier(true, 6552, pal), 3, "exact ratio 3");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 6999, pal), 3, "2.8 rounds up to 3");
  US_CHECK_EQ_U(asid_speed_multiplier(true, 7999, pal), 2, "2.45 rounds down to 2");
  return 0;
}

int test_type_message(void)
{
  Rig r;
  r.be.sendSIDType(true);
  r.be.sendSIDType(false);
  US_CHECK_EQ_U(g_sent.size(), 2, "two type messages");
  if (g_sent.size() == 2) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x32, 0x00, 0x00, 0xf7 }), "6581 type bytes");
    US_CHECK(g_sent[1] == (Bytes{ 0xf0, 0x2d, 0x32, 0x00, 0x01, 0xf7 }), "8580 type bytes");
  }
  return 0;
}

int test_single_register_bytes(void)
{
  Rig r;
  r.be.write(0x18, 0x0f, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "one message");
  if (g_sent.size() == 1) {
    /* register 0x18 is ASID bit 21: mask byte 3 bit 0 */
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x4e, 0x00, 0x00, 0x00, 0x01,
                                  0x00, 0x00, 0x00, 0x00, 0x0f, 0xf7 }),
             "volume write bytes");
  }
  return 0;
}

int test_msb_packing(void)
{
  Rig r;
  r.be.write(0x00, 0xff, 0);
  r.be.write(0x01, 0x80, 0);
  r.be.write(0x02, 0x7f, 0);
  r.be.write(0x03, 0x00, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "one message");
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x4e, 0x0f, 0x00, 0x00, 0x00,
                                  0x03, 0x00, 0x00, 0x00,
                                  0x7f, 0x00, 0x7f, 0x00, 0xf7 }),
             "bit 7 moves to the msb mask, data stays 7 bit");
    std::vector<std::pair<int, int> > d = fw_decode_sid(g_sent[0]);
    US_CHECK_EQ_U(d.size(), 4, "firmware sees 4 writes");
    if (d.size() == 4) {
      US_CHECK(d[0] == std::make_pair(0, 0xff), "reg 0 = 0xff");
      US_CHECK(d[1] == std::make_pair(1, 0x80), "reg 1 = 0x80");
      US_CHECK(d[2] == std::make_pair(2, 0x7f), "reg 2 = 0x7f");
      US_CHECK(d[3] == std::make_pair(3, 0x00), "reg 3 = 0x00");
    }
  }
  return 0;
}

int test_register_order_vs_firmware(void)
{
  /* Every register 0x00-0x18 alone must decode to the same register and value */
  for (int reg = 0; reg <= 0x18; reg++) {
    Rig r;
    const int val = 0x80 | (reg + 1);
    r.be.write((addr_t)reg, (data_t)val, 0);
    r.be.flush();
    char what[48];
    snprintf(what, sizeof(what), "reg 0x%02x one message", reg);
    US_CHECK_EQ_U(g_sent.size(), 1, what);
    if (g_sent.size() != 1) continue;
    std::vector<std::pair<int, int> > d = fw_decode_sid(g_sent[0]);
    snprintf(what, sizeof(what), "reg 0x%02x decodes to 1 write", reg);
    US_CHECK_EQ_U(d.size(), 1, what);
    if (d.size() == 1) {
      snprintf(what, sizeof(what), "reg 0x%02x register", reg);
      US_CHECK_EQ_U(d[0].first, reg, what);
      snprintf(what, sizeof(what), "reg 0x%02x value", reg);
      US_CHECK_EQ_U(d[0].second, val, what);
    }
  }
  /* All 25 at once: ordered by ASID bit, mask uses 25 bits */
  Rig r;
  for (int reg = 0; reg <= 0x18; reg++) r.be.write((addr_t)reg, (data_t)(reg * 5), 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "full frame is one message");
  if (g_sent.size() == 1) {
    US_CHECK_EQ_U(g_sent[0].size(), 3 + 8 + 25 + 1, "full frame length 37");
    std::vector<std::pair<int, int> > d = fw_decode_sid(g_sent[0]);
    US_CHECK_EQ_U(d.size(), 25, "firmware sees 25 writes");
    bool ok = d.size() == 25;
    std::map<int, int> seen;
    for (size_t i = 0; i < d.size(); i++) seen[d[i].first] = d[i].second;
    for (int reg = 0; reg <= 0x18 && ok; reg++) ok = ok && seen[reg] == reg * 5;
    US_CHECK(ok, "every register value survives the round trip");
  }
  return 0;
}

int test_chip_addresses(void)
{
  Rig r;
  const unsigned char want[4] = { 0x4e, 0x50, 0x51, 0x52 };
  for (int chip = 0; chip < 4; chip++) r.be.write((addr_t)(chip * 0x20 + 0x18), (data_t)(chip + 1), 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 4, "one message per chip");
  for (size_t i = 0; i < g_sent.size() && i < 4; i++) {
    US_CHECK_EQ_U(g_sent[i][2], want[i], "chip address byte in chip order");
    US_CHECK(g_sent[i] == sid_msg(want[i], { { 21, (int)i + 1 } }), "chip message bytes");
  }
  r.clear();
  /* chips 5 and up are dropped */
  r.be.write(0x80 + 0x18, 0x0f, 0);
  r.be.write(14 * 0x20 + 0x18, 0x0f, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "chips 5-15 dropped");
  /* asid_dump bounds: chip number is 1 based */
  r.be.asid_dump(0x18, 1, 0);
  r.be.asid_dump(0x18, 1, 5);
  r.be.asid_dump(0x18, 1, -1);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "asid_dump sidno 0, 5 and -1 ignored");
  return 0;
}

int test_flush_clears_and_dedups(void)
{
  Rig r;
  r.be.write(0x00, 0x10, 0);
  r.be.write(0x00, 0x20, 0);
  r.be.write(0x00, 0x30, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "same register thrice is one message");
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == sid_msg(0x4e, { { 0, 0x30 } }), "last value of the frame wins");
  }
  r.clear();
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "second flush with nothing pending sends nothing");
  r.be.write(0x01, 0x40, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "the following frame sends again");
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == sid_msg(0x4e, { { 1, 0x40 } }), "only the new register, old one not resent");
  }
  r.clear();
  /* registers above 0x18 are not SID registers */
  for (int reg = 0x19; reg <= 0x1f; reg++) r.be.write((addr_t)reg, 0x55, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "registers 0x19-0x1f ignored");
  return 0;
}

int test_control_register_shadow(void)
{
  const int regs[3] = { 0x04, 0x0b, 0x12 };
  const int ctrl_bit[3] = { 22, 23, 24 };
  const int shadow_bit[3] = { 25, 26, 27 };
  for (int v = 0; v < 3; v++) {
    Rig r;
    r.be.write((addr_t)regs[v], 0x41, 0);
    r.be.write((addr_t)regs[v], 0x40, 0);
    r.be.flush();
    char what[48];
    snprintf(what, sizeof(what), "ctrl 0x%02x one message", regs[v]);
    US_CHECK_EQ_U(g_sent.size(), 1, what);
    if (g_sent.size() != 1) continue;
    snprintf(what, sizeof(what), "ctrl 0x%02x primary then shadow", regs[v]);
    US_CHECK(g_sent[0] == sid_msg(0x4e, { { ctrl_bit[v], 0x41 }, { shadow_bit[v], 0x40 } }), "%s", what);
    std::vector<std::pair<int, int> > d = fw_decode_sid(g_sent[0]);
    snprintf(what, sizeof(what), "ctrl 0x%02x firmware writes in order", regs[v]);
    US_CHECK(d.size() == 2 && d[0] == std::make_pair(regs[v], 0x41) &&
             d[1] == std::make_pair(regs[v], 0x40), "%s", what);
  }
  /* third write cuts the frame */
  Rig r;
  r.be.write(0x04, 0x11, 0);
  r.be.write(0x04, 0x10, 0);
  US_CHECK_EQ_U(g_sent.size(), 0, "two writes stay pending");
  r.be.write(0x04, 0x41, 0);
  US_CHECK_EQ_U(g_sent.size(), 1, "third write flushes the first two");
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == sid_msg(0x4e, { { 22, 0x11 }, { 25, 0x10 } }), "cut frame bytes");
  }
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 2, "third value goes in the following frame");
  if (g_sent.size() == 2) {
    US_CHECK(g_sent[1] == sid_msg(0x4e, { { 22, 0x41 } }), "following frame bytes");
  }
  return 0;
}

int test_filter_volume_double_write(void)
{
  const int regs[3] = { 0x16, 0x17, 0x18 };
  const int bits[3] = { 19, 20, 21 };
  for (int v = 0; v < 3; v++) {
    Rig r;
    r.be.write(0x00, 0x33, 0);
    r.be.write((addr_t)regs[v], 0x0a, 0);
    US_CHECK_EQ_U(g_sent.size(), 0, "first write stays pending");
    r.be.write((addr_t)regs[v], 0x0b, 0);
    US_CHECK_EQ_U(g_sent.size(), 1, "second write flushes the first");
    if (g_sent.size() == 1) {
      US_CHECK(g_sent[0] == sid_msg(0x4e, { { 0, 0x33 }, { bits[v], 0x0a } }),
               "flush carries pending registers and the first value");
    }
    r.be.flush();
    US_CHECK_EQ_U(g_sent.size(), 2, "second value in the following frame");
    if (g_sent.size() == 2) {
      US_CHECK(g_sent[1] == sid_msg(0x4e, { { bits[v], 0x0b } }), "following frame bytes");
    }
  }
  return 0;
}

int test_mid_frame_flush_covers_all_chips(void)
{
  Rig r;
  r.be.write(0x20 + 0x00, 0x77, 0);   /* chip 2 pending */
  r.be.write(0x18, 0x01, 0);
  r.be.write(0x18, 0x02, 0);          /* flushes all chips */
  US_CHECK_EQ_U(g_sent.size(), 2, "flush sends both chips");
  if (g_sent.size() == 2) {
    US_CHECK(g_sent[0] == sid_msg(0x4e, { { 21, 0x01 } }), "chip 1 first");
    US_CHECK(g_sent[1] == sid_msg(0x50, { { 0, 0x77 } }), "chip 2 second");
  }
  return 0;
}

int test_reset_drops_pending(void)
{
  Rig r;
  r.be.write(0x00, 0x12, 0);
  r.be.write(0x20 + 0x01, 0x34, 0);
  fm_write(r.be, 0x20, 0x01);
  r.be.reset();
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "reset drops SID and FM state without sending");
  r.be.wait(1000);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "wait is a no-op");
  r.be.write(0x00, 0x56, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "writes work after reset");
  if (g_sent.size() == 1) US_CHECK(g_sent[0] == sid_msg(0x4e, { { 0, 0x56 } }), "bytes after reset");
  return 0;
}

int test_fm_message(void)
{
  Rig r;
  fm_write(r.be, 0x20, 0x05);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "one FM message");
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x60, 0x00, 0x00, 0x20, 0x05, 0xf7 }),
             "single pair bytes");
  }
  r.clear();
  /* msb bits: index 0xa0 and data 0xff are bytes 0 and 1 of the stream */
  fm_write(r.be, 0xa0, 0xff);
  r.be.flush();
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x60, 0x00, 0x03, 0x20, 0x7f, 0xf7 }),
             "pair with both msb set");
  } else {
    US_CHECK(false, "msb pair sent");
  }
  r.clear();
  /* index stays latched: one index write, two data writes */
  r.be.write(kFmIndex, 0x40, 0);
  r.be.write(kFmData, 0x01, 0);
  r.be.write(kFmData, 0x02, 0);
  r.be.flush();
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x60, 0x01, 0x00, 0x40, 0x01, 0x40, 0x02, 0xf7 }),
             "latched index, two pairs");
  } else {
    US_CHECK(false, "latched pairs sent");
  }
  r.clear();
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 0, "FM queue cleared after flush");
  return 0;
}

int test_fm_mask_byte_boundary(void)
{
  /* 4 pairs = 8 bytes = 2 mask bytes ~ data byte 7 (last) bit 7 set lands in mask byte 1 bit 0 */
  Rig r;
  for (int i = 0; i < 3; i++) fm_write(r.be, 0x10 + i, 0x20 + i);
  fm_write(r.be, 0x13, 0xe3);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "one FM message");
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x60, 0x03, 0x00, 0x01,
                                  0x10, 0x20, 0x11, 0x21, 0x12, 0x22, 0x13, 0x63, 0xf7 }),
             "mask byte 1 carries the 8th data bit");
    Bytes regs;
    US_CHECK(fw_decode_fm(g_sent[0], regs), "firmware accepts it");
    US_CHECK(regs == (Bytes{ 0x10, 0x20, 0x11, 0x21, 0x12, 0x22, 0x13, 0xe3 }),
             "firmware recovers the flat index/data list");
  }
  return 0;
}

int test_fm_autoflush_at_16_pairs(void)
{
  Rig r;
  Bytes want;
  for (int i = 0; i < 16; i++) {
    fm_write(r.be, 0x80 + i, 0xc0 + i);
    want.push_back((unsigned char)(0x80 + i));
    want.push_back((unsigned char)(0xc0 + i));
    if (i < 15) {
      char what[48];
      snprintf(what, sizeof(what), "no send before pair 16 (%d)", i);
      US_CHECK_EQ_U(g_sent.size(), 0, what);
    }
  }
  US_CHECK_EQ_U(g_sent.size(), 1, "pair 16 triggers the send");
  if (g_sent.size() == 1) {
    US_CHECK_EQ_U(g_sent[0].size(), 4 + 5 + 32 + 1, "length: header, 5 mask bytes, 32 data, F7");
    US_CHECK_EQ_U(g_sent[0][3], 15, "pair count minus one");
    Bytes regs;
    US_CHECK(fw_decode_fm(g_sent[0], regs), "firmware accepts 16 pairs");
    US_CHECK(regs == want, "firmware recovers all 32 bytes");
  }
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 1, "flush after auto send adds nothing");
  /* 17th pair starts a new message */
  fm_write(r.be, 0x01, 0x02);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 2, "17th pair goes out on flush");
  return 0;
}

int test_fm_before_sid_in_flush(void)
{
  Rig r;
  r.be.write(0x18, 0x0f, 0);
  fm_write(r.be, 0x20, 0x01);
  r.be.write(0x20 + 0x00, 0x22, 0);
  r.be.flush();
  US_CHECK_EQ_U(g_sent.size(), 3, "FM plus two chips");
  if (g_sent.size() == 3) {
    US_CHECK_EQ_U(g_sent[0][2], 0x60, "FM first");
    US_CHECK_EQ_U(g_sent[1][2], 0x4e, "chip 1 second");
    US_CHECK_EQ_U(g_sent[2][2], 0x50, "chip 2 third");
  }
  return 0;
}

int test_fm_port_decode(void)
{
  Rig r;
  /* any parked register that is not the data port latches the index */
  r.be.write(kFmIndex + 5, 0x30, 0);
  r.be.write(kFmData, 0x09, 0);
  /* parked registers of other 0x20 blocks use the same low nibble rule */
  r.be.write(kFmIndex + 0x20 + 0x10, 0x31, 0);
  r.be.flush();
  if (g_sent.size() == 1) {
    US_CHECK(g_sent[0] == (Bytes{ 0xf0, 0x2d, 0x60, 0x01, 0x00, 0x30, 0x09, 0x30, 0x31, 0xf7 }),
             "low nibble 0x10 is data, everything else is index");
  } else {
    US_CHECK_EQ_U(g_sent.size(), 1, "FM message sent");
  }
  return 0;
}

} /* namespace */

int us_test_asid(void)
{
  US_TEST_BEGIN("asid");

  test_closed_port();
  test_init_port_unavailable();
  test_failed_init_no_leak();
  test_env_type_without_port();
  test_init_start_message();
  test_close_stop_message();
  test_environment_message();
  test_speed_multiplier();
  test_type_message();
  test_single_register_bytes();
  test_msb_packing();
  test_register_order_vs_firmware();
  test_chip_addresses();
  test_flush_clears_and_dedups();
  test_control_register_shadow();
  test_filter_volume_double_write();
  test_mid_frame_flush_covers_all_chips();
  test_reset_drops_pending();
  test_fm_message();
  test_fm_mask_byte_boundary();
  test_fm_autoflush_at_16_pairs();
  test_fm_before_sid_in_flush();
  test_fm_port_decode();

  US_TEST_END("asid");
}

#else /* no RtMidi vendored: nothing to test */

int us_test_asid(void)
{
  US_TEST_BEGIN("asid");
  printf("[SKIP] asid: RtMidi not available in this build\n");
  US_TEST_END("asid");
}

#endif

US_TEST_MAIN(us_test_asid)
