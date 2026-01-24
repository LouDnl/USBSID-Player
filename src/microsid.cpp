/*
 * USBSID-Player aims to be a command line SID file player that is also
 * suited for embedding where both implementations target use
 * with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
 * Pico2/Pico2W (RP2350) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with
 * your computer, phone or ASID supporting player
 *
 * Parts if this emulator are based on other great emulators and players
 * like Vice, SidplayFp, Websid, SidBerry and emudore/adorable
 *
 * microsid.cpp
 * This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2025-2026 LouD
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
#if DESKTOP
#include <signal.h>
#endif

#include <mos6510_cpu.h>
#include <mos6526_cia.h>
#include <mos906114_pla.h>
#include <mos6560_6561_vic.h>
#include <mos6581_8580_sid.h>

#include <sidfile.h>

#include <c64util.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"


/* External emulator functions */
extern void getinfo_USBSID(int clockspeed);
extern uint8_t emu_read_byte(uint16_t addr);
extern void emu_write_byte(uint16_t addr, uint8_t data);
extern uint8_t emu_dma_read_ram(uint16_t address);
extern uint8_t emu_dma_write_ram(uint16_t address, uint8_t data);
extern void cycle_callback(mos6510* cpu);
extern void emulate_c64_upto(uint_least16_t pc);
extern void emulate_until_opcode(uint_least8_t opcode);
extern void emulate_until_rti(void);
extern void emulate_c64(void);
extern void start_c64_test(void);
extern void log_logs(void);

/* External emulator variables */
extern mos6510 *Cpu;
extern mos6526 *Cia1;
extern mos6526 *Cia2;
extern mos6560_6561 *Vic;
extern mos906114 *Pla;
extern mos6581_8580 *SID;
extern bool
  log_instructions,
  log_timers,
  log_readwrites,
  log_sidrw,
  log_cia1rw,
  log_cia2rw,
  log_vicrw,
  log_vicrrw;

/* Player variables */
bool havefile = false;
bool prgfile = false;
bool force_rsiddrv = false;

/* MicroSID player variables */
enum CIA_registers
{ /* https://www.c64-wiki.com/wiki/mos6526 */
  CIA1_TIMER_LO = 0xDC04,
  CIA1_TIMER_HI = 0xDC05
};

static const char *us_sidtype[5] = {"Unknown", "N/A", "MOS8580", "MOS6581", "FMopl" };  /* 0 = unknown, 1 = N/A, 2 = MOS8085, 3 = MOS6581, 4 = FMopl */
static const char *chiptype[4] = {"Unknown", "MOS6581", "MOS8580", "MOS6581 and MOS8580"};
static const char *clockspeed[5] = {"Unknown", "PAL", "NTSC", "PAL and NTSC", "DREAN"};

/* SID file variables applied to C64 */
SidFile *sidfile_;
uint8_t songno = -1;
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
uint16_t load_addr,
  play_addr,
  init_addr,
  sid_len;
uint8_t *psid_buffer;

/* external USBSID variables */
#if DESKTOP
#include <USBSID.h>
extern USBSID_NS::USBSID_Class* usbsid;
#elif EMBEDDED
#include <config.h>
extern "C" {
// #include <sid.h>
void reset_sid(void);
int return_clockrate(void);
void apply_clockrate(int n_clock, bool suspend_sids);
extern Config usbsid_config;
extern RuntimeCFG cfg;
}
#endif
extern uint16_t sidone, sidtwo, sidthree, sidfour;
extern int pcbversion, fmoplsidno;
extern int sidssockone, sidssocktwo;
extern int sockonesidone, sockonesidtwo;
extern int socktwosidone, socktwosidtwo;
extern bool forcesockettwo;
extern int sidcount;
extern int sidno;


/**
 * @brief Set the sid addresses object
 * @note MicroSID player function
 */
void set_sid_addresses(void) // TODO: Must set sid class properties here! but how!?
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
  MOSDBG("[SID] [1]$%04X ", sidone);
  if (sv == 3 || sv == 4 || sv == 78) {
    sidtwo = 0xD000 | (sidfile_->GetSIDaddr(2) << 4);
    MOSDBG("[2]$%04X ", sidtwo);
    if (sv == 4 || sv == 78) {
      sidthree = 0xD000 | (sidfile_->GetSIDaddr(3) << 4);
      // sidthree = (sidthree == sidone ? 0xD440 : sidthree);
      MOSDBG("[3]$%04X ", sidthree);
    }
    if (sv == 78) {
      sidfour = 0xD000 | (sidfile_->GetSIDaddr(4) << 4);
      // sidfour = (sidfour == sidthree ? 0xD460 : sidfour);
      MOSDBG("[4]$%04X ", sidfour);
    }
  }
  MOSDBG("\n");
}

/**
 * @brief Copy SID data to RAM
 * @note MicroSID player function
 */
void copy_sid_to_ram(void)
{
  /* Retrieve SID variables */
  load_addr  = sidfile_->GetLoadAddress();
  play_addr  = sidfile_->GetPlayAddress();
  init_addr  = sidfile_->GetInitAddress();
  sid_len    = sidfile_->GetDataLength();
  psid_buffer = sidfile_->GetDataPtr();

  /* Copy SID data to RAM */
  // MOSDBG("[USPLAYER] RAM@0x%x SID@0x%x SIZE:%u\n", RAM+load_addr,psid_buffer,sid_len);
  for(int i = load_addr; i < (load_addr+sid_len); i++) {
    emu_dma_write_ram(i,psid_buffer[(i-load_addr)]);
  }
}

/**
 * @brief Load the majestic MicroSID player into RAM
 * Uses datasette buffer region for microsid player
 * $033c-$03fb
 * @note MicroSID player function
 * @param song_number
 */
void load_microsid_player(uint_fast8_t song_number)
{
  copy_sid_to_ram();
  if (log_instructions) Cpu->loginstructions = true;

  /* Definitions confirming to sidfileformat.txt */
  uint_least8_t pal_lo = 0x25;
  uint_least8_t pal_hi = 0x40;
  uint_least8_t ntsc_lo = 0x42;
  uint_least8_t ntsc_hi = 0x95;
  uint_least8_t bank_config = (!is_rsid
      ? (
      ((load_addr < 0xa000) && (play_addr < 0xa000))
      ? 0x37
      : ((load_addr < 0xd000) && (play_addr < 0xd000))
      ? 0x36
      : ((load_addr >= 0xe000) && (play_addr >= 0xe000))
      ? 0x35
      : 0x34)
      : 0x37 /* RSID default */
    );

  uint_least8_t rst_lo = 0x3e;
  uint_least8_t rst_hi = 0x03;

  uint_least8_t irq_lo = 0x51;
  uint_least8_t irq_hi = 0x03;

  uint_least8_t nmi_lo = 0x59;
  uint_least8_t nmi_hi = 0x03;

  uint_least8_t brk_lo = 0x47;
  uint_least8_t brk_hi = 0x03;

  uint_least8_t idle_lo = 0x43;
  uint_least8_t idle_hi = 0x03;

  /* BASIC TUNE REQUIRES: (Header 0x77 will be 0x01) */
  // emu_dma_read_ram(0x0039) = emu_dma_read_ram(0x0803);
  // emu_dma_read_ram(0x003a) = emu_dma_read_ram(0x0803);
  /* Also requires the JMP to be to 0xa7ae */

  /* Init Rmos6581_8580 */
  emu_dma_write_ram(0x033e, 0xa9);                    /* LDA #immediate */
  emu_dma_write_ram(0x033f, song_number);             /* song number */
  emu_dma_write_ram(0x0340, 0x20);                    /* JSR $absolute */
  emu_dma_write_ram(0x0341, (init_addr & 0xff));        /* init address lo */
  emu_dma_write_ram(0x0342, ((init_addr >> 8) & 0xff)); /* init address hi */

  /* Idling/Delay routine */
  emu_dma_write_ram(0x0343, 0xea);                    /* NOP implied */
  emu_dma_write_ram(0x0344, 0x4c);                    /* JMP $absolute */
  emu_dma_write_ram(0x0345, idle_lo);                 /* init address lo */
  emu_dma_write_ram(0x0346, idle_hi);                 /* init address hi */

  /* Break routine */
  emu_dma_write_ram(0x0347, 0xa2);                    /* LDX #immediate */
  emu_dma_write_ram(0x0348, 0xff);                    /*  */
  emu_dma_write_ram(0x0349, 0x9a);                    /* TXS implied */
  emu_dma_write_ram(0x034a, 0x58);                    /* CLI implied */
  emu_dma_write_ram(0x034b, 0x20);                    /* JSR $absolute */
  emu_dma_write_ram(0x034c, irq_lo);                  /* idle address lo */
  emu_dma_write_ram(0x034d, irq_hi);                  /* idle address hi */
  emu_dma_write_ram(0x034e, 0x4c);                    /* JMP $absolute */
  emu_dma_write_ram(0x034f, brk_lo);                  /* init address lo */
  emu_dma_write_ram(0x0350, brk_hi);                  /* init address hi */

  /* IRQ routine */
  emu_dma_write_ram(0x0351, 0xad);                    /* LDA $absolute */
  emu_dma_write_ram(0x0352, 0x0d);                    /* cia1 irq address lo */
  emu_dma_write_ram(0x0353, 0xdc);                    /* cia1 irq address hi */
  emu_dma_write_ram(0x0354, 0x68);                    /* PLA implied */
  emu_dma_write_ram(0x0355, 0xa8);                    /* TAY implied */
  emu_dma_write_ram(0x0356, 0x68);                    /* PLA implied */
  emu_dma_write_ram(0x0357, 0xaa);                    /* TAX implied */
  emu_dma_write_ram(0x0358, 0x68);                    /* PLA implied */
  /* NMI routine */
  emu_dma_write_ram(0x0359, 0x40);                    /* RTI */

  emu_dma_write_ram(0x030c, song_number); /* songno */
  emu_dma_write_ram(0x030d, song_number); /* songno */
  emu_dma_write_ram(0x030e, song_number); /* songno */
  emu_dma_write_ram(0x02A6, (cs ? 0x01 : 0x00)); /* Video standard PAL(+NTSC) / NTSC */

  emu_write_byte(0xd011, 0x1b); /* VIC set raster IRQ hi to 0x100 */
  emu_write_byte(0xd012, 0x37); /* VIC set raster IRQ lo to 0x37 */
  emu_write_byte(0xd01a, 0x00); /* VIC disable raster IRQ */

  emu_write_byte(0xdc0d, 0x7f); /* Cia 1 disable IRQ's */
  emu_write_byte(0xdc0e, 0x80); /* Cia 1 disable timer a */
  emu_write_byte(0xdc0f, 0x00); /* Cia 1 disable timer b */
  emu_write_byte(0xdc04, (cs ? pal_lo : ntsc_lo)); /* Cia 1 prescaler a lo */
  emu_write_byte(0xdc05, (cs ? pal_hi : ntsc_hi)); /* Cia 1 prescaler a hi and force load */
  emu_write_byte(0xdc06, 0xff); /* Cia 1 prescaler b lo */
  emu_write_byte(0xdc07, 0xff); /* Cia 1 prescaler b hi and force load */
  emu_write_byte(0xdc0d, 0x81); /* Cia 1 enable timer a IRQ */
  emu_write_byte(0xdc0e, 0x81); /* Cia 1 enable timer a with TOD @ 60Hz */

  emu_write_byte(0xdd0d, 0x7f); /* Cia 2 disable IRQ's */
  emu_write_byte(0xdd0e, 0x80); /* Cia 2 disable timer a */
  emu_write_byte(0xdd0f, 0x00); /* Cia 2 disable timer b */
  emu_write_byte(0xdd04, 0xff); /* Cia 2 prescaler a lo */
  emu_write_byte(0xdd05, 0xff); /* Cia 2 prescaler a hi and force load */
  emu_write_byte(0xdd06, 0xff); /* Cia 2 prescaler b lo */
  emu_write_byte(0xdd07, 0xff); /* Cia 2 prescaler b hi and force load */

  emu_write_byte(0x0001, bank_config); /* Switch banks */

  //  /* IRQ routine address */
  emu_dma_write_ram(0x0314, irq_lo); /* IRQ lo */
  emu_dma_write_ram(0x0315, irq_hi); /* IRQ hi */
  // /* Break routine address */
  emu_dma_write_ram(0x0316, brk_lo); /* BRK lo */
  emu_dma_write_ram(0x0317, brk_hi); /* BRK hi */
  // /* NMI routine address */
  emu_dma_write_ram(0x0318, nmi_lo); /* NMI lo */
  emu_dma_write_ram(0x0319, nmi_hi); /* NMI hi */

  // /* NMI execution address */
  emu_dma_write_ram(0xfffa, nmi_lo); /* NMI lo */
  emu_dma_write_ram(0xfffb, nmi_hi); /* NMI hi */
  // /* Reset vector execution address */
  emu_dma_write_ram(0xfffc, rst_lo); /* RST lo */
  emu_dma_write_ram(0xfffd, rst_hi); /* RST hi */
  // /* IRQ execution address */
  emu_dma_write_ram(0xfffe, irq_lo); /* IRQ lo */
  emu_dma_write_ram(0xffff, irq_hi); /* IRQ hi */

  MOSDBG("[RAM] $%04x:%02x\n", init_addr, emu_dma_read_ram(init_addr));

  Cpu->pc(((rst_hi << 8) | rst_lo));  /* For some reason the reset does not go to the reset vector address !? */

}

/**
 * @brief Print parsed SID information
 * @note MicroSID player function
 */
void print_sid_info(void)
{
  sv = sidfile_->GetSidVersion();
  cout << "---------------------------------------------" << endl;
  cout << "SID Title          : " << sidfile_->GetModuleName() << endl;
  cout << "Author Name        : " << sidfile_->GetAuthorName() << endl;
  cout << "Release & (C)      : " << sidfile_->GetCopyrightInfo() << endl;
  cout << "---------------------------------------------" << endl;
  cout << "SID Type           : " << sidfile_->GetSidType() << endl;
  cout << "SID Format version : " << dec << sv << endl;
  cout << "---------------------------------------------" << endl;
  cout << "SID Flags          : 0x" << hex << sidflags << " 0b" << bitset<8>{sidflags} << endl;
  cout << "Chip Type          : " << chiptype[ct] << endl;
  if (sv == 3 || sv == 4)
  cout << "Chip Type 2        : " << chiptype[sidfile_->GetChipType(2)] << endl;
  if (sv == 4)
  cout << "Chip Type 3        : " << chiptype[sidfile_->GetChipType(3)] << endl;
  cout << "Clock Type         : " << hex << clockspeed[cs] << endl;
  cout << "Clock Speed        : " << dec << clock_speed << endl;
  cout << "Raster Lines       : " << dec << raster_lines << endl;
  cout << "Rasterrow Cycles   : " << dec << rasterrow_cycles << endl;
  cout << "Frame Cycles       : " << dec << frame_cycles << endl;
  cout << "Refresh Rate       : " << dec << refresh_rate << endl;
  cout << "Refresh Frequency  : " << std::setprecision (5) << (double)(clock_speed/refresh_rate) << endl;
  if (sv == 3 || sv == 4 || sv == 78) {
    cout << "---------------------------------------------" << endl;
    cout << "SID 2 $addr        : $d" << hex << sidfile_->GetSIDaddr(2) << "0" << endl;
    if (sv == 4 || sv == 78)
      cout << "SID 3 $addr        : $d" << hex << sidfile_->GetSIDaddr(3) << "0" << endl;
    if (sv == 78)
      cout << "SID 4 $addr        : $d" << hex << sidfile_->GetSIDaddr(4) << "0" << endl;
  }
  cout << "---------------------------------------------" << endl;
  cout << "Data Offset        : $" << setfill('0') << setw(4) << hex << sidfile_->GetDataOffset() << endl;
  cout << "Image length       : $" << hex << sidfile_->GetLoadAddress() << " - $" << hex << (sidfile_->GetLoadAddress() - 1) + sidfile_->GetDataLength() << endl;
  cout << "Load Address       : $" << hex << sidfile_->GetLoadAddress() << endl;
  cout << "Init Address       : $" << hex << sidfile_->GetInitAddress() << endl;
  cout << "Play Address       : $" << hex << sidfile_->GetPlayAddress() << endl;
  cout << "Start Page         : $" << hex << sidfile_->GetStartPage() << endl;
  cout << "Max Pages          : $" << hex << sidfile_->GetMaxPages() << endl;
  cout << "---------------------------------------------" << endl;
    cout << "Song Speed(s)      : $" << hex << curr_sidspeed << " $0x" << hex << sidspeed << " 0b" << bitset<32>{sidspeed} << endl;
    cout << "Timer              : " << (curr_sidspeed == 1 ? "CIA1" : "Clock") << endl;
    cout << "Selected Sub-Song  : " << dec << songno + 1 << " / " << dec << sidfile_->GetNumOfSongs() << endl;
  cout << "---------------------------------------------" << endl;

  return;
}

/**
 * @brief Parse info from a loaded SID file
 * @note MicroSID player function
 */
void parse_sid_info(void)
{
#if DESKTOP
  is_rsid = (sidfile_->GetSidType() == "RSID");

  sidflags = sidfile_->GetSidFlags();
  sidspeed = sidfile_->GetSongSpeed(songno); // + 1);
  curr_sidspeed = (sidspeed & (1 << songno)); // ? 1 : 0;  // 1 ~ 60Hz, 2 ~ 50Hz
  ct = sidfile_->GetChipType(1);
  cs = sidfile_->GetClockSpeed();
  sv = sidfile_->GetSidVersion();
  // 2 2 3 ~ Genius
  // 2 1 3 ~ Jump
  // MOSDBG("%d %d %d\n", ct, cs, sv);
  clock_speed = clockSpeed[cs];
  raster_lines = scanLines[cs];
  rasterrow_cycles = scanlinesCycles[cs];
  frame_cycles = (raster_lines * rasterrow_cycles);
  refresh_rate = refreshRate[cs];
#elif EMBEDDED
  /* TODO */
#endif
  return;
}

#if DESKTOP
/**
 * @brief Process a SID file by filename
 * @note MicroSID player function
 * @param fname
 * @return true
 * @return false
 */
bool process_sid_file(string fname)
#elif EMBEDDED
/**
 * @brief Process a SID file from a pointer by size
 * @note MicroSID player function
 * @param binary_
 * @param binsize_
 * @return true
 * @return false
 */
bool process_sid_file(uint8_t * binary_, size_t binsize_)
#endif
{
  sidfile_ = new SidFile();
#if DESKTOP
  filename = fname;
  int res = (havefile ? sidfile_->Parse(filename) : -1);
#elif EMBEDDED
  int res = (havefile ? sidfile_->ParsePtr(binary_, binsize_) : -1);
#endif

  if (havefile) {
#if DESKTOP
    if (res != 0) {
      MOSDBG("SID file '");
      cout << fname;
      MOSDBG("' not found, exiting!\n");
      goto FAIL;
    }
#endif
    parse_sid_info();
    print_sid_info();
  } else {
    MOSDBG("No SID file supplied, exiting!\n");
    goto FAIL;
  }

  if (songno < 0 or songno >= sidfile_->GetNumOfSongs()) {
    cout << "Warning: Invalid Sub-Song Number. Default Sub-Song will be chosen." << endl;
    songno = sidfile_->GetFirstSong();
  }

  return true;
FAIL:
  delete sidfile_;
  return false;
}

/**
 * @brief Start MicroSID player
 * @note MicroSID player function
 */
void start_player(void)
{
  // if (Cpu) Cpu->set_cycle_callback(cycle_callback); /* Disabled in favor of internal callback */
  MOSDBG("[SID] %s\n", (is_rsid ? "RSID" : "PSID"));

  set_sid_addresses();

#if DESKTOP
  if (usbsid) {
    getinfo_USBSID(clock_speed);
    /* USBSID related variables and defaults */
    /* TODO: Implement use */
    SID->fmoplsidno = fmoplsidno;
    SID->sidssockone = sidssockone;
    SID->sidssocktwo = sidssocktwo;
    SID->sockonesidone = sockonesidone;
    SID->sockonesidtwo = sockonesidtwo;
    SID->socktwosidone = socktwosidone;
    SID->socktwosidtwo = socktwosidtwo;
    SID->forcesockettwo = forcesockettwo; /* TODO: Needs a commandline function */
  } else {
    SID->fmoplsidno = 0; /* Disabled */
    SID->sidssockone = ((sidcount >= 2) ? 2 : 1);
    SID->sidssocktwo = ((sidcount >= 4) ? 2 : (sidcount == 3) ? 1 : 0);
    SID->sockonesidone = 0;
    SID->sockonesidtwo = 0;
    SID->socktwosidone = 0;
    SID->socktwosidtwo = 0;
    SID->forcesockettwo = forcesockettwo; /* TODO: Needs a commandline function */
  }
#elif EMBEDDED
 {/* TODO: SETUP USBSID FROM HERE */
    SID->fmoplsidno = 0; /* Disabled */
    SID->sidssockone = ((sidcount >= 2) ? 2 : 1);
    SID->sidssocktwo = ((sidcount >= 4) ? 2 : (sidcount == 3) ? 1 : 0);
    SID->sockonesidone = 0;
    SID->sockonesidtwo = 0;
    SID->socktwosidone = 0;
    SID->socktwosidtwo = 0;
    SID->forcesockettwo = forcesockettwo; /* TODO: Needs a commandline function */
 }
#endif


  play_rate = 0;
  if (curr_sidspeed == 1) { /* NOTE: Should be ignored for RSID tunes! */
    /* if (is_rsid || force_rsiddrv) */ MOSDBG("[SID] PLAY_RATE: %u [CIA1] %4x [RSID or FORCED]\n", play_rate, Cia1->ta_prescaler());
    // else MOSDBG("[SID] PLAY_RATE: %u [CIA1] %4x\n", play_rate, (emu_dma_read_ram(CIA1_TIMER_HI) << 8 | emu_dma_read_ram(CIA1_TIMER_LO)));
    // Cpu->emulate_n(100);
    // play_rate = ((is_rsid || force_rsiddrv) ? Cia1->ta_prescaler() : (emu_dma_read_ram(CIA1_TIMER_HI) << 8 | emu_dma_read_ram(CIA1_TIMER_LO)));  /* CIA timing */
    play_rate = Cia1->ta_prescaler();
    play_rate = play_rate == 0 ? refresh_rate : play_rate; /* Fallback */
    /* if (is_rsid || force_rsiddrv) */ MOSDBG("[SID] PLAY_RATE: %u [CIA1] %4x [RSID or FORCED]\n", play_rate, Cia1->ta_prescaler());
    // else MOSDBG("[SID] PLAY_RATE: %u [CIA1] %4x\n", play_rate, (emu_dma_read_ram(CIA1_TIMER_HI) << 8 | emu_dma_read_ram(CIA1_TIMER_LO)));
  } else {
    play_rate = refresh_rate; /* V-Blank */
  }
  if (play_rate >= 20000) play_rate = 19656;
  /* if (is_rsid || force_rsiddrv) */ MOSDBG("[SID] PLAY_RATE: %u [CIA1] %4x [RSID or FORCED]\n", play_rate, Cia1->ta_prescaler());
  // else MOSDBG("[SID] PLAY_RATE: %u [CIA1] %4x\n", play_rate, (emu_dma_read_ram(CIA1_TIMER_HI) << 8 | emu_dma_read_ram(CIA1_TIMER_LO)));

  /* SID */
  SID->sidcount = sidcount; /* Default is 1 */
  SID->sidno    = sidno;    /* Default startnum */
  SID->sidone   = sidone;   /* Default */
  SID->sidtwo   = (sidcount >= 2 && sidtwo != 0x0000 ? sidtwo : 0x0000);
  SID->sidthree = (sidcount >= 2 && sidthree != 0x0000 ? sidtwo : 0x0000);
  SID->sidfour  = (sidcount >= 2 && sidfour != 0x0000 ? sidtwo : 0x0000);

  SID->print_settings();

  Vic->cycles_per_sec = clock_speed;
  Vic->refresh_frequency = (double)((double)clock_speed / (double)frame_cycles);
  Vic->refresh_rate = (double)play_rate;
  Vic->refresh_frequency = (double)((double)Vic->cycles_per_sec / (double)Vic->refresh_rate);
  Vic->raster_lines = raster_lines;
  Vic->raster_row_cycles = rasterrow_cycles;
  Vic->set_timer_speed(100);
#if DESKTOP
  usbsid->USBSID_SetClockRate(clock_speed, true);
#elif EMBEDDED
  /* TODO: Do something */
#endif

  /* MICROSID player */
  load_microsid_player(songno);
  MOSDBG("[USPLAYER] loaded\n");

PLAY:
#if DESKTOP
  delete sidfile_;
#endif
    if (log_instructions) Cpu->loginstructions = true;
    log_logs();

    MOSDBG("[VIC] RL:%u RRC:%u\n",Vic->raster_lines,Vic->raster_row_cycles);

    MOSDBG("[emulate_c64]\n");
    emulate_c64();

  END:
  return;
}

#pragma GCC diagnostic pop
