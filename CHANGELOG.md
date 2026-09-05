# Changelog
Please refer to the [releases page](https://github.com/LouDnl/USBSID-Player/releases) for more information on version changes

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
