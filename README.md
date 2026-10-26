# USBSID-Player
USBSID-Player is a Cycle Exact Commodore64 emulating PSID/RSID & PRG tune player for command line, websites and embedding that is aimed for use with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) & Pico2/Pico2W (RP2350) based board for interfacing one or two MOS SID chips and/or hardware SID emulators over (WEB)USB with your computer, phone or ASID supporting player.


# Features
- Cycle exact C64 emulation (CPU, CIA1/2, VIC and up to 15 SID chips, the `multiSidConfig` limit of a v5 tune), the same core for every target below
- Plays PSID, RSID and PRG/P00 (tunes skip the boot, programs boot to BASIC and RUN/SYS themselves)
- Four output backends (`SidBackend`): a real USBSID-Pico over USB, software audio (ResidFp) via miniaudio, a WAV file or a Network SID Device server (v4/v5) over TCP
- Software audio and netdevice play up to 15 SID chips, a real board (USB, WebUSB or embedded) plays the first 4 sockets
- `--stereo` pans a multi-SID tune per the v5 panning hint (software audio and WAV only, off by default)
- FM/OPL playback via Nuked-OPL3-fast for tunes that use the FM/OPL chip of a board
- Netdevice sends FM OPL as NSD v5 `TRY_WRITE_EX` writes to `$DF40` (address) and `$DF50` (data), `TRY_SET_FM_OPL` enables/disables FM OPL on the server
- HVSC Songlengths support, a tune stops when the song ends (a v5 tune uses its embedded song length table when the database has no entry)
- Muting and solo by chip and voice, subtune switching, PAL/NTSC forcing and a `--trace` mode that records SID register events to a file
- Logging switches per subsystem (SID/CIA/VIC/CPU reads and writes, banking, instructions, timers) like the old player
- Embeds into the USBSID-Pico firmware (`ONBOARD_SIDPLAYER=1`, RP2350 only, needs `-O3`), a drop in replacement for the old player
- Compiles to WebAssembly for the browser, drives a board over WebUSB (cycle exact) or ASID over Web MIDI (a snapshot per frame), the AudioWorklet worker keeps playing in a background tab


# Usage
Output `nsd` and `--net-*` options are only available on Linux and MacOs.
```shell
usage: usplayer [options] <file.sid|file.prg|file.p00>

  a SID file plays; a program is loaded where it says and started the
  way you would start it, with RUN or with SYS

  -s,  --song N     start at subtune N (default: the tune's own)
  -t,  --seconds N  stop after N seconds (default: play until ctrl-c)
  -i,  --info       print what the file says and exit
  -n,  --no-device  run without hardware, useful for checking a tune
  --no-roms         run on the stub KERNAL, without BASIC and character
                    ROM, the way a build without ROMs does

  sound:
  -o,  --output M   usbsid (default), asid, nsd, audio or wav.
                    usplayer falls back to audio when no board is found
  -lb, --list-boards
                    list every attached board's serial number and exit
  -b,  --boards SPEC
                    open these boards for --output=usbsid instead of
                    the one the driver picks, in this order (board 1
                    is the first, etc.). SPEC is a comma separated list
                    of serial numbers from --list-boards, for example
                    AB12,CD34 opens AB12 as board 1 and CD34 as board 2
                    regardless of USB-connect order, and leaves any
                    other attached board untouched. The tune's SIDs are
                    spread across every opened board's SIDs, board 1's
                    first. (default: one board, the first in USB
                    bus/port order)
  -lm, --list-midi  list every available midi output device
  -a,  --asid N     Midi supporting ASID port to use for --output=asid
                    (default is 0)
  -nh, --net-host H Network SID Device server to connect to for
                    --output=nsd (default 127.0.0.1)
  -np, --net-port P its TCP port (default 6581)
  -ns, --net-sids N SIDs to tell it about (default: the tune's own count)
  -w,  --wav FILE   write a WAV instead of playing, implies --output=wav
  -ra, --rate N     sample rate for audio and wav (default 44100). A device
                    may impose its own, which is then what is used
  -q,  --quality Q  fast (linear) or good (sinc, default)
  -st, --stereo     pan multi-SID tunes per the v5 file's own hint
                    (--output=audio/wav only; off by default, one channel)
  -rv, --resid-volume N
                    reSIDfp (SID) output level, percent, 0-300
                    (--output=audio/wav only; default 100)
  -fv, --fmopl-volume N
                    FM/OPL output level, percent, 0-300 (same scope;
                    default 50 - the OPL is the louder of the two chips)
  -T,  --trace FILE write every SID register event to FILE, laid out like
                    -srw (chip, C64 address, register:value, [C] cycle
                    delta) plus the running cycle total and the play
                    time MM:SS.mmm. Records what is played, also works
                    with a board and with --wav; add -n for a silent
                    run that only records
  -m,  --mute SPEC  silence voices (gate/sustain forced off; everything
                    else for that voice still reaches the backend). SPEC
                    is a comma separated list of CHIP:VOICE or CHIP for
                    all three, chips and voices counting from 1, for
                    example 1:3 or 2 or 1:1,1:2
  -S,  --solo SPEC  the other way round, and harder: only SPEC's writes
                    reach the backend, everything else is dropped before
                    it gets there (and before -srw sees it), not just
                    silenced. --solo 1:2 --wav v2.wav records voice two
                    on its own with no other chip/voice traffic
  -ms, --mute-solo SPEC
                    the old --solo: silence everything except SPEC
                    the soft way, same as --mute. Kept under its own name
                    for anyone relying on that behavior specifically
  -P,  --pal        force PAL timing
  -N,  --ntsc       force NTSC timing

  hardware:
  -rr               read the SID back from the chip, not the mirror
  -f                force everything into socket two
  -fa XX            force everything to physical base $XX (hex)
  -ss, --select-sids SPEC
                    play only these of the tune's SIDs, on the
                    board's sockets in the order given. SPEC is a comma
                    separated list of tune SID numbers counting from 1,
                    each optionally followed by :SLOT to name the exact
                    socket it lands on, also counting from 1. For
                    example 3,4,5 puts the tune's 3rd SID on the board's
                    first socket, its 4th on the second, and its 5th on
                    the third; 3:1,5:4 instead puts the 3rd SID on the
                    first socket and the 5th on the fourth, leaving the
                    second and third empty. A bare entry claims the
                    lowest socket no :SLOT entry already claimed. At
                    most 4 sockets are used on one board, or every
                    SID of the boards --boards opened. fm or fm:SLOT
                    puts the tune's FM/OPL on that socket, only when
                    it is the board's configured FM/OPL; with SPEC
                    given, an FM/OPL not named this way is not played
                    (--output=usbsid only; default: the tune's first N
                    SIDs on the first N sockets, in order)
  -oh, --overhead N Cycles one hardware access costs (default 1; 0 for
                    netdevice software servers)
  -sl, --songlengths F
                    HVSC Songlengths database, to stop when the song
                    ends. Found by itself in $SONGLENGTHS,
                    ~/Songlengths.md5, $HVSCROOT or $HVSC_BASE
                    DOCUMENTS/Songlengths.md5, or $HVSCDB.
  -nsl, --no-songlengths
                    ignore it even when one is found

  logging, to stdout, same switches as old player:
  -srw              SID reads and writes
  -sidr / -sidw     SID reads or SID writes only
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
Linux example.
```bash
sudo apt install libusb-1.0-0-dev libudev-dev

git clone https://github.com/LouDnl/USBSID-player && \
  git clone https://github.com/LouDnl/USBSID-Pico-driver USBSID-player/lib/driver

cd USBSID-Player

cmake -S . -B build_cli \
  -DDESKTOP=1 -DEMBEDDED=0 -DWEB=0 -DREQUIRE_AUDIO=1 && \
	cmake --build build_cli --parallel $(nproc) && \
  cp build_cli/usplayer .
```
## Usage examples for the command line player
Linux example.
```bash
./usplayer --song 1 tune.sid          # play from a subtune
./usplayer --output wav --wav out.wav tune.sid
./usplayer --output nsd --net-host 192.168.1.42 tune.sid  # play over a Network SID Device server
./usplayer -i tune.sid                # print the tune's own info and exit
./usplayer -h                         # the full option list
```
Needs `libusb-1.0` + `libusb-1.0-0-dev` and on Linux `libudev` + `libudev-dev` for USB access, found via `pkg-config`. `-dev` is for building, the other for running.

## Build the WebAssembly player
Linux example.  
Needs the Emscripten SDK (built and tested with emcc 4.0.12):
```bash
source /path/to/emsdk/emsdk_env.sh

git clone https://github.com/LouDnl/USBSID-player

cd USBSID-Player

emcmake cmake -S . -B build_web -DWEB=1 && \
	cmake --build build_web --parallel $(nproc)

cd web

python3 -m http.server 8000
# http://localhost:8000/web/demo.html
```
WebUSB and Web MIDI need a secure context (`https://` or `http://localhost`), see [docs/API_WEB.md](docs/API_WEB.md) for the JavaScript API, the transports and how to embed the player in another page.

## Embed into the firmware
The firmware pulls this repository in via `cmake/usbsid-player.cmake` and builds with `ONBOARD_SIDPLAYER=1`. Use the API with `#include <usplayer.h>`:
```c
load_sidtune(sidfile, sidfile_size, tuneno);
init_sidplayer();
start_sidplayer(false);
while (playing) loop_sidplayer();   /* one frame per call */
stop_sidplayer();
```
See [docs/API_EMBEDDED.md](docs/API_EMBEDDED.md) for the full API.

## Run the test suite
The suite is part of the command line build (`BUILD_TESTS` defaults to 1), use the same build directory:
```bash
cmake -S . -B build_cli \
  -DDESKTOP=1 -DEMBEDDED=0 -DWEB=0 -DBUILD_TESTS=1 && \
	cmake --build build_cli --target usplayer-test --parallel $(nproc) && \
  ./build_cli/usplayer-test
```
Every component has a `test_<name>` executable registered with CTest, run them all with `ctest --test-dir build_cli` after a full `cmake --build build_cli`.

The `asid` suite (`tests/test_asid.cpp`, run with `ctest -R asid` or `./build.sh test` for all) checks the ASID SysEx messages against stub RtMidi symbols, no MIDI port needed. It is built when RtMidi is available, else it reports `[SKIP]`.

## Build the command line player with ymfm
Uses [ymfm](https://github.com/aaronsgiles/ymfm) as FM/OPL engine. Command line player only (CMake refuses `FM_YMFM=1` with `WEB=1` or `EMBEDDED=1`), `EXECUTABLE` names the binary, the regular `usplayer` stays untouched:
```bash
cmake -S . -B build_ymfm \
  -DDESKTOP=1 -DEMBEDDED=0 -DWEB=0 -DREQUIRE_AUDIO=1 \
  -DFM_YMFM=1 -DEXECUTABLE=usplayer-ymfm && \
	cmake --build build_ymfm --parallel $(nproc) && \
  cp build_ymfm/usplayer-ymfm .
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
- [Nuked-OPL3-fast](https://github.com/tgies/Nuked-OPL3-fast), a bit-exact fork of [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3)
- [ymfm](https://github.com/aaronsgiles/ymfm), optional FM/OPL engine for the `cli-ymfm` build (CLI only)


# License
USBSID-Player is licensed under the GNU General Public License, version 2 or (at your
option) any later version (`GPL-2.0-or-later`), see [LICENSE](LICENSE). Vendored libraries in
`lib/` keep their own licences.


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
