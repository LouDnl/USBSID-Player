# Changelog
Please refer to the [releases page](https://github.com/LouDnl/USBSID-Player/releases) for more information on version changes

#### Version: 1.4.0
* Add multiboard playback: `--boards SERIAL,...` opens several USBSID-Pico
  boards and spreads a tune's SIDs across all of them, `--list-boards` lists
  the attached serials. `--output usbsid` without `--boards` still opens one
  board. Requires a USBSID-Pico-driver with `USBSID_Manager`
* Add FM/OPL to multiboard playback: without `-f`/`-fa`/`--select-sids`, an
  FM/OPL tune's FM goes to the first board in `--boards` order that has an
  FM/OPL configured, also when that is past the first board's four SIDs
* Add `fm`/`fm:SLOT` to `--select-sids`: puts the tune's FM/OPL on that slot,
  only when the slot is its board's configured FM/OPL. With `--select-sids`
  an FM/OPL not named this way is not played
* Fix `-f`/`-fa` moving only a tune's first SID while the rest stayed on
  socket one: every SID now moves along, and voice/chip mute replays and
  pause/resume follow the forced address too
* Add CMake option `FM_YMFM=1`: a command line build that uses ymfm
  (`lib/ymfm`, YM3812) instead of Nuked-OPL3-fast for FM/OPL, command line
  only; with `-DEXECUTABLE=usbsid-ymfm` it builds beside the regular `usbsid`
* Update `-T`/`--trace` to log like `-srw` (`[W SID1] $d400 $000:0f [C]   23`,
  reads included) plus the running cycle total and the play time of every
  event as `[T]MM:SS.mmm`
* Add PSID/RSID v5 support: variable length metadata strings, up to 15 SID
  chips via `multiSidConfig`, an SID panning hint, the FM/OPL flag, and song
  lengths embedded in the file itself as a fallback to the external
  Songlengths database
* Add a fourth output backend, `--output nsd`, a client of the Network
  SID Device protocol (Linux/MacOs only!): plays a tune through any compliant 
  server (USBSID-Pico acting as an NSD server included) over TCP instead of 
  local hardware or software SID
* Update the netdevice backend to protocol version 5: FM/OPL writes that have
  no local SID socket to piggyback on now go out over `TRY_WRITE_EX`'s own
  `$df00`-`$dfff` address range instead of being dropped, when the server
  supports it; add `TRY_SET_FM_OPL` (command 21), enabling/disabling FM OPL
  on the server itself
* Widen software audio (`--output audio`/`wav`) and the netdevice backend to
  the full 15 chips a v5 tune's own `multiSidConfig` can ask for, not just 4
  - `--output usbsid`/`webusb` and the embedded player still play the first 4,
  a real board's own physical socket limit
* Add `--stereo`: render a v5 tune's own panning hint as an actual stereo mix
  (`--output audio`/`wav` only, off by default - every other output stays one
  channel per chip, unaffected)
* Fix a v5 tune's own embedded song length table being ignored in favour of
  the five minute default just because the tune also was not in the external
  Songlengths database
* Add `docs/API_WEB.md` and `docs/API_EMBEDDED.md`.
* Switch the vendored FM/OPL engine from Nuked-OPL3 to
  [Nuked-OPL3-fast](https://github.com/tgies/Nuked-OPL3-fast), a bit-exact
  perf fork pinned to current upstream (1.5x-2.8x faster, and picks up
  upstream's 2024 envelope generator fix our old vendored copy predated)

#### Version: 1.3.0
* Add Cynthcart support to the embedded player, runs through USBSID-Player's own C64 core instead of the old emudore based path
* Update SID/PRG loading for the embedded player

#### Version: 1.2.2
* Fix JavaScript issues in the web player

#### Version: 1.2.1
* Update the web JavaScript API

#### Version: 1.2.0
* Add per chip and per voice muting, in the API and the CLI (also usable while recording)
* Fix SID register tracing
* Fix reset handling
* Update the boot image used to skip a tune's boot sequence
* Update the WebAssembly backend
* Drop USBSID-driver as a git submodule in favor of a plain checkout
* Add tests

#### Version: 1.1.0
* Add software audio output (ResidFp via miniaudio) and WAV file output, alongside the USBSID-Pico hardware backend
* Add FM/OPL emulation (Nuked-OPL3) and audio output for FM/OPL tunes
* Add HVSC Songlengths support
* Add hot reset handling
* Vendor miniaudio, Nuked-OPL3 and ResidFp as libraries
* Fix board configuration not reaching the player config in CLI, Web and embedded builds
* Fix NTSC raster timing

#### Version: 1.0.0
* Rewrite the player from scratch: cycle exact 6502/6510, CIA, VIC-II (SID only) and SID emulation, replacing the old emudore and SidBerry derived emulation core
* Split the emulation into embeddable source files behind a wrapper API, separate from the CLI
* Add PRG program support alongside PSID/RSID tunes for the embedded player, with next/previous tune switching
* Add first iteration of a WebAssembly build and web player demo
* Add multiplatform (Linux/Windows/MacOS) desktop CLI builds and CI workflows
* Add unit test suite
