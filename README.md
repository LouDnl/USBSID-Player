# USBSID-Player
USBSID-Player is a Cycle Exact Commodore64 emulating PSID/RSID & PRG tune player for command line, websites and embedding that is aimed for use with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) & Pico2/Pico2W (RP2350) based board for interfacing one or two MOS SID chips and/or hardware SID emulators over (WEB)USB with your computer, phone or ASID supporting player.


# Features
- Cycle exact C64 emulation (CPU, CIA1/2, VIC, and up to three SID chips), the same emulation core behind every target below
- Plays PSID, RSID and PRG/P00, tunes skip the boot and start in milliseconds, programs boot to BASIC and RUN/SYS themselves
- Three interchangeable output backends behind one seam (`SidBackend`): a real USBSID-Pico over USB, software audio (ResidFp) via miniaudio, or a WAV file
- FM/OPL playback via Nuked-OPL3, for tunes that use a board's FM/OPL chip alongside its SID chips
- HVSC Songlengths support, stops a tune when the song ends instead of playing on forever
- Muting and solo by chip and voice, subtune switching, PAL/NTSC forcing, and a `--trace` mode that records every SID register event to a file
- Per subsystem logging switches (SID/CIA/VIC/CPU reads and writes, banking, instructions, timers) carried over from the old player
- Embeds directly into the USBSID-Pico firmware (`ONBOARD_SIDPLAYER=1`, RP2350 only, needs `-O3`), a drop in replacement for the player it succeeds
- Compiles to WebAssembly for the browser, driving a board over WebUSB (cycle exact) or ASID over Web MIDI (a snapshot per frame), with an AudioWorklet worker so playback survives a backgrounded tab


# Usage
```shell
usage: usbsid [options] <file.sid|file.prg|file.p00>

  a SID file plays; a program is loaded where it says and started the
  way you would start it, with RUN or with SYS

  -s, --song N      start at subtune N (default: the tune's own)
  -t, --seconds N   stop after N seconds (default: play until ctrl-c)
  -i, --info        print what the file says and exit
  -n, --no-device   run without hardware, useful for checking a tune

  sound:
      --output M    usbsid (default), audio, or wav. usbsid falls back to
                    audio when no board is found
      --wav FILE    write a WAV instead of playing, implies --output=wav
      --rate N      sample rate for audio and wav (default 44100). A device
                    may impose its own, which is then what is used
      --quality Q   fast (linear) or good (sinc, default)
  -T, --trace FILE  write every SID register event to FILE. Records what
                    is played, so it works with a board and with --wav;
                    add -n for a silent run that only records
      --mute SPEC   silence voices. SPEC is a comma separated list of
                    CHIP:VOICE or CHIP for all three, chips and voices
                    counting from 1, for example 1:3 or 2 or 1:1,1:2
      --solo SPEC   the other way round: silence everything except SPEC.
                    --solo 1:2 --wav v2.wav records voice two on its own
      --pal         force PAL timing
      --ntsc        force NTSC timing

  hardware:
  -rr               read the SID back from the chip, not the mirror
  -f                force everything into socket two
  -fa XX            force everything to physical base $XX (hex)
      --overhead N  cycles one hardware access costs (default 1)
      --songlengths F  HVSC Songlengths database, to stop when the song ends.
                    Found by itself in $SONGLENGTHS, ~/Songlengths.md5,
                    $HVSCROOT or $HVSC_BASE DOCUMENTS/Songlengths.md5, or $HVSCDB.
      --no-songlengths  ignore it even when one is found

  logging, to stdout, same switches as old player:
  -srw              SID reads and writes
  -c1rw / -c2rw     CIA1 / CIA2 reads and writes
  -vrw / -vrrw      VIC register writes / reads
  -lrw              every CPU read and write
  -llrw             reads that come out of a ROM
  -pla              banking changes
  -ins              every instruction
  -tim              the timers, once a frame
  -lmem             the SID registers, once a frame

  -h, --help        this
```


# How to

## Build the command line player
```bash
./build.sh cli
./usbsid --song 1 tune.sid          # play from a subtune
./usbsid --output wav --wav out.wav tune.sid
./usbsid -i tune.sid                # print the tune's own info and exit
./usbsid -h                         # the full option list
```
Needs `libusb-1.0` and, on Linux, `libudev` for USB access, both found via `pkg-config`.

## Build the WebAssembly player
Needs the Emscripten SDK (built and tested with emcc 4.0.12):
```bash
source /path/to/emsdk/emsdk_env.sh
./build.sh web
python3 -m http.server 8000
# http://localhost:8000/web/demo.html
```
A secure context is required: WebUSB and Web MIDI only exist on `https://` or `http://localhost`. See [docs/WEB.md](docs/WEB.md) for the JavaScript API, the transports, and how to embed the player in another page.

## Embed into the firmware
The firmware pulls this repository in via `cmake/usbsid-player.cmake` and builds with `ONBOARD_SIDPLAYER=1`. The API is `#include <usplayer.h>`:
```c
load_sidtune(sidfile, sidfile_size, tuneno);
init_sidplayer();
start_sidplayer(false);
while (playing) loop_sidplayer();   /* one frame per call */
stop_sidplayer();
```
See [docs/EMBEDDED.md](docs/EMBEDDED.md) for the full API, what it costs in flash and RAM, and how to check it on the desktop before flashing.

## Run the test suite
```bash
./build.sh test
```

## Other build.sh targets
```bash
./build.sh all      # cli + web
./build.sh install  # copy web/ into the config tool's usplayer/
./build.sh clean    # remove build/ and build-web/
./build.sh          # full target list, including the site/deepsid deploy targets
```


# USBSID-Player is currently used/implemented in
Embedded player:  
- [USBSID-Pico firmware](https://github.com/LouDnl/USBSID-Pico) ~ supported by boards with a Pico2  

Web player:  
- [USBSID-Pico Web configtool](https://usbsid.loudai.nl/)
- [HippoPlayer](https://hippoplayer.se) ~ a webbased Amiga emulator
- [Deepsid](https://deepsid.chordian.net)  

CLI player:  
- [USBSID-Configtool](https://github.com/LouDnl/USBSID-Configtool) ~ in active development

# Libraries used for non USBSID audio play (CLI & Web)
- [ResidFp](https://github.com/libsidplayfp/libresidfp)
- [miniaudio](https://github.com/mackron/miniaudio)
- [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3)


# Disclaimer
The workings of this player are heavily inspired by and based upon the following fantastic projects:
- [SidBerry](https://github.com/LouDnl/SidBerry) (USBSID version)
  * Original by [gianlucag](https://github.com/gianlucag)
  * USBSID version by [LouDnl](https://github.com/LouDnl)
- [emudore-embedded](https://github.com/LouDnl/emudore-embedded) (USBSID version)
  * Original by [marioballano](https://github.com/marioballano)
  * USBSID version by [LouDnl](https://github.com/LouDnl)
- [Vice](https://github.com/VICE-Team/svn-mirror) by [VICE-Team](https://github.com/VICE-Team)
- [libsidplayfp](https://github.com/libsidplayfp/libsidplayfp)
  * Maintained by [drfiemost](https://github.com/drfiemost)
- [Jsidplay2](https://sourceforge.net/projects/jsidplay2/)
  * Maintained by [kenchis](https://haendel.ddns.net:8443/)
- [Denise](https://sourceforge.net/projects/deniseemu/) by [piciji](https://sourceforge.net/u/piciji/profile/)
- [WebSID](https://bitbucket.org/wothke/websid/src/master/) by [wothke](https://www.wothke.ch/)
- And many more!!
