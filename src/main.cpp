/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * main.cpp
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025 LouD
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

#include <bitset>
#include <sstream>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <ios>
#include <chrono>
#include <thread>

#include <sys/time.h>
#include <signal.h>

#include <mos6502.h>
#include <mos6510.h>
#include <sidfile.h>
#include <usplayer.h>

#if DESKTOP
#include <USBSID.h>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"


/* Player variables */
volatile sig_atomic_t stop;
static bool log_instructions = false;
static bool log_readwrites = false;
static bool log_sidrw = false;
static bool log_cia1rw = false;
static bool log_cia2rw = false;
static bool log_vicrw = false;
static bool file = false;

/* USBSID variables */
int pcbversion = -1;
int fmoplsidno = -1;
uint16_t sidone, sidtwo, sidthree, sidfour;
int sidssockone = 0, sidssocktwo = 0;
int sockonesidone = 0, sockonesidtwo = 0;
int socktwosidone = 0, socktwosidtwo = 0;
bool forcesockettwo = false; /* force play on socket two */
#if DESKTOP
USBSID_NS::USBSID_Class* usbsid;
#endif

/* SID file variables */
static uint8_t songno = 0;
string filename = "";
int clock_speed,
  raster_lines,
  frame_cycles,
  refresh_rate,
  play_rate,
  rasterrow_cycles,
  sidflags,
  curr_sidspeed,
  ct, cs, sv;
bool is_rsid = false;
uint32_t sidspeed;
uint16_t load_addr, sid_len, play_addr, init_addr;
uint8_t *sid_buffer;

/* C64 Variables */
uint8_t *RAM;
mos6510 *CpuA; /* A */
mos6502 *CpuB; /* B */
SidFile *sid;
static char active_cpu = 'A';
uint64_t cyclecount = 0, w_cyclecount = 0, r_cyclecount = 0;
int sidcount = 1;
int sidno;


#if DESKTOP

int setup_USBSID(void)
{
  usbsid = new USBSID_NS::USBSID_Class();

  printf("Opening USBSID-Pico with buffer for cycle exact writing\n");
  if (usbsid->USBSID_Init(true, true) < 0) {
    printf("USBSID-Pico not found, exiting\n");
    return 0;
  }
  /* Sleep 400ms for Reset to settle */
  struct timespec tv = { .tv_sec = 0, .tv_nsec = (400 * 1000 * 1000) };
  nanosleep(&tv, &tv);

  return 1;
}

void clear_USBSID(void)
{
  if (usbsid) {
    usbsid->USBSID_Mute();
    usbsid->USBSID_ClearBus();
    usbsid->USBSID_UnMute();
  }
  return;
}

void getinfo_USBSID(void)
{
  if(usbsid->USBSID_GetClockRate() != clock_speed) {
    usbsid->USBSID_SetClockRate(clock_speed, true);
  }

  if(usbsid->USBSID_GetNumSIDs() < sidcount) {
    printf("[WARNING] Tune no.sids %d is higher then USBSID-Pico no.sids %d\n", sidcount, usbsid->USBSID_GetNumSIDs());
  }

  uint8_t socket_config[10];
  usbsid->USBSID_GetSocketConfig(socket_config);
  printf("SOCKET CONFIG: ");
  for (int i = 0; i < 10; i++) {
    printf("%02X ", socket_config[i]);
  }
  printf("\n");

  printf("SOCK1#.%d SID1:%d SID2:%d\nSOCK2#.%d SID1:%d SID2:%d\n",
    usbsid->USBSID_GetSocketNumSIDS(1, socket_config),
    usbsid->USBSID_GetSocketSIDType1(1, socket_config),
    usbsid->USBSID_GetSocketSIDType2(1, socket_config),
    usbsid->USBSID_GetSocketNumSIDS(2, socket_config),
    usbsid->USBSID_GetSocketSIDType1(2, socket_config),
    usbsid->USBSID_GetSocketSIDType2(2, socket_config)
  );

  sidssockone = usbsid->USBSID_GetSocketNumSIDS(1, socket_config);
  sidssocktwo = usbsid->USBSID_GetSocketNumSIDS(2, socket_config);
  sockonesidone = usbsid->USBSID_GetSocketSIDType1(1, socket_config);
  sockonesidtwo = usbsid->USBSID_GetSocketSIDType2(1, socket_config);
  socktwosidone = usbsid->USBSID_GetSocketSIDType1(2, socket_config);
  socktwosidtwo = usbsid->USBSID_GetSocketSIDType2(2, socket_config);
  fmoplsidno = usbsid->USBSID_GetFMOplSID();
  pcbversion = usbsid->USBSID_GetPCBVersion();

  return;
}

#endif

void init(void)
{
  printf("[C64] Init\n");
  RAM = new uint8_t[C64RamSize]();
  /* Erase RAM */
  memset(RAM,0,sizeof(RAM));
  #if DESKTOP
  if (!setup_USBSID()) { usbsid = nullptr; }
  #endif
}

void deinit(void)
{
  printf("[C64] Deinit\n");
  #if DESKTOP
  if (usbsid) {
    usbsid->USBSID_Flush();
    usbsid->USBSID_Reset();
    usbsid->USBSID_UnMute();
    delete usbsid;
  }
  #endif
  delete CpuA;
  delete CpuB;
  delete RAM;
}

void inthand(int signum)
{
  stop = 1;
}

uint8_t addr_translation(uint16_t addr)
{
  uint8_t sock2add = (forcesockettwo ? (sidssockone == 1 ? 0x20 : sidssockone == 2 ? 0x40 : 0x0) : 0x0);
    if (addr == 0xDF40 || addr == 0xDF50) {
        if (fmoplsidno >= 1 && fmoplsidno <=4) {
            sidno = fmoplsidno;
            return (((sidno - 1) * 0x20) + (addr & 0x1F));
        } else {
            sidno = 5; /* Skip */
            return (0x80 + (addr & 0x1F)); /* Out of scope address */
        }
    }
    switch (sidcount) {
        case 1: /* Easy just return the address */
            if (addr >= sidone && addr < (sidone + 0x1F)) {
                sidno = 1;
                return (sock2add + (addr & 0x1F));
            };
            break;
        case 2: /* Intermediate remap all above 0x20 to sidno 2 */
            if (addr >= sidone && addr < (sidone + 0x1F)) {
                sidno = 1;
                return (addr & 0x1F);
            } else if (addr >= sidtwo && addr < (sidtwo + 0x1F)) {
                sidno = 2;
                return (0x20 + (addr & 0x1F));
            };
            break;
        case 3: /* Advanced */
            if (addr >= sidone && addr < (sidone + 0x1F)) {
                sidno = 1;
                return (addr & 0x1F);
            } else if (addr >= sidtwo && addr < (sidtwo + 0x1F)) {
                sidno = 2;
                return (0x20 + (addr & 0x1F));
            } else if (addr >= sidthree && addr < (sidthree + 0x1F)) {
                sidno = 3;
                return (0x40 + (addr & 0x1F));
            };
            break;
        case 4: /* Expert */
            if (addr >= sidone && addr < (sidone + 0x1F)) {
                sidno = 1;
                return (addr & 0x1F);
            } else if (addr >= sidtwo && addr < (sidtwo + 0x1F)) {
                sidno = 2;
                return (0x20 + (addr & 0x1F));
            } else if (addr >= sidthree && addr < (sidthree + 0x1F)) {
                sidno = 3;
                return (0x40 + (addr & 0x1F));
            } else if (addr >= sidfour && addr < (sidfour + 0x1F)) {
                sidno = 4;
                return (0x60 + (addr & 0x1F));
            };
            break;
        default:
            break;
    };
    return 0xFE;
}

void set_sid_addresses(void)
{
  sidcount = ((sv == 3)
    ? 2
    : (sv == 4)
    ? 3
    : (sv == 78)
    ? 4
    : 1);
  sidno = 0;
  sidone = 0xD400;
  printf("SIDS: [1]$%04X ", sidone);
  if (sv == 3 || sv == 4 || sv == 78) {
    sidtwo = 0xD000 | (sid->GetSIDaddr(2) << 4);
    printf("[2]$%04X ", sidtwo);
    if (sv == 4 || sv == 78) {
      sidthree = 0xD000 | (sid->GetSIDaddr(3) << 4);
      // sidthree = (sidthree == sidone ? 0xD440 : sidthree);
      printf("[3]$%04X ", sidthree);
    }
    if (sv == 78) {
      sidfour = 0xD000 | (sid->GetSIDaddr(4) << 4);
      // sidfour = (sidfour == sidthree ? 0xD460 : sidfour);
      printf("[4]$%04X ", sidfour);
    }
  }
  printf("\n");
}

/* SID */
uint8_t read_sid(uint16_t addr)
{
  uint8_t phyaddr = addr_translation(addr) & 0xFF;  /* 4 SIDs max */
  uint8_t data = 0;
  uint16_t cycles = (cyclecount-r_cyclecount);
  if (log_sidrw) printf("[R SID%d]$%04x(%02x):%02x C:%5u\n",
    sidno,addr,phyaddr,data,cycles);
  r_cyclecount = cyclecount;

  return data; /* temporary */
}

void write_sid(uint16_t addr, uint8_t data)
{
  uint8_t phyaddr = addr_translation(addr) & 0xFF;  /* 4 SIDs max */
  uint16_t cycles = (cyclecount - w_cyclecount);
  #if DESKTOP
  if (usbsid) { usbsid->USBSID_WriteRingCycled(phyaddr, data, cycles); }
  #endif
  if (log_sidrw) printf("[W SID%d]$%04x(%02x):%02x C:%5u\n",
    sidno,addr,phyaddr,data,cycles);
  w_cyclecount = cyclecount;

  return;
}

/* Memory */
uint8_t read_byte(uint16_t addr)
{
  uint8_t data = RAM[addr]; /* Always read from RAM (fallback) */
  RAM[addr] = data; /* Always write to RAM (fallback) */
  if (log_readwrites) printf("[R  MEM]$%04x:%02x\n",addr,data);
  switch (addr) {
    case kAddrVicFirstPage ... (kAddrVicLastPage + (C64PageSize-1)):
      if (log_vicrw) printf("[R  VIC]$%04x:%02x\n",addr,data);
      break;
    case kAddrSIDFirstPage ... (kAddrSIDLastPage + (C64PageSize-1)):
      data = read_sid(addr);
      break;
    case kAddrCIA1Page ... (kAddrCIA1Page + (C64PageSize-1)):
      if (log_cia1rw) printf("[R CIA1]$%04x:%02x\n",addr,data);
      break;
    case kAddrCIA2Page ... (kAddrCIA2Page + (C64PageSize-1)):
      if (log_cia2rw) printf("[R CIA2]$%04x:%02x\n",addr,data);
      break;
    case kAddrIO1Page ... (kAddrIO2Page + (C64PageSize-1)):
      data = read_sid(addr);
      break;
    default:
      break;
  }
  return data;
}

void write_byte(uint16_t addr, uint8_t data)
{
  cyclecount = ((active_cpu == 'A') ? CpuA->cycles() : cyclecount);
  RAM[addr] = data; /* Always write to RAM (fallback) */
  if (log_readwrites) printf("[W  MEM]$%04x:%02x\n",addr,data);
  switch (addr) {
    case kAddrVicFirstPage ... (kAddrVicLastPage + (C64PageSize-1)):
      if (log_vicrw) printf("[W  VIC]$%04x:%02x\n",addr,data);
      break;
    case kAddrSIDFirstPage ... (kAddrSIDLastPage + (C64PageSize-1)):
      write_sid(addr, data);
      break;
    case kAddrCIA1Page ... (kAddrCIA1Page + (C64PageSize-1)):
      if (log_cia1rw) printf("[W CIA1]$%04x:%02x\n",addr,data);
      break;
    case kAddrCIA2Page ... (kAddrCIA2Page + (C64PageSize-1)):
      if (log_cia2rw) printf("[W CIA2]$%04x:%02x\n",addr,data);
      break;
    case kAddrIO1Page ... (kAddrIO2Page + (C64PageSize-1)):
      write_sid(addr, data);
      break;
    default:
      break;
  }
}

void cycle_callback(mos6510* cpu)
{
  printf("POO! %u\n",cpu->cycles());
  return;
}

void load_sid_microplayer(int song_number)
{
  /* Retrieve SID variables */
  uint16_t load_addr = sid->GetLoadAddress();
  uint16_t play_addr = sid->GetPlayAddress();
  uint16_t init_addr = sid->GetInitAddress();
  uint16_t sid_len = sid->GetDataLength();
  uint8_t *sid_buffer = sid->GetDataPtr();

  /* Copy SID data to RAM */
  memcpy(RAM+load_addr,sid_buffer,sid_len);

  /* Initialize micro player */
  /* install reset vector for microplayer (0x0000) */
  RAM[0xFFFD] = 0x00;
  RAM[0xFFFC] = 0x00;

  // install IRQ vector for play routine launcher (0x0013)
  RAM[0xFFFF] = 0x00;
  RAM[0xFFFE] = 0x13;

  // install the micro player, 6502 assembly code

  RAM[0x0000] = 0xA9; // A = 0, load A with the song number
  RAM[0x0001] = song_number;

  RAM[0x0002] = 0x20;               // jump sub to INIT routine
  RAM[0x0003] = init_addr & 0xFF;        // lo addr
  RAM[0x0004] = (init_addr >> 8) & 0xFF; // hi addr

  RAM[0x0005] = 0x58; // enable interrupt
  RAM[0x0006] = 0xEA; // nop
  RAM[0x0007] = 0x4C; // jump to 0x0006
  RAM[0x0008] = 0x06;
  RAM[0x0009] = 0x00;

  RAM[0x0013] = 0xEA; // nop  //0xA9; // A = 1
  RAM[0x0014] = 0xEA; // nop //0x01;
  RAM[0x0015] = 0xEA; // 0x78 CLI
  RAM[0x0016] = 0x20; // jump sub to play routine
  RAM[0x0017] = play_addr & 0xFF;
  RAM[0x0018] = (play_addr >> 8) & 0xFF;
  RAM[0x0019] = 0xEA; // 0x58 SEI
  RAM[0x001A] = 0x40; // RTI: return from interrupt

  printf("[CPU: %s]\n", (active_cpu == 'A' ? "mos6510" : "mos6502"));
  if (active_cpu == 'A') { /* A */
    if (log_instructions) CpuA->loginstructions = true;
    CpuA->reset();
    CpuA->emulate_n(INIT_CYCLES);
  } else { /* B */
    if (log_instructions) CpuB->loginstructions = true;
    CpuB->Reset();
    CpuB->RunN(INIT_CYCLES, cyclecount);
  }
}

void process_arguments(int argc, char **argv)
{
  for (int param_count = 1; param_count < argc; param_count++) {
    if (filename.length() == 0 and argv[param_count][0] != '-') {
      filename = argv[param_count];
      file = true;
    }
    if (!strcmp(argv[param_count], "-a") and argv[param_count][1] != 'b') { /* Set CpuA */
      active_cpu = 'A';
    }
    if (!strcmp(argv[param_count], "-b") and argv[param_count][1] != 'a') { /* Set CpuB */
      active_cpu = 'B';
    }
    if (!strcmp(argv[param_count], "-s")) { /* Set subtune # */
      param_count++;
      songno = (atoi(argv[param_count]) - 1);
    }
    if (!strcmp(argv[param_count], "-srw")) { /* log sid read/writes */
      log_sidrw = true;
    }
    if (!strcmp(argv[param_count], "-c1rw")) { /* log cia1 read/writes */
      log_cia1rw = true;
    }
    if (!strcmp(argv[param_count], "-c2rw")) { /* log cia2 read/writes */
      log_cia2rw = true;
    }
    if (!strcmp(argv[param_count], "-vrw")) { /* log vic read/writes */
      log_vicrw = true;
    }
    if (!strcmp(argv[param_count], "-lrw")) { /* log all read/writes */
      log_readwrites = true;
    }
    if (!strcmp(argv[param_count], "-ins")) { /* log all cpu instructions */
      log_instructions = true;
    }
  }
}

void print_sid_info(void)
{
  sv = sid->GetSidVersion();
  cout << "---------------------------------------------" << endl;
  cout << "SID Title          : " << sid->GetModuleName() << endl;
  cout << "Author Name        : " << sid->GetAuthorName() << endl;
  cout << "Release & (C)      : " << sid->GetCopyrightInfo() << endl;
  cout << "---------------------------------------------" << endl;
  cout << "SID Type           : " << sid->GetSidType() << endl;
  cout << "SID Format version : " << dec << sv << endl;
  cout << "---------------------------------------------" << endl;
  cout << "SID Flags          : 0x" << hex << sidflags << " 0b" << bitset<8>{sidflags} << endl;
  cout << "Chip Type          : " << chiptype[ct] << endl;
  if (sv == 3 || sv == 4)
  cout << "Chip Type 2        : " << chiptype[sid->GetChipType(2)] << endl;
  if (sv == 4)
  cout << "Chip Type 3        : " << chiptype[sid->GetChipType(3)] << endl;
  cout << "Clock Type         : " << hex << clockspeed[cs] << endl;
  cout << "Clock Speed        : " << dec << clock_speed << endl;
  cout << "Raster Lines       : " << dec << raster_lines << endl;
  cout << "Rasterrow Cycles   : " << dec << rasterrow_cycles << endl;
  cout << "Frame Cycles       : " << dec << frame_cycles << endl;
  cout << "Refresh Rate       : " << dec << refresh_rate << endl;
  if (sv == 3 || sv == 4 || sv == 78) {
    cout << "---------------------------------------------" << endl;
    cout << "SID 2 $addr        : $d" << hex << sid->GetSIDaddr(2) << "0" << endl;
    if (sv == 4 || sv == 78)
      cout << "SID 3 $addr        : $d" << hex << sid->GetSIDaddr(3) << "0" << endl;
    if (sv == 78)
      cout << "SID 4 $addr        : $d" << hex << sid->GetSIDaddr(4) << "0" << endl;
  }
  cout << "---------------------------------------------" << endl;
  cout << "Data Offset        : $" << setfill('0') << setw(4) << hex << sid->GetDataOffset() << endl;
  cout << "Image length       : $" << hex << sid->GetInitAddress() << " - $" << hex << (sid->GetInitAddress() - 1) + sid->GetDataLength() << endl;
  cout << "Load Address       : $" << hex << sid->GetLoadAddress() << endl;
  cout << "Init Address       : $" << hex << sid->GetInitAddress() << endl;
  cout << "Play Address       : $" << hex << sid->GetPlayAddress() << endl;
  cout << "---------------------------------------------" << endl;
    cout << "Song Speed(s)      : $" << hex << curr_sidspeed << " $0x" << hex << sidspeed << " 0b" << bitset<32>{sidspeed} << endl;
    cout << "Timer              : " << (curr_sidspeed == 1 ? "CIA1" : "Clock") << endl;
    cout << "Selected Sub-Song  : " << dec << songno + 1 << " / " << dec << sid->GetNumOfSongs() << endl;
  cout << "---------------------------------------------" << endl;

  return;
}

void parse_sid_info(void)
{
  is_rsid = (sid->GetSidType() == "RSID");

  sidflags = sid->GetSidFlags();
  sidspeed = sid->GetSongSpeed(songno); // + 1);
  curr_sidspeed = (sidspeed & (1 << songno)); // ? 1 : 0;  // 1 ~ 60Hz, 2 ~ 50Hz
  ct = sid->GetChipType(1);
  cs = sid->GetClockSpeed();
  sv = sid->GetSidVersion();
  // 2 2 3 ~ Genius
  // 2 1 3 ~ Jump
  // printf("%d %d %d\n", ct, cs, sv);
  clock_speed = clockSpeed[cs];
  raster_lines = scanLines[cs];
  rasterrow_cycles = scanlinesCycles[cs];
  frame_cycles = (raster_lines * rasterrow_cycles);
  refresh_rate = refreshRate[cs];

  play_rate = 0;
  if (curr_sidspeed == 1) {
    play_rate = (RAM[CIA_TIMER_HI] << 8 | RAM[CIA_TIMER_LO]);  /* CIA timing */
    play_rate = play_rate == 0 ? refresh_rate : play_rate; /* Fallback */
  } else {
    play_rate = refresh_rate; /* V-Blank */
  }

  return;
}

void psid_player(void)
{
  timeval t1, t2;
  timeval d1;
  long int elaps;
  while (!stop) {

    gettimeofday(&t1, NULL);

    if (stop) {
      break;
    }

    gettimeofday(&d1, NULL);
    if (active_cpu == 'A') {
      CpuA->irq();
      CpuA->emulate_n(0);
    } else { /* 1 */
      CpuB->IRQ();
      CpuB->RunN(0, cyclecount);
    }

    gettimeofday(&t2, NULL);
    /* printf("[EXEC TIME] EQ:%5d NE:%7d C:%5d\n",
      (t2.tv_usec - d1.tv_usec),
      (clock_speed - d1.tv_usec + t2.tv_usec),
      cyclecount
    ); */

    // wait 1/50 sec.
    if (t1.tv_sec == t2.tv_sec) {
      elaps = t2.tv_usec - t1.tv_usec;
    } else {
      elaps = clock_speed - t1.tv_usec + t2.tv_usec;
    }
    if (elaps < play_rate /* frame_cycles */) {
      std::this_thread::sleep_for(std::chrono::microseconds(play_rate - elaps));
    }
    #if DESKTOP
    if (usbsid) { usbsid->USBSID_SetFlush(); }
    #endif
  }
}

int main(int argc, char **argv)
{
  signal(SIGINT, inthand);
  init();

  sid = new SidFile();

  process_arguments(argc,argv);

  int res = (file ? sid->Parse(filename) : -1);

  if (file) {
    parse_sid_info();
    print_sid_info();
  } else {
    printf("No SID file supplied, exiting!\n");
    return 1;
  }

  if (songno < 0 or songno >= sid->GetNumOfSongs())
  {
    cout << "Warning: Invalid Sub-Song Number. Default Sub-Song will be chosen." << endl;
    songno = sid->GetFirstSong();
  }

  #if DESKTOP
  if (usbsid) { getinfo_USBSID(); }
  #endif

  CpuA = (active_cpu == 'A' ? new mos6510(read_byte, write_byte) : nullptr);
  CpuB = (active_cpu == 'B' ? new mos6502(read_byte, write_byte) : nullptr);
  // if (CpuA) CpuA->set_cycle_callback(cycle_callback); /* Disabled in favor of internal callback */

  load_sid_microplayer(songno);
  set_sid_addresses();
  delete sid;

  if (!is_rsid) { psid_player(); }

  deinit();

  return 0;
}

#pragma GCC diagnostic pop
