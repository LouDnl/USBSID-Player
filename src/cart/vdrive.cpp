/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
 * command line playback, websites and for embedding on RP2040/RP2350.
 *
 * vdrive.cpp
 * Read-only virtual disk drive on device 8 at KERNAL level, files from a table
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
 */

#include "vdrive.h"

#include "mem/ram.h"

namespace usbsid {

namespace {

constexpr addr_t kStubBase = 0xde80;
constexpr addr_t kRegBase  = 0xdef0;

/* Register offsets from $def0 */
enum : uint8_t { R_A = 0, R_X, R_Y, R_P, R_CMD, R_PASS, R_JMPLO, R_JMPHI, R_PIN };

/* KERNAL zero page and vectors */
constexpr addr_t kStatus  = 0x90;
constexpr addr_t kVerify  = 0x93;
constexpr addr_t kInDev   = 0x99;
constexpr addr_t kOutDev  = 0x9a;
constexpr addr_t kEndLo   = 0xae;
constexpr addr_t kNameLen = 0xb7;
constexpr addr_t kLa      = 0xb8;
constexpr addr_t kSa      = 0xb9;
constexpr addr_t kDevice  = 0xba;
constexpr addr_t kNamePtr = 0xbb;
constexpr addr_t kLoadPtr = 0xc3;
constexpr uint8_t kDrive  = 8;

/* Indexed by VirtualDrive::Fn: OPEN CLOSE CHKIN CHKOUT CLRCHN CHRIN CHROUT GETIN CLALL LOAD SAVE */
constexpr addr_t kVectors[] = {
  0x031a, 0x031c, 0x031e, 0x0320, 0x0322, 0x0324, 0x0326, 0x032a, 0x032c, 0x0330, 0x0332,
};

/* Entry n at $de80+3n is JSR common, common reports the return address low byte as the function.
 * Handled: A/X/Y and flags come back from the registers. Passed: caller's A/X/Y/P, JMP (rom vector). */
constexpr data_t kStub[] = {
  0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde, /* OPEN..CHKOUT */
  0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde, /* CLRCHN..GETIN */
  0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde, 0x20, 0xa1, 0xde,                   /* CLALL LOAD SAVE */
  /* $dea1 common */
  0x8d, 0xf0, 0xde,  /* STA R_A */
  0x8e, 0xf1, 0xde,  /* STX R_X */
  0x8c, 0xf2, 0xde,  /* STY R_Y */
  0x08,              /* PHP */
  0x68,              /* PLA */
  0x8d, 0xf8, 0xde,  /* STA R_PIN */
  0x68,              /* PLA      return low byte */
  0x8d, 0xf4, 0xde,  /* STA R_CMD  runs the function */
  0x68,              /* PLA      return high byte, dropped */
  0xad, 0xf5, 0xde,  /* LDA R_PASS */
  0xd0, 0x0f,        /* BNE pass */
  0xad, 0xf3, 0xde,  /* LDA R_P */
  0x48,              /* PHA */
  0xad, 0xf0, 0xde,  /* LDA R_A */
  0xae, 0xf1, 0xde,  /* LDX R_X */
  0xac, 0xf2, 0xde,  /* LDY R_Y */
  0x28,              /* PLP */
  0x60,              /* RTS */
  /* $dec8 pass */
  0xad, 0xf8, 0xde,  /* LDA R_PIN */
  0x48,              /* PHA */
  0xad, 0xf0, 0xde,  /* LDA R_A */
  0xae, 0xf1, 0xde,  /* LDX R_X */
  0xac, 0xf2, 0xde,  /* LDY R_Y */
  0x28,              /* PLP */
  0x6c, 0xf6, 0xde,  /* JMP (R_JMPLO) */
};
static_assert(sizeof(kStub) <= kRegBase - kStubBase, "stub overlaps the registers");

constexpr uint8_t kRetLoFirst = static_cast<uint8_t>((kStubBase + 2) & 0xff);

/* 1541 status codes used here */
constexpr uint8_t kOk = 0, kWriteProtect = 26, kSyntax = 31, kNotFound = 62, kDosVersion = 73;

bool name_matches(const char * pattern, const char * name)
{
  while (*pattern != '\0') {
    if (*pattern == '*') return true;
    if (*name == '\0') return false;
    if (*pattern != '?' && *pattern != *name) return false;
    pattern++;
    name++;
  }
  return *name == '\0';
}

/* Drops "@", a "0:" or ":" drive part and anything from the first ',' */
const char * strip_name(char * name, bool & write)
{
  write = false;
  char * p = name;
  if (*p == '@') p++;
  for (char * c = p; *c != '\0'; c++) {
    if (*c == ':') { p = c + 1; break; }
  }
  for (char * c = p; *c != '\0'; c++) {
    if (*c != ',') continue;
    *c = '\0';
    for (char * m = c + 1; *m != '\0'; m++) if (*m == 'W' || *m == 'A') write = true;
    break;
  }
  return p;
}

/* "$", "$0", "$:PAT", "$0:PAT*" -> pattern after the colon, "*" without one */
const char * dir_pattern(char * raw)
{
  for (char * c = raw; *c != '\0'; c++) {
    if (*c != ':') continue;
    bool write = false;
    const char * p = strip_name(raw, write);
    return (*p == '\0') ? "*" : p;
  }
  return "*";
}

} /* namespace */

VirtualDrive::VirtualDrive(Ram & ram, IoDevice * lower, const VFile * files, size_t file_count,
                           const char * disk_name, const char * disk_id)
  : ram_(ram), lower_(lower), files_(files), file_count_(file_count),
    disk_name_(disk_name), disk_id_(disk_id)
{
  reset();
}

void VirtualDrive::reset(void)
{
  close_all();
  in_la_ = 0;
  set_status(kDosVersion, "CBM DOS V2.6 1541");
}

void VirtualDrive::install(void)
{
  for (size_t f = 0; f < FN_COUNT; f++) {
    const addr_t v = kVectors[f];
    const addr_t cur = static_cast<addr_t>(ram_.dma_read(v) | (ram_.dma_read(static_cast<addr_t>(v + 1)) << 8));
    /* A second install keeps the KERNAL targets */
    if (!(installed_ && cur >= kStubBase && cur < kRegBase)) rom_[f] = cur;
    const addr_t entry = static_cast<addr_t>(kStubBase + 3 * f);
    ram_.dma_write(v, static_cast<data_t>(entry & 0xff));
    ram_.dma_write(static_cast<addr_t>(v + 1), static_cast<data_t>(entry >> 8));
  }
  installed_ = true;
}

data_t VirtualDrive::io_read(addr_t addr)
{
  const uint8_t off = static_cast<uint8_t>(addr & 0xff);
  if (off < 0x80) return (lower_ != nullptr) ? lower_->io_read(addr) : 0xff;
  if (addr >= kRegBase) return regs_[addr - kRegBase];
  const size_t i = static_cast<size_t>(addr - kStubBase);
  return (i < sizeof(kStub)) ? kStub[i] : 0xea;
}

void VirtualDrive::io_write(addr_t addr, data_t value)
{
  const uint8_t off = static_cast<uint8_t>(addr & 0xff);
  if (off < 0x80) {
    if (lower_ != nullptr) lower_->io_write(addr, value);
    return;
  }
  if (addr < kRegBase) return;
  regs_[addr - kRegBase] = value;
  if (addr - kRegBase == R_CMD) execute(value);
}

void VirtualDrive::execute(uint8_t ret_lo)
{
  const uint8_t fn = static_cast<uint8_t>((ret_lo - kRetLoFirst) / 3);
  regs_[R_PASS] = 0;
  bool handled = false;
  bool carry = false;

  switch (fn) {
    case OPEN:
      if (ram_.dma_read(kDevice) == kDrive) { handled = true; carry = op_open(); }
      break;
    case CLOSE:
      if (channel_for(regs_[R_A]) != nullptr) { handled = true; carry = op_close(); }
      break;
    case CHKIN:
    case CHKOUT:
      if (channel_for(regs_[R_X]) != nullptr) { handled = true; carry = op_chkin(fn == CHKOUT); }
      break;
    case CLRCHN:
      if (ram_.dma_read(kInDev) == kDrive) ram_.dma_write(kInDev, 0);
      if (ram_.dma_read(kOutDev) == kDrive) ram_.dma_write(kOutDev, 3);
      break;
    case CHRIN:
    case GETIN:
      if (ram_.dma_read(kInDev) == kDrive) { handled = true; op_chrin(); }
      break;
    case CHROUT:
      /* Writes to a write protected disk go nowhere */
      if (ram_.dma_read(kOutDev) == kDrive) handled = true;
      break;
    case CLALL:
      close_all();
      if (ram_.dma_read(kInDev) == kDrive) ram_.dma_write(kInDev, 0);
      if (ram_.dma_read(kOutDev) == kDrive) ram_.dma_write(kOutDev, 3);
      break;
    case LOAD:
      if (ram_.dma_read(kDevice) == kDrive) { handled = true; carry = op_load(); }
      break;
    case SAVE:
      if (ram_.dma_read(kDevice) == kDrive) { handled = true; op_save(); carry = true; }
      break;
    default:
      break;
  }

  if (!handled) {
    const addr_t rom = (fn < FN_COUNT) ? rom_[fn] : 0;
    regs_[R_PASS] = 1;
    regs_[R_JMPLO] = static_cast<data_t>(rom & 0xff);
    regs_[R_JMPHI] = static_cast<data_t>(rom >> 8);
    return;
  }
  finish(carry);
}

void VirtualDrive::finish(bool carry)
{
  const data_t a = regs_[R_A];
  data_t p = static_cast<data_t>(regs_[R_PIN] & ~(0x80 | 0x02 | 0x01));
  if (a == 0) p |= 0x02;
  if (a & 0x80) p |= 0x80;
  if (carry) p |= 0x01;
  regs_[R_P] = p;
}

VirtualDrive::Channel * VirtualDrive::channel_for(uint8_t la)
{
  for (Channel & c : channels_) if (c.used && c.la == la) return &c;
  return nullptr;
}

void VirtualDrive::close_all(void)
{
  for (Channel & c : channels_) c = Channel{};
}

size_t VirtualDrive::read_name(char * out)
{
  size_t len = ram_.dma_read(kNameLen);
  if (len > kNameMax) len = kNameMax;
  const addr_t p = static_cast<addr_t>(ram_.dma_read(kNamePtr) | (ram_.dma_read(static_cast<addr_t>(kNamePtr + 1)) << 8));
  for (size_t i = 0; i < len; i++) out[i] = static_cast<char>(ram_.dma_read(static_cast<addr_t>(p + i)));
  out[len] = '\0';
  return len;
}

const VFile * VirtualDrive::find(const char * pattern)
{
  if (*pattern == '\0') return nullptr;
  for (size_t i = 0; i < file_count_; i++) {
    if (name_matches(pattern, files_[i].name)) return &files_[i];
  }
  return nullptr;
}

void VirtualDrive::set_status(uint8_t code, const char * text)
{
  size_t n = 0;
  status_[n++] = static_cast<char>('0' + code / 10);
  status_[n++] = static_cast<char>('0' + code % 10);
  status_[n++] = ',';
  if (code == kOk) status_[n++] = ' ';
  while (*text != '\0' && n < sizeof(status_) - 8) status_[n++] = *text++;
  const char tail[] = ",00,00\r";
  for (const char * t = tail; *t != '\0'; t++) status_[n++] = *t;
  status_len_ = n;
}

void VirtualDrive::run_command(const char * cmd)
{
  switch (cmd[0]) {
    case '\0':
      break;
    case 'I': case 'U':
      set_status(kOk, "OK");
      break;
    case 'S': case 'R': case 'N': case 'C': case 'V': case 'B': case 'M': case 'P':
      set_status(kWriteProtect, "WRITE PROTECT ON");
      break;
    default:
      set_status(kSyntax, "SYNTAX ERROR");
      break;
  }
}

void VirtualDrive::build_directory(const char * pattern)
{
  size_t n = 0;
  auto put = [&](uint8_t b) { if (n < kDirMax) dir_[n++] = b; };
  auto text = [&](const char * s) { while (*s != '\0') put(static_cast<uint8_t>(*s++)); };
  auto line = [&](unsigned number) { put(0x01); put(0x01); put(number & 0xff); put(number >> 8); };

  put(0x01); put(0x04);

  line(0);
  put(0x12); put('"');
  size_t len = 0;
  for (const char * s = disk_name_; *s != '\0' && len < 16; s++, len++) put(static_cast<uint8_t>(*s));
  for (; len < 16; len++) put(' ');
  put('"'); put(' ');
  text(disk_id_);
  text(" 2A");
  put(0);

  unsigned used = 0;
  for (size_t i = 0; i < file_count_; i++) {
    const unsigned blocks = static_cast<unsigned>((files_[i].size + 253) / 254);
    used += blocks;
    if (!name_matches(pattern, files_[i].name)) continue;
    line(blocks);
    text(blocks < 10 ? "   " : (blocks < 100 ? "  " : " "));
    put('"');
    size_t nl = 0;
    for (const char * s = files_[i].name; *s != '\0'; s++, nl++) put(static_cast<uint8_t>(*s));
    put('"');
    for (; nl < 16; nl++) put(' ');
    text(" PRG  ");
    put(0);
  }

  line(used < 664 ? 664 - used : 0);
  text("BLOCKS FREE.             ");
  put(0);
  put(0); put(0);
  dir_len_ = n;
}

bool VirtualDrive::op_open(void)
{
  const uint8_t la = ram_.dma_read(kLa);
  const uint8_t sa = ram_.dma_read(kSa) & 0x0f;
  if (channel_for(la) != nullptr) { regs_[R_A] = 2; return true; }  /* FILE OPEN */
  Channel * ch = nullptr;
  for (Channel & c : channels_) if (!c.used) { ch = &c; break; }
  if (ch == nullptr) { regs_[R_A] = 1; return true; }               /* TOO MANY FILES */

  char raw[kNameMax + 1];
  read_name(raw);
  ram_.dma_write(kStatus, 0);
  *ch = Channel{ true, la, sa, nullptr, 0, 0 };

  if (sa == 15) {
    run_command(raw);
    return false;
  }
  if (raw[0] == '$') {
    build_directory(dir_pattern(raw));
    ch->data = dir_;
    ch->size = dir_len_;
    set_status(kOk, "OK");
    return false;
  }
  bool write = false;
  const char * name = strip_name(raw, write);
  if (write || sa == 1) {
    set_status(kWriteProtect, "WRITE PROTECT ON");
    return false;
  }
  const VFile * f = find(name);
  if (f == nullptr) {
    set_status(kNotFound, "FILE NOT FOUND");
    return false;
  }
  ch->data = f->data;
  ch->size = f->size;
  set_status(kOk, "OK");
  return false;
}

bool VirtualDrive::op_close(void)
{
  Channel * ch = channel_for(regs_[R_A]);
  if (ch != nullptr) *ch = Channel{};
  return false;
}

bool VirtualDrive::op_chkin(bool out)
{
  const uint8_t la = regs_[R_X];
  if (out) {
    ram_.dma_write(kOutDev, kDrive);
  } else {
    ram_.dma_write(kInDev, kDrive);
    in_la_ = la;
  }
  return false;
}

void VirtualDrive::op_chrin(void)
{
  Channel * ch = channel_for(in_la_);
  uint8_t st = ram_.dma_read(kStatus);
  if (ch == nullptr) {
    regs_[R_A] = 0x0d;
    ram_.dma_write(kStatus, static_cast<data_t>(st | 0x42));
    return;
  }
  if (ch->sa == 15) {
    /* Status line, then back to OK like a 1541 */
    regs_[R_A] = static_cast<data_t>(status_[ch->pos++]);
    if (ch->pos >= status_len_) {
      ram_.dma_write(kStatus, static_cast<data_t>(st | 0x40));
      set_status(kOk, "OK");
      ch->pos = 0;
    }
    return;
  }
  if (ch->pos >= ch->size) {
    regs_[R_A] = 0x0d;
    ram_.dma_write(kStatus, static_cast<data_t>(st | 0x42));
    return;
  }
  regs_[R_A] = ch->data[ch->pos++];
  if (ch->pos >= ch->size) ram_.dma_write(kStatus, static_cast<data_t>(st | 0x40));
}

bool VirtualDrive::op_load(void)
{
  const bool verify = regs_[R_A] != 0;
  ram_.dma_write(kVerify, regs_[R_A]);
  ram_.dma_write(kStatus, 0);

  char raw[kNameMax + 1];
  read_name(raw);
  const uint8_t * data = nullptr;
  size_t size = 0;
  if (raw[0] == '$') {
    build_directory(dir_pattern(raw));
    data = dir_;
    size = dir_len_;
  } else {
    bool write = false;
    const VFile * f = find(strip_name(raw, write));
    if (f != nullptr) { data = f->data; size = f->size; }
  }
  if (data == nullptr || size < 2) {
    set_status(kNotFound, "FILE NOT FOUND");
    ram_.dma_write(kStatus, 0x42);
    regs_[R_A] = 4;  /* FILE NOT FOUND */
    return true;
  }

  addr_t addr = (ram_.dma_read(kSa) == 0)
    ? static_cast<addr_t>(ram_.dma_read(kLoadPtr) | (ram_.dma_read(static_cast<addr_t>(kLoadPtr + 1)) << 8))
    : static_cast<addr_t>(data[0] | (data[1] << 8));
  for (size_t i = 2; i < size; i++, addr++) {
    if (verify) {
      if (ram_.dma_read(addr) != data[i]) ram_.dma_write(kStatus, static_cast<data_t>(ram_.dma_read(kStatus) | 0x10));
    } else {
      ram_.dma_write(addr, data[i]);
    }
  }
  ram_.dma_write(kEndLo, static_cast<data_t>(addr & 0xff));
  ram_.dma_write(static_cast<addr_t>(kEndLo + 1), static_cast<data_t>(addr >> 8));
  regs_[R_X] = static_cast<data_t>(addr & 0xff);
  regs_[R_Y] = static_cast<data_t>(addr >> 8);
  set_status(kOk, "OK");
  return false;
}

void VirtualDrive::op_save(void)
{
  set_status(kWriteProtect, "WRITE PROTECT ON");
  regs_[R_A] = 5;
}

} /* namespace usbsid */
