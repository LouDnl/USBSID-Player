# Changelog
Please refer to the [releases page](https://github.com/LouDnl/USBSID-Player/releases) for more information on version changes

* Fix environment and SID type not being sent from the web player
* Fix silent ASID webplay after using ReSIDfp in the browser
* Fix reads from voice 3 for all modes when not using real reads
* Add identification of backend used to redirect reads for voice 3
* Set backend identification for each backend when initialised
* Make asid device number and midi device string readable
* Update fixed device logging to use backend identification
* Fix voice 3 reading when using the embedded player
* Fix FM/OPL over NSD to follow the NSD v5 spec (needs matching firmware)
* Rename CLI player binary usbsid to usplayer (test and ymfm builds follow)
* Change licence to GPL-2.0-or-later (was GPL-2.0-only)
* Fix double encoded tune name, author and release in web player
* Add playing without the Commodore ROMs (stub KERNAL, EMBED_ROMS=0, --no-roms)
* Add usplayer_set_roms() and usplayer_needs_roms() to the embedded API
* Add src/host/session.h playback session on an engine thread
* Fix slow play and fast forward in web software audio (reSIDfp)
* Add usp_audio_set_speed() and usp_audio_speed() exports
* Fix memory view in web software audio with the worker
* Add usp_read_memory_block() export
* Fix DeepSID buffer sizes below 200 ms being ignored
* Add per voice oscilloscope tap to software audio (reSIDfp)
* Add usp_audio_scope, _scope_voices and _scope_take exports
* Compile reSIDfp from a patched copy in the build tree (see patches/residfp)
* Add stereo and filter control to software audio in the web player
* Add usp_audio_set_stereo, _channels and _set_panning exports
* Add usp_audio_pan and usp_audio_set_filter exports
* Move scope, panning and filter handling to web/usplayer-softaudio.js
* Add web/usplayer-scope.js for per voice oscilloscopes
* Fix usp_audio_available() counting wrong after taking rendered audio
* Fix web status line in software audio with the worker
* Fix reSIDfp output differing from run to run in native builds
* Add ASID backend unit tests (tests/test_asid.cpp)
* Fix ASID environment message speed multiplier
* Fix failed asid_init leaking RtMidiOut
* Fix sendSIDEnvironment and sendSIDType crash before init
* Fix NTSC CIA1 timer A default to $4295
* Set UNIX_COMPILE and WINDOWS_COMPILE per platform

#### Version: 1.4.0
* Add multiboard playback (--boards SERIAL,... and --list-boards)
* Add FM/OPL to multiboard playback
* Add fm and fm:SLOT to --select-sids
* Fix -f and -fa moving only the first SID of a tune
* Add CMake option FM_YMFM=1 to build with ymfm for FM/OPL (CLI only)
* Update --trace to log like -srw plus cycle total and play time
* Add PSID/RSID v5 support (up to 15 SIDs, panning hint, FM flag, song lengths)
* Add --output nsd, a Network SID Device client (Linux/MacOs only!)
* Update netdevice backend to protocol version 5
* Add TRY_SET_FM_OPL (command 21) to enable/disable FM OPL on the server
* Widen software audio and netdevice backend to 15 chips
* Add --stereo to render a v5 panning hint as a stereo mix
* Fix embedded v5 song length table being ignored
* Add docs/API_WEB.md and docs/API_EMBEDDED.md
* Switch FM/OPL engine from Nuked-OPL3 to [Nuked-OPL3-fast](https://github.com/tgies/Nuked-OPL3-fast)
* Add SID register and CIA1 timer A reports from the web audio worker
* Add setSidVolume(), setFmVolume() and hasFm() to the web adapters
* Add hasFm and sidAddresses to the tune info of the web adapters
* Fix hasFm() missing SID+FM tunes older than v5
* Add setBufferSamples() to the DeepSID adapter
* Fix web audio ring overfilling after a throttled tab
* Fix slow controls after returning to a hidden tab
* Fix silence after a tab or app switch on mobile
* Fix cached older usplayer-worker.js being used by a newer adapter

#### Version: 1.3.0
* Add Cynthcart support to the embedded player
* Run Cynthcart through the player C64 core
* Update SID/PRG loading for the embedded player

#### Version: 1.2.2
* Fix JavaScript issues in the web player

#### Version: 1.2.1
* Update the web JavaScript API

#### Version: 1.2.0
* Add per chip and per voice muting to the API and CLI
* Fix SID register tracing
* Fix reset handling
* Update the boot image used to skip a tune's boot sequence
* Update the WebAssembly backend
* Drop USBSID-driver as a git submodule in favor of a plain checkout
* Add tests

#### Version: 1.1.0
* Add software audio output (ResidFp via miniaudio)
* Add WAV file output
* Add FM/OPL emulation (Nuked-OPL3) and audio output for FM/OPL tunes
* Add HVSC Songlengths support
* Add hot reset handling
* Vendor miniaudio, Nuked-OPL3 and ResidFp as libraries
* Fix board configuration not reaching the player config
* Fix NTSC raster timing

#### Version: 1.0.0
* Rewrite the player from scratch with a cycle exact CPU, CIA, VIC-II and SID
* Split the emulation into embeddable source files behind a wrapper API
* Add PRG program support to the embedded player
* Add previous/following tune switching
* Add first iteration of a WebAssembly build and web player demo
* Add multiplatform (Linux/Windows/MacOS) desktop CLI builds and CI workflows
* Add unit test suite
