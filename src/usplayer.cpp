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
 * embedded.cpp
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

#include <cstring>
#include <bitset>
#include <sstream>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <ios>
#include <chrono>
#include <string>
#include <thread>

#include <sys/time.h>
#include <signal.h>
#include <stdio.h>

#include <c64util.h>

using namespace std;

/* Declare external functions */

/* Emulation */
extern void emu_init(void);
extern void emu_deinit(void);
extern void emu_next_subtune(void);
extern void emu_previous_subtune(void);
extern void hardwaresid_init(void);
extern void hardwaresid_deinit(void);
#if DESKTOP
extern bool process_sid_file(string fname);
extern void run_prg(string fname);
extern void start_player(void);
extern void start_test(void);

/* New smaller sidfile reader */
extern void readfile(const char * filename);
/* VSID Pmos6581_8580 */
extern int psid_load_file(const char* filename, int subtune);
#elif EMBEDDED
extern int psid_load_file(uint8_t * binary_, size_t binsize_, int subtune);
extern void run_prg(uint8_t * binary_, size_t binsize_);
#endif
extern void psid_init_tune(int install_driver_hook);
extern void psid_init_driver(void);
extern void psid_shutdown(void);
extern void start_vsid_player(bool pal);
extern bool is_pal;
char * filename;
bool force_microsidplayer = false;

#if EMBEDDED
#ifdef ANALYSE_STACKUSAGE
extern "C" {
#include "tool_heapusage.c"
extern void heapbefore(void);
extern void heapafter(void);
extern void getFreeStack(void);
}
#else
extern void heapbefore(void){};
extern void heapafter(void){};
extern void getFreeStack(void){};
#endif
#endif

/* External player variables */
#if DESKTOP
extern volatile sig_atomic_t stop;
#elif EMBEDDED
extern volatile bool stop;
extern volatile bool core2_init;
#endif
/* External emulation variables */
extern uint8_t songno;
extern bool
  forcesockettwo,
  is_rsid,
  havefile,
  prgfile,
  enable_r2;
extern bool
  log_instructions,
  log_timers,
  log_readwrites,
  log_romrw,
  log_sidrw,
  log_cia1rw,
  log_cia2rw,
  log_vicrw,
  log_vicrrw,
  log_pla;

#if DESKTOP
/* Local variables */
string fname;
#endif

void init(void)
{
  emu_init();
  hardwaresid_init();

  return;
}

void deinit(void)
{
  hardwaresid_deinit();
  emu_deinit();

  return;
}

#if DESKTOP
void inthand(int signum)
{
  stop = 1;
}

void process_arguments(int argc, char **argv)
{
  MOSDBG("[USPLAYER] Parse command line arguments\n");
  for (int param_count = 1; param_count < argc; param_count++) {
    if (fname.length() == 0 and argv[param_count][0] != '-') {
      filename = argv[param_count];
      // readfile(flname);
      fname = argv[param_count];
      size_t ext_i = fname.find_last_of(".");
      if(ext_i != std::string::npos) {
        std::string ext(fname.substr(ext_i+1));
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        if(ext == "sid") { prgfile = false; havefile = true; }
        else if(ext == "prg") { prgfile = true; havefile = true; }
        else if(ext == "p00") { prgfile = true; havefile = true; }
        else { prgfile = true; havefile = true; }
      } else { /* Assume prg */
        prgfile = true; havefile = true;
      }
    }
    else if (!strcmp(argv[param_count], "-f")) { /* Force tunes to socket two */
      forcesockettwo = true;
    }
    else if (!strcmp(argv[param_count], "-m")) { /* Force MicroSID player */
      force_microsidplayer = true;
    }
    else if (!strcmp(argv[param_count], "-s")) { /* Set subtune # */
      param_count++;
      songno = (atoi(argv[param_count]) - 1);
    }
    else if (!strcmp(argv[param_count], "-srw")) { /* log sid read/writes */
      log_sidrw = true;
    }
    else if (!strcmp(argv[param_count], "-c1rw")) { /* log cia1 read/writes */
      log_cia1rw = true;
    }
    else if (!strcmp(argv[param_count], "-c2rw")) { /* log cia2 read/writes */
      log_cia2rw = true;
    }
    else if (!strcmp(argv[param_count], "-vrw")) { /* log vic read/writes */
      log_vicrw = true;
    }
    else if (!strcmp(argv[param_count], "-vrrw")) { /* log vic read/writes */
      log_vicrrw = true;
    }
    else if (!strcmp(argv[param_count], "-lrw")) { /* log all read/writes */
      log_readwrites = true;
    }
    else if (!strcmp(argv[param_count], "-llrw")) { /* log rom reads */
      log_romrw = true;
    }
    else if (!strcmp(argv[param_count], "-pla")) { /* log all pla changes */
      log_pla = true;
    }
    else if (!strcmp(argv[param_count], "-ins")) { /* log all cpu instructions */
      log_instructions = true;
    }
    else if (!strcmp(argv[param_count], "-tim")) { /* log all timers */
      log_timers = true;
    }
    else if (!strcmp(argv[param_count], "-r2")) { /* enable r2 debugging support */
      enable_r2 = true;
    }
  }
  MOSDBG("[USPLAYER] FILE:%d PRG:%d FORCEMICROSID:%d SONGO:%d CPU:%d L:%d%d%d%d%d%d%d%d%d\n",
    havefile,
    prgfile,
    force_microsidplayer,
    songno,
    log_instructions,
    log_timers,
    log_pla,
    log_readwrites,
    log_romrw,
    log_vicrw,
    log_vicrrw,
    log_cia1rw,
    log_cia2rw,
    log_sidrw
  );
  return;
}

int main(int argc, char **argv)
{
  songno = -1;
  signal(SIGINT, inthand);
  process_arguments(argc,argv);
  init();

  MOSDBG("[USPLAYER] FILE:%d PRG:%d FORCEMICROSID:%d SONGO:%d CPU:%d L:%d%d%d%d%d%d%d%d%d\n",
    havefile,
    prgfile,
    force_microsidplayer,
    songno,
    log_instructions,
    log_timers,
    log_pla,
    log_readwrites,
    log_romrw,
    log_vicrw,
    log_vicrrw,
    log_cia1rw,
    log_cia2rw,
    log_sidrw
  );
  if (prgfile) {
    run_prg(fname);
    goto END;
  }
  if (!force_microsidplayer && psid_load_file(filename, (int)((songno != -1) ? (songno+1) : songno))) {
    psid_init_driver();
    psid_init_tune(1); /* 1 to install driver hook */
    MOSDBG("[USPLAYER] is_pal: %d\n",is_pal);
    psid_shutdown();
    start_vsid_player(is_pal);
    goto END;
  }
  if (force_microsidplayer && process_sid_file(fname)) {
    start_player();
    goto END;
  }
END:
  deinit();

  return 0;
}

#elif EMBEDDED
extern "C" {
bool sidplayer_init = false;
bool sidplayer_playing = false;
bool sidplayer_start = false;
}

extern "C" int load_sidtune(uint8_t * sidfile, int sidfilesize, char subt)
{
  (void) heapbefore();
  MOSDBG("[USPLAYER] load_sidtune\n");
  core2_init = true;
  stop = false; /* Always init to false on sidtune load */
  init();
  songno = ((int)subt == 0 ? -1 : (int)subt);
  MOSDBG("[USPLAYER] psid_load_file %d\n",songno);
  psid_load_file(sidfile,sidfilesize,(int)((songno != -1) ? (songno+1) : songno));
  (void) heapafter();
  return 0;
}

extern "C" void start_sidplayer(void)
{
  (void) heapbefore();
  MOSDBG("[USPLAYER] psid_init_driver\n");
  psid_init_driver();
  MOSDBG("[USPLAYER] psid_init_tune\n");
  psid_init_tune(1); /* 1 to install driver hook */
  (void) heapafter();
  return;
}

extern "C" unsigned int run_psidplayer(void)
{
  (void) heapbefore();
  MOSDBG("[USPLAYER] start_sidplayer\n");
  MOSDBG("[USPLAYER] is_pal: %d\n",is_pal);
  psid_shutdown();
  start_vsid_player(is_pal);
  (void) heapafter();
  return 0;
}

extern "C" void sleep_ms_emu(uint32_t ms);

extern "C" bool stop_emulator(void)
{
  (void) heapbefore();
  MOSDBG("[USPLAYER] stop_emulator\n");
  stop = true;
  sleep_ms_emu(100); /* Allow for player to stop */
  deinit();
  (void) heapafter();
  return 0;
}

extern "C" void next_subtune(void)
{
  emu_next_subtune();
};
extern "C" void previous_subtune(void)
{
  emu_previous_subtune();
};

extern "C" int load_sidtune_fromflash(int sidflashid, char tuneno){ return 0; };
extern "C" void reset_sidplayer(void){};


#endif
