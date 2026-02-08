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
#include <algorithm> // Required for std::transform
#include <cctype>    // Required for ::tolower

#include <sys/time.h>
#include <signal.h>
#include <stdio.h>

#ifdef DESKTOP
#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif /* _WIN32 */
#endif /* DESKTOP */

#include <c64util.h>
#include <wrappers.h>

using namespace std;

/* Declare external functions */

/* Emulation */
extern void emu_init(void);
extern void emu_deinit(void);
extern void emu_next_subtune(void);
extern void emu_previous_subtune(void);
extern void emu_pause_playing(bool pause);
extern void emulate_c64_single(void);
extern void hardwaresid_init(void);
extern void hardwaresid_deinit(void);
#if DESKTOP
extern bool process_sid_file(string fname);
extern void run_prg(string fname, bool loop);
extern void start_player(void);
extern void start_test(void);
/* New smaller sidfile reader */
extern void readfile(const char * filename);
/* VSID Pmos6581_8580 */
extern int psid_load_file(const char* filename, int subtune);
#elif EMBEDDED
extern int psid_load_file(uint8_t * binary_, size_t binsize_, int subtune);
extern void run_prg(uint8_t * binary_, size_t binsize_, bool loop);
#endif
extern void psid_init_tune(int install_driver_hook);
extern void psid_init_driver(void);
extern void psid_shutdown(void);
extern void start_vsid_player(bool is_pal, bool loop);
extern bool is_pal;
char * filename;
bool from_stdin = false;
bool force_microsidplayer = false;
bool threaded = true;

/* External emulation variables */
#if DESKTOP
extern volatile sig_atomic_t stop;
extern volatile sig_atomic_t playing;
extern volatile sig_atomic_t vsidpsid;
#elif EMBEDDED
extern volatile bool stop;
extern volatile bool playing;
extern volatile bool vsidpsid;
#endif
extern uint8_t songno;
extern bool
  forcesockettwo,
  is_rsid,
  havefile,
  prgfile;

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
pthread_t usplayer_ptid;
static pthread_mutex_t usplayer_mutex;
#endif

void init(void)
{
  emu_init();
  hardwaresid_init();

  return;
}

void deinit(void)
{
  emu_deinit();
  hardwaresid_deinit();

  return;
}

#if DESKTOP
void inthand(int signum)
{
  stop = 1;
  playing = false;
}

void run_player(void);

/**
 * @brief Emulation thread for DESKTOP
 *
 * @param arg
 * @return void*
 */
void* Emulation_Thread(void* arg)
{
  (void)arg;
  MOSDBG("[EMU] Thread starting\r\n");
  #ifdef _GNU_SOURCE
  pthread_setname_np(pthread_self(), "Emulation thread");
  #endif
  pthread_detach(pthread_self());
  MOSDBG("[EMU] Thread detached\r\n");
  pthread_mutex_lock(&usplayer_mutex);
  run_player();
  MOSDBG("[EMU] Thread finished\r\n");
  playing = false;
  pthread_mutex_unlock(&usplayer_mutex);
  pthread_exit(NULL);
  return NULL;
}

void run_player(void)
{
  if (prgfile) {
    vsidpsid = false;
    run_prg(fname, true);
    goto END;
  }
  if (!force_microsidplayer && psid_load_file(filename, (int)((songno != -1) ? (songno+1) : songno))) {
    vsidpsid = true;
    psid_init_driver();
    psid_init_tune(1); /* 1 to install driver hook */
    MOSDBG("[USPLAYER] is_pal: %d\n",is_pal);
    psid_shutdown();
    start_vsid_player(is_pal, true);
    goto END;
  }
  if (force_microsidplayer && process_sid_file(fname)) {
    vsidpsid = false;
    start_player();
    goto END;
  }
END:
  deinit();
}

/**
 * @brief Check for key press multiplatform
 *
 * @return boolean key pressed
 */
bool check_keyboard() {
#ifdef _WIN32
  return _kbhit() != 0;
#else
  struct termios oldt, newt;
  int ch;
  int oldf;

  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

  ch = getchar();

  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);

  if (ch != EOF) {
      ungetc(ch, stdin);
      return true;
  }
  return false;
#endif
}

/**
 * @brief Return the pressed key multiplatform
 *
 * @return integer pressed key
 */
int getch_noblock() {
#ifdef _WIN32
    return _getch();
#else
    return getchar();
#endif
}


/**
 * @brief Wait for input function with courtesy of Hermit's CrSID
 *
 */
void wait_for_input(void)
{
#if defined(_WIN32) || defined(__CYGWIN__)
  /* Windows scancodes (usually following a 0 or 0xE0 prefix) */
  enum cursor_keys { KEY_UP=0x48, KEY_DOWN=0x50, KEY_LEFT=0x4B, KEY_RIGHT=0x4D };
#elif defined(__APPLE__) && defined(__MACH__)
  /* macOS / Darwin specific codes (often used in Carbon/Cocoa or specific frameworks) */
  enum cursor_keys { KEY_UP=273, KEY_DOWN=274, KEY_LEFT=276, KEY_RIGHT=275 };
#elif defined(__linux__) || defined(__unix__) || defined(__unix)
  /* Unix/Linux: These are the suffix bytes for ANSI escape sequences (\033[A, etc.) */
  enum cursor_keys { KEY_UP=0x41, KEY_DOWN=0x42, KEY_RIGHT=0x43, KEY_LEFT=0x44 };
#else
  #error "Unknown Platform"
#endif

  char pressed_key_char = 0;
  bool skip_capture = false;
  bool paused = false;

  MOSDBG("[USPLAYER] Waiting for input\n");

  while(playing) {
    if (!skip_capture) {
      if (check_keyboard()) {
        int ch = (char)getch_noblock();
        pressed_key_char = (char)ch;
        if(/*pressed_key_char==27 ||*/ pressed_key_char=='\n' || pressed_key_char=='\r') {
          std::cout << "\rKEY_STOP       \n" << std::flush;
          emu_pause_playing(false);
          stop=true;
          skip_capture=true;
        } else if (pressed_key_char=='p') {
          paused = !paused;
          std::cout << "\rKEY_PAUSE       " << (int)paused << std::flush;
          emu_pause_playing(paused);
        } else if (pressed_key_char==0x09 || pressed_key_char=='`') {
        } else if ('1'<=pressed_key_char &&  pressed_key_char<='9') {
        } else if (pressed_key_char==KEY_RIGHT) {
          std::cout << "\rKEY_RIGHT      \n" << std::flush;
          emu_pause_playing(true);
          emu_next_subtune();
          emu_pause_playing(false);
        } else if (pressed_key_char==KEY_LEFT) {
          std::cout << "\rKEY_LEFT       \n" << std::flush;
          emu_pause_playing(true);
          emu_previous_subtune();
          emu_pause_playing(false);
        } else if (pressed_key_char==KEY_UP) {
          std::cout << "\rKEY_UP         " << std::flush;
        } else if (pressed_key_char==KEY_DOWN) {
          std::cout << "\rKEY_DOWN       " << std::flush;
        } else {
          std::cout << "\rKEY: " << (char)ch << "   " << std::flush;
        }
        // emu_sleep_us(5000);
      }
    }
  }
  return;
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
      vsidpsid = false;
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
    else if (!strcmp(argv[param_count], "-t")) { /* disable threading */
      threaded = false;
    }
  }
  MOSDBG("[USPLAYER ARGS] FILE:%d PRG:%d FORCEMICROSID:%d FORCESOCK2:%d SONGO:%d CPU:%d L:%d%d%d%d%d%d%d%d%d\n",
    havefile,
    prgfile,
    force_microsidplayer,
    forcesockettwo,
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
  MOSDBG("[USPLAYER MAIN] FILE:%d PRG:%d FORCEMICROSID:%d SONGO:%d CPU:%d L:%d%d%d%d%d%d%d%d%d\n",
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

  if (threaded) {
    int error;
    error = pthread_create(&usplayer_ptid, NULL, &Emulation_Thread, NULL);
    if (error != 0) {
      MOSDBG("[USPLAYER] Thread can't be created :[%s]\n", strerror(error));
    } else {
      MOSDBG("[USPLAYER] Thread created\n");
    }
    wait_for_input();
  } else {
    run_player();
  }
  exit(1);
}

#elif EMBEDDED
extern "C" void load_prg(uint8_t * binary_, size_t binsize_, bool loop)
{
  MOSDBG("[USPLAYER] load_prg, size: %u\n", binsize_);
  stop = false; /* Always init to false on sidtune load */
  init();
  vsidpsid = false;
  run_prg(binary_, binsize_, loop);
  /* Intermission, thread will halt here until stopped */
  if (loop && stop) { /* If looping the thread comes back here so we need to stop the emulator here */
    emu_sleep_ms(100); /* Allow for player to stop */
    deinit();
  }
  return;
}

extern "C" void load_sidtune(uint8_t * sidfile, int sidfilesize, char subt)
{
  MOSDBG("[USPLAYER] load_sidtune, size: %u\n", sidfilesize);
  stop = false; /* Always init to false on sidtune load */
  init();
  vsidpsid = true;
  songno = ((int)subt == 0 ? -1 : (int)subt);
  MOSDBG("[USPLAYER] psid_load_file %d\n",songno);
  psid_load_file(sidfile,sidfilesize,(int)((songno != -1) ? (songno+1) : songno));
  return;
}

extern "C" void init_sidplayer(void)
{
  MOSDBG("[USPLAYER] psid_init_driver\n");
  psid_init_driver();
  MOSDBG("[USPLAYER] psid_init_tune\n");
  psid_init_tune(1); /* 1 to install driver hook */
  return;
}

extern "C" void start_sidplayer(bool loop)
{
  psid_shutdown();
  MOSDBG("[USPLAYER] start_vsid_player\n");
  MOSDBG("[USPLAYER] is_pal: %d\n",is_pal);
  start_vsid_player(is_pal, loop);
  /* Intermission, thread will halt here until stopped */
  if (loop && stop) { /* If looping the thread comes back here so we need to stop the emulator here */
    emu_sleep_ms(100); /* Allow for player to stop */
    deinit();
  }

  return;
}

extern "C" void loop_sidplayer(void)
{
  emulate_c64_single();
  return;
}

extern "C" bool stop_sidplayer(void)
{
  MOSDBG("[USPLAYER] stop_sidplayer\n");
  stop = true;

  deinit();

  return stop;
}

extern "C" void next_subtune(void)
{
  emu_next_subtune();

  return;
}

extern "C" void previous_subtune(void)
{
  emu_previous_subtune();

  return;
}

extern "C" int load_sidtune_fromflash(int sidflashid, char tuneno){ return 0; };
extern "C" void reset_sidplayer(void){};


#endif
