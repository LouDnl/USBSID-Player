## The JavaScript API

```js
import { USBSIDPlayerWeb } from './usplayer-web.js';
import { USBSIDWebUSBTransport } from './usbsid-webusb.js';   // or
import { ASIDMIDITransport } from './asid-midi.js';

const M = await USBSIDPlayer({ locateFile: (p) => './' + p });   // usbsid.js's own factory

const transport = new USBSIDWebUSBTransport();
await transport.connect();              // needs a user gesture

const player = new USBSIDPlayerWeb(M, transport);
await player.applyBoardConfig();             // see below, before loading
player.loadSID(sidBytes, /* subtune */ 0);   // Uint8Array; falsy if not a SID
await player.start();                        // starts the pump

player.pause(true);   player.pause(false);
player.nextSubtune(); player.prevSubtune();
player.fastForward(true, 4);                 // 4x, fastForward(false) back to 1x
player.runStop();                            // RUN/STOP, for a program
player.setClock(1);                          // 0 default 1 PAL 2 NTSC 3 DREAN
player.info();                               // title, author, released, song, songs
player.stop();                               // stops and silences the device
```

`loadSID` returns falsy when the bytes are not a SID file, the cue to try `loadPRG(bytes)`. Both stop a tune that is already playing.

**A program is slow to start, a tune is not.** `loadPRG` boots a C64 to the BASIC prompt, loads the program and types RUN. That takes about two seconds of emulated time on the calling thread. A tune skips the boot and starts in about fourteen milliseconds of emulated time.

Use `USBSIDPlayerWorker` instead of `USBSIDPlayerWeb` when playback must survive a backgrounded tab or the WASM engine and UI must not share a thread, see "The worker variant" below. It has the same methods, made async.

### Free functions

Exported by `usplayer-web.js` beside the classes, to decide what to do with a file before handing it to a player.

| Function | Returns | What it does |
|---|---|---|
| `isSidHeader(bytes)` | `boolean` | `bytes` starts with the `PSID`/`RSID` magic |
| `countSids(bytes)` | `number` (1 to `MAX_SIDS`) | SID chips the header asks for: `$7a`/`$7b` for a pre-v5 header (1-3), low nibble of `$7a` (multiSidConfig) for v5, up to `MAX_SIDS` (15) |
| `sidModel(bytes)` | `number` (0 or 1) | Chip the tune was written for: 0 for 6581, 1 for 8580. Software audio only, a board plays the chip that is fitted |

### `USBSIDPlayerWeb`

`new USBSIDPlayerWeb(module, transport = new NullTransport())` where `module` is an instantiated Emscripten module (the object `USBSIDPlayer()` resolves to) and `transport` implements the transport contract below. `NullTransport` discards everything, a player without transport is inert and does not throw. `CaptureTransport` records every write, the test suite drains against it.

#### Loading

| Method | What it does |
|---|---|
| `loadSID(bytes, subtune = 0)` | Load a PSID/RSID `Uint8Array`. `subtune` 0 is the default of the file. Stops what is playing, boots and starts the tune. Falsy if `bytes` is not a SID file |
| `loadPRG(bytes)` | Load a PRG or P00 `Uint8Array`. Stops what is playing, boots to BASIC, types RUN |

#### Playback control

| Method | What it does |
|---|---|
| `async start(opts = {})` | Start the pump, an AudioWorklet tick source when Web Audio is available (alive in a backgrounded tab), else `requestAnimationFrame`. `opts.externalClock: true` installs neither clock, for a caller (the worker) that calls `tick()` |
| `stop()` | Stop the pump, silence the device, tear the tune down |
| `pause(on)` / `get paused` | Pause or resume. Also mutes/unmutes the transport, a sustained note goes quiet and does not stop mid-attack |
| `setSpeed(mult)` / `get speed` | Playback speed from 0.1 to 8, 1 is normal |
| `fastForward(on, mult = 4)` | Seek: run the emulation at `mult`x with nothing reaching the device (silent, see the transport contract), then resume at 1x. Use this for a fast-forward button, `setSpeed()` sends every write and floods the queue of the device |
| `setClock(rateId)` | Forward a clock id to the transport: 0 default, 1 PAL, 2 NTSC, 3 DREAN. Applied on load to match the tune |
| `tick()` | One tick of the playback driver: steps the emulated frames for the wall-clock time (`performance.now()`) passed since the last tick, scaled by the speed. Call it only with an external clock (`start({ externalClock: true })`) |
| `stepAndDrain()` | One frame of emulation, then everything the ring produced goes to the transport. Called by `tick()` per step. For single-frame control (a debugger, a frame-accurate test) |
| `nextSubtune()` / `prevSubtune()` | Move to the following/previous song with wrapping and a full re-initialise |
| `runStop()` | Press RUN/STOP on the emulated keyboard matrix to interrupt a running program. Returns whether it was accepted |
| `forceSocketTwo()` | Send everything to socket two instead of socket one |

#### Board configuration

| Method | What it does |
|---|---|
| `async applyBoardConfig()` | Read the socket/FM-OPL configuration of the connected board (via `transport.readBoardConfig()`) and apply it to the emulation. Returns `{ sidsSocketOne, sidsSocketTwo, fmoplSid, fmoplApplied }`, or `null` when the transport cannot answer (e.g. ASID). **Call this after connecting and before loading**, the init writes of a tune use the configuration at that moment and it is not re-read on later loads |
| `setSidConfig(numsids, socketOne, socketTwo, fmopl)` | Set the same configuration by hand for a transport that cannot be asked. `numsids` is ignored, the chip count comes from the tune header. `fmopl` is the chip that answers `$df40`/`$df50`, 1-based, `-1`/`undefined` for none. Forced to `-1` when the transport carries FM/OPL as a message of its own (`transport.fmAsOwnMessage`, e.g. ASID) |
| `boardConfig()` | The last configuration applied (by either method above), or `null` |

Without a socket/FM-OPL configuration the `$df40`/`$df50` writes have nowhere to go and are dropped. An FM/OPL tune then plays only its SID voices.

#### Inspection and diagnostics

| Method | Returns | What it is |
|---|---|---|
| `isPlaying()` | `boolean` | |
| `isPrg()` | `boolean` | A program is loaded instead of a tune |
| `info()` | `{ name, author, released, song, songs, isPrg, numSidsRequested, sids, panLayout, panMode, hasFmOpl, hasEmbeddedSongLengths }` | Strings of the SID header, current/total song count and (v5) the multi-SID/panning summary. `numSidsRequested` is the raw count of the header, up to 15 for v5. `sids` holds the (at most 4) chips this player wires up as `{ addr, pan }` with `pan` `'L'`/`'C'`/`'R'` (a hint, the audio path is one channel). `panLayout`/`panMode` are `'standard'`/`'lcr'`/`'centerFirst'`/`'fullyCentered'` and `'direct'`/`'reverse'`/`'group'`/`'spread'`, `'standard'`/`'direct'` for a pre-v5 tune |
| `frames()` | `number` | Frames of emulation run since load |
| `refreshHz()` | `number` | 50.125 (PAL) or 59.83 (NTSC), the frame rate of the tune. Not rounded, pacing against it does not drift a frame every few seconds |
| `playtimeMs()` | `number` | Position in the current **song** in emulated milliseconds. Resets on every load and subtune change, stands still while paused, jumps on a seek. This is not the song length (needs an HVSC Songlengths database, see below) |
| `sidWrites()` | `number` | SID writes done since the tune started |
| `droppedWrites()` | `number` | Writes dropped because the ring shared by WASM heap and page was full. Non-zero means the page stopped draining for a while |
| `readMemory(address)` | `number` (byte) | One byte of the emulated RAM, 0-65535, no banking and no side effects. An I/O address answers with the RAM beneath the chip, polling every frame cannot break a tune |
| `sidRegister(chip, reg)` | `number` (byte) | Last value written to SID register `reg` (0-31) of `chip` (1-4), from the mirror of the emulation. Works when a transport takes writes inside itself and never emits them (software audio) |
| `ciaLatch(cia = 1, timer = 0)` | `number` | Reload latch of a CIA timer (`cia` 1 or 2, `timer` 0 for A / 1 for B). The cycles of a frame divided by this are the play routine calls per frame of a CIA-driven tune |
| `timing()` | `{ irq: string[], start: string, driver: number }` | What drives the tune at this moment, read from the chips. `irq` lists the armed sources of `'CIA1 TA'/'TB'/'TOD'`, `'CIA2 TA'/'TB'/'TOD'`, `'VIC raster'`. `start` is `'PSIDdrv'`, `'BASIC'` or `'PRG'`. `driver` is where the PSID driver was relocated in RAM |
| `resetStats()` / `stats()` | - / object | Playback health since the last `resetStats()` (or load): `{ seconds, ticks, framesPerSecond, meanDrainGap, maxDrainGap, maxTickGap, starved, blocked, maxQueue, workPercent, dropped, usb }`. Read `maxDrainGap` for audible trouble, the board plays what it was given at the delays it was given. It survives one late tick and starves on a long one, an average hides the audible event. `usb` is `transport.usbStats()` when the transport has one, else `null` |

#### Muting

| Method | What it does |
|---|---|
| `setVoiceMute(chip, voice, muted)` | Silence voice `voice` (1-3) of `chip` (1-4) while the tune keeps playing. Masks the gate bit only, other writes (waveform, sync, filter) reach the chip. An unmuted voice returns in the right state |
| `voiceMute(chip = 1)` | Mute bitmask of one chip, bit 0 = voice 1 |
| `setChipMute(chip, muted)` | Drop all writes of a chip incl. the volume register. Needed to silence a tune that plays digi samples through `$d418`, a voice mute cannot touch that |
| `chipMute()` | Muted chips bitmask, bit 0 = chip 1 |

#### Songlengths (HVSC)

| Method | What it does |
|---|---|
| `md5(bytes)` | Plain MD5 of the whole file (not the PSID-MD5 variant), 32 lowercase hex characters, the HVSC Songlengths key. Computed in WASM, browsers have no plain MD5 |
| `loadSonglengths(text)` | Hand over the text of `Songlengths.md5` once. Kept in the WASM heap to skip re-uploading on every lookup. Returns `false` if it could not be allocated |
| `releaseSonglengths()` / `get hasSonglengths` | Free it / one is loaded |
| `songLengths(key)` | One array entry per song in milliseconds for the 32-character `key` from `md5()`. `null` if the key is not in the database |
| `embeddedSongLengths()` | Same shape as `songLengths()` from the embedded table of a v5 tune (flags bit 10). `null` when the tune has none. A fallback when `songLengths()` returns `null` |

### Transports

A transport implements `writeCycled(reg, val, cycles)` and `flush()`, and may implement `resetSID()` / `reset()`, `mute()` / `unmute()`, `setClock(id)` and `playbackStart()`. It may set `fmAsOwnMessage = true` (see `setSidConfig()` above) and implement `async readBoardConfig()` (see `applyBoardConfig()` above). Construct the player with another transport or assign `player.transport` to swap.

| | `USBSIDWebUSBTransport` | `ASIDMIDITransport` |
|---|---|---|
| API | WebUSB, `navigator.usb` | Web MIDI, `requestMIDIAccess({sysex:true})` |
| Timing | cycle exact, the gap travels with the write | a register snapshot per frame |
| Packet | `0x80\|(n*4)` then n x `[reg,val,cyc_hi,cyc_lo]` | `F0 2D 4E <mask><msb><values> F7` |
| Clock | `setClock(id)` | not applicable |
| FM/OPL | as ordinary chip registers, once a socket is configured | its own `0x60` SysEx message (`fmAsOwnMessage`) |
| Good for | everything, digis included | anything that is not a digi |

ASID ignores the cycle gaps, the protocol has nowhere to put them (the 0x30 timing extension would, it is not implemented). A digi tune writing `$d418` six hundred times a frame turns into six hundred SysEx messages. That is fine over USB MIDI and hopeless over a 31250 baud DIN cable.

#### `USBSIDWebUSBTransport`

`new USBSIDWebUSBTransport(opts = {})`. `opts.device` reuses a `USBSIDDevice` the host page has opened (see "Embedding in another page" below) and skips a second connection to the same board.

| Method | What it does |
|---|---|
| `async connect()` | Show the WebUSB device picker (needs a user gesture) and open the board, or attach to `opts.device` |
| `async connectGranted()` | Attach to a board the origin has been granted access to (`navigator.usb.getDevices()`) without picker. Used by a module worker, `requestDevice()` needs a document |
| `async disconnect()` | Release the interface and close the device (no-op with `opts.device`) |
| `get isOpen` / `get productName` | |
| `writeCycled(reg, val, cycles)` / `flush()` | The transport contract. Batches several writes into one USB transfer |
| `resetSID()` / `mute()` / `unmute()` | Immediate device commands, bypassing the write batch |
| `setClock(rateId)` | 0 default, 1 PAL, 2 NTSC, 3 DREAN |
| `setAudioSwitch(stereo)` / `toggleAudioSwitch()` | Audio switch of the v1.3 PCB, if the board has one |
| `async readBoardConfig()` | `{ sidsSocketOne, sidsSocketTwo, fmoplSid }` from the socket/FM-OPL configuration of the board, called by `USBSIDPlayerWeb.applyBoardConfig()` |
| `async features()` / `async hasSidPlayer()` | Feature bitmask of the board, and whether the onboard SID player (`ONBOARD_SIDPLAYER=1`) is built in |

The **onboard** SID player (the firmware plays the tune, the browser uploads the file and sends transport controls) is a separate feature from streaming register writes. Use `uploadSIDFile(bytes, subtune, fileType, onProgress)` and the `playerLoadTune`/`playerStart`/`playerStop`/`playerPause`/`playerNext`/`playerPrev`/`playerSocketTwo`/`playerSetPlaytime`/`playerTime`/`playerMute`/`playerMuteAll`/`playerMuteChip`/`playerMuteState` methods on the transport directly. No `USBSIDPlayerWeb` is constructed, there is no browser-side emulation to drive.

#### `ASIDMIDITransport`

`new ASIDMIDITransport(opts = {})`. `opts.nosids` (default 1) is the number of SID chips of registers to send per frame. `opts.deviceNameHint` (default `'usbsid'`) picks a default output by name.

| Method | What it does |
|---|---|
| `async connect(outputId = null)` | Request Web MIDI access and pick an output: `outputId` if given, else one with `deviceNameHint` in its name, else the first available |
| `outputs()` | `[{ id, name }]` of the outputs `connect()` found |
| `selectOutput(outputId)` / `selectOutputByName(name)` | Switch outputs after connecting |
| `async disconnect()` | |
| `get isOpen` / `get productName` | |
| `playbackStart()` / `playbackStop()` | Send the ASID start/stop SysEx, on stop clear the shadowed registers of every chip |
| `writeCycled(reg, val, cycles)` / `flush()` | The transport contract. `cycles` is ignored (see above). `flush()` emits one `0x4E`/`0x50`/`0x51`/`0x52` SysEx per chip that changed this frame and one `0x60` SysEx for FM/OPL register pairs queued since the last flush |
| `resetSID()` / `reset()` | `resetSID()` calls `playbackStop()`. `reset()` clears the shadowed registers only |

### The worker variant

`web/usplayer-worker-client.js` exports `USBSIDPlayerWorker` with the method names of `USBSIDPlayerWeb` (`loadSID`, `loadPRG`, `start`, `stop`, `pause`, `setSpeed`, `fastForward`, `nextSubtune`, `prevSubtune`, `runStop`, `forceSocketTwo`, `setClock`, `setSidConfig`, `applyBoardConfig`, `boardConfig`, `resetStats`, `stats`, `isPlaying`, `isPrg`, `frames`, `sidWrites`, `droppedWrites`, `refreshHz`, `info`), made `async`. Swap one class for the other and add `await`. The WASM engine, the emulation loop and the WebUSB connection live in a module worker (`web/usplayer-worker.js`) and the audio thread posts ticks to that worker over a `MessageChannel`. The main thread is not in the playback path, a busy or backgrounded tab cannot starve the board.

```js
import { USBSIDPlayerWorker } from './usplayer-worker-client.js';

if (USBSIDPlayerWorker.available()) {
  const player = new USBSIDPlayerWorker();
  await player.connect();                     // shows the WebUSB picker itself
  await player.loadSID(sidBytes, 0);
  await player.start();
} else {
  // fall back to USBSIDPlayerWeb + USBSIDWebUSBTransport
}
```

`USBSIDPlayerWorker.available()` is `false` without `Worker`, WebUSB or Web Audio, the worker path needs all three. Fall back to `USBSIDPlayerWeb` when it is. ASID always falls back, Web MIDI has no worker API and `ASIDMIDITransport` runs on the main thread only.

`new USBSIDPlayerWorker(opts = {})` accepts `opts.wasmUrl` (default `'./usbsid.mjs'`), `opts.workerUrl` (default `'./usplayer-worker.js'` beside this file) and `opts.onLog` (callback for the log lines of the worker). `connect()` shows the WebUSB picker on the main thread (needs the user gesture and document) and hands the granted device to the worker, which reopens it with `navigator.usb.getDevices()`. `player.onState = fn` is called about ten times a second with a state snapshot (`{ playing, prg, frames, sidWrites, dropped, queueDepth, refreshHz,
name, author, released, song, songs, stats }`) whenever the worker reports. `player.transportKind` and `player.clockFallback` (true if the audio clock never ticked and the worker fell back to its timer) are set once `connect()`/`start()` have run. `terminate()` ends the worker.

### Embedding in another page

`web/usplayer-adapter.js` wraps a player in the interface `repo/examples/config-tool-web` expects of a player backend (`load`, `play`, `pause`, `stop`, `setVolume`, `getSongInfo`, `paused`, `emulator`) and reuses a `USBSIDDevice` the host app has connected:

```js
import { USPlayerAdapter } from './usplayer-adapter.js';   // registers window.USPlayerAdapter too

const adapter = new USPlayerAdapter(emulatorKind, appsAlreadyConnectedDevice);
adapter.setHost({ log, status, registers, sidCount });   // wire it into the host app's own UI
await adapter.load(subtune, timeoutMs, url);
adapter.play();
```

Taken from `player-repo/web/usplayer-adapter.js`, made for the backend interface of `config-tool-web`. A page that is not that app is better served by `USBSIDPlayerWeb`/`USBSIDPlayerWorker` directly.
