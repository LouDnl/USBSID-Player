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
 * main.cpp
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

using namespace std;

/* Declare external functions */

/* Emulation */
extern void emu_init(void);
extern void emu_deinit(void);
extern void hardwaresid_init(void);
extern void hardwaresid_deinit(void);
extern bool process_sid_file(string fname);
extern void run_prg(string fname);
extern void start_player(void);
extern void start_test(void);

/* New smaller sidfile reader */
extern void readfile(const char * filename);

/* VSID Pmos6581_8580 */
extern void start_vsid_player(bool pal);
extern int psid_load_file(const char* filename, int subtune);
extern void psid_init_tune(int install_driver_hook);
extern void psid_init_driver(void);
extern void psid_shutdown(void);
extern bool is_pal;
char * filename;

/* External player variables */
extern volatile sig_atomic_t stop;
/* External emulation variables */
extern uint8_t songno;
extern bool
  force_psiddrv,
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

/* Local variables */
string fname;

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

void inthand(int signum)
{
  stop = 1;
}

void process_arguments(int argc, char **argv)
{
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
    else if (!strcmp(argv[param_count], "-f")) { /* Force PSIDDRV */
      forcesockettwo = true;
    }
    else if (!strcmp(argv[param_count], "-p")) { /* Force PSIDDRV */
      force_psiddrv = true;
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
  fprintf(stdout, "[ARGS] F:%d P:%d FP:%d S:%d CPU:%d L:%d%d%d%d%d%d%d%d%d\n",
    havefile,
    prgfile,
    force_psiddrv,
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
  signal(SIGINT, inthand);
  songno = -1;
  process_arguments(argc,argv);
  init();

  if (prgfile) {
    run_prg(fname);
  } else if (force_psiddrv && !psid_load_file(filename, (int)((songno != -1) ? (songno+1) : songno))) {
    psid_init_driver();
    psid_init_tune(1);
    printf("[MAIN] is_pal: %d\n",is_pal);
    start_vsid_player(is_pal);
    psid_shutdown();
  } else if (process_sid_file(fname)) {
    start_player();
  }

  deinit();

  return 0;
}
