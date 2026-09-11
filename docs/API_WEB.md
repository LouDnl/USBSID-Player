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

`loadSID` returns falsy when the bytes are not a SID file, which is the page's cue to try
`loadPRG(bytes)`. Both stop a tune that is already playing first.

**A program is slow to start and a tune is not.** `loadPRG` boots a C64 to the BASIC prompt, loads
the program and types RUN, about two seconds of emulated time, and it runs on the calling thread. A
tune skips the boot entirely and starts in about fourteen milliseconds of emulated time.

A page that wants playback to survive a backgrounded tab, or does not want the WASM engine and the
UI sharing a thread, should use `USBSIDPlayerWorker` instead of constructing `USBSIDPlayerWeb`
directly — see "The worker variant" below. It presents the same methods, made async.

### Free functions

Exported by `usplayer-web.js` alongside the classes, for a page that wants to decide what to do
with a file before handing it to a player.

| Function | Returns | What it does |
|---|---|---|
| `isSidHeader(bytes)` | `boolean` | Whether `bytes` starts with the `PSID`/`RSID` magic. |
| `countSids(bytes)` | `number` (1 to `MAX_SIDS`) | How many SID chips the header asks for: `$7a`/`$7b` for a pre-v5 header (1-3), `$7a`'s low nibble (multiSidConfig) for v5, clamped to `MAX_SIDS` (15). |
| `sidModel(bytes)` | `number` (0 or 1) | Which chip the header says the tune was written for: 0 for 6581, 1 for 8580. Only meaningful for software audio — a board plays whatever chip is fitted. |

### `USBSIDPlayerWeb`

`new USBSIDPlayerWeb(module, transport = new NullTransport())` — `module` is an instantiated
Emscripten module (the object `USBSIDPlayer()` resolves to), `transport` is anything implementing
the transport contract below. `NullTransport` discards everything, so a player constructed with no
transport is inert rather than throwing; `CaptureTransport` records every write and is what the
test suite drains against.

#### Loading

| Method | What it does |
|---|---|
| `loadSID(bytes, subtune = 0)` | Load a PSID/RSID `Uint8Array`. `subtune` 0 is the file's own default. Stops anything already playing, then boots and starts the tune. Returns falsy if `bytes` is not a SID file. |
| `loadPRG(bytes)` | Load a PRG or P00 `Uint8Array`. Stops anything already playing, boots to BASIC, types RUN. |

#### Playback control

| Method | What it does |
|---|---|
| `async start(opts = {})` | Start the pump: an AudioWorklet tick source when Web Audio is available (kept alive by the audio thread even in a backgrounded tab), `requestAnimationFrame` otherwise. `opts.externalClock: true` skips installing either clock, for a caller (the worker) that calls `tick()` itself. |
| `stop()` | Stop the pump, silence the device, tear the tune down. |
| `pause(on)` / `get paused` | Pause or resume. Also mutes/unmutes the transport, so a sustained note actually goes quiet rather than just stopping mid-attack. |
| `setSpeed(mult)` / `get speed` | Playback speed, clamped to 0.1-8. 1 is normal. |
| `fastForward(on, mult = 4)` | Seek: run the emulation at `mult`x with nothing reaching the device (silent, see the transport contract), then resume at 1x. Use this rather than `setSpeed()` for a fast-forward button — `setSpeed()` alone still sends every write, which floods the device's queue. |
| `setClock(rateId)` | Forward a clock id to the transport: 0 default, 1 PAL, 2 NTSC, 3 DREAN. Applied automatically on load to match the tune. |
| `tick()` | One tick of whatever is driving playback: steps as many emulated frames as wall-clock time (`performance.now()`) has passed since the last tick, scaled by the current speed. Call this yourself only when driving the player with an external clock (`start({ externalClock: true })`); `start()` installs its own tick source otherwise. |
| `stepAndDrain()` | One frame of emulation, then hand everything the ring produced to the transport. What `tick()` calls internally, per step; exposed for a caller that wants single-frame control (a debugger, a frame-accurate test). |
| `nextSubtune()` / `prevSubtune()` | Move to the next/previous song, wrapping, with a full re-initialise. |
| `runStop()` | Press RUN/STOP on the emulated keyboard matrix — interrupts a running program. Returns whether it was accepted. |
| `forceSocketTwo()` | Send everything to socket two instead of socket one. |

#### Board configuration

| Method | What it does |
|---|---|
| `async applyBoardConfig()` | Read the connected board's own socket/FM-OPL configuration (via `transport.readBoardConfig()`) and apply it to the emulation. Returns `{ sidsSocketOne, sidsSocketTwo, fmoplSid, fmoplApplied }`, or `null` when the transport cannot answer (e.g. ASID). **Call this after connecting and before loading** — a tune's init writes go out under whatever is set at that moment, and it is not re-read on later loads. |
| `setSidConfig(numsids, socketOne, socketTwo, fmopl)` | Set the same configuration by hand, for a transport that cannot be asked. `numsids` is accepted and ignored: how many chips the emulation decodes comes from the tune's own header, not the board. `fmopl` is which chip answers `$df40`/`$df50`, 1-based, `-1`/`undefined` for none. Forced to `-1` regardless of the argument when the transport carries FM/OPL in a message of its own (`transport.fmAsOwnMessage`, e.g. ASID) rather than as ordinary chip registers. |
| `boardConfig()` | The last configuration applied (by either method above), or `null`. |

Without a socket/FM-OPL configuration applied, `$df40`/`$df50` writes have nowhere to go and are
dropped, so an FM/OPL tune plays only its SID voices.

#### Inspection and diagnostics

| Method | Returns | What it is |
|---|---|---|
| `isPlaying()` | `boolean` | |
| `isPrg()` | `boolean` | Whether what is loaded is a program rather than a tune. |
| `info()` | `{ name, author, released, song, songs, isPrg, numSidsRequested, sids, panLayout, panMode, hasFmOpl, hasEmbeddedSongLengths }` | The SID header's own strings, current/total song count, and (v5) the multi-SID/panning summary. `numSidsRequested` is the raw count the file's own header asks for, which a v5 tune may set as high as 15; `sids` is only ever the (at most 4) chips this player actually wires up, each `{ addr, pan }` with `pan` one of `'L'`/`'C'`/`'R'` (a hint, not rendered: the audio path is one channel). `panLayout`/`panMode` are `'standard'`/`'lcr'`/`'centerFirst'`/`'fullyCentered'` and `'direct'`/`'reverse'`/`'group'`/`'spread'`, both `'standard'`/`'direct'` for a pre-v5 tune. |
| `frames()` | `number` | Frames of emulation run since load. |
| `refreshHz()` | `number` | 50.125 (PAL) or 59.83 (NTSC), the tune's actual frame rate — not rounded, so a page pacing against it does not drift a frame every few seconds. |
| `playtimeMs()` | `number` | How far into the current **song** the emulation is, in emulated milliseconds. Resets on every load and every subtune change; does not advance while paused; jumps on a seek. Not the song's length (that needs an HVSC Songlengths database, see below). |
| `sidWrites()` | `number` | SID writes performed since the tune started. |
| `droppedWrites()` | `number` | Writes dropped because the ring the WASM heap and the page share was full — non-zero means the page stopped draining for a while. |
| `readMemory(address)` | `number` (byte) | One byte of the emulated machine's RAM, 0-65535, straight through with no banking and no side effects — an I/O address answers with what is beneath the chip, not the chip's own register, so polling this every frame cannot itself break a tune. |
| `sidRegister(chip, reg)` | `number` (byte) | The last value written to SID register `reg` (0-31) of `chip` (1-4), from the emulation's own mirror. Works even when a transport takes writes inside itself and never emits them onward (software audio). |
| `ciaLatch(cia = 1, timer = 0)` | `number` | A CIA timer's reload latch (`cia` 1 or 2, `timer` 0 for A / 1 for B). A frame's cycles divided by this is how many times a CIA-driven tune calls its play routine per frame. |
| `timing()` | `{ irq: string[], start: string, driver: number }` | What is actually driving the tune right now, read from the chips rather than guessed from the file: `irq` lists which of `'CIA1 TA'/'TB'/'TOD'`, `'CIA2 TA'/'TB'/'TOD'`, `'VIC raster'` are currently armed; `start` is `'PSIDdrv'`, `'BASIC'` or `'PRG'`; `driver` is where the PSID driver was relocated in RAM. |
| `resetStats()` / `stats()` | — / object | Playback health since the last `resetStats()` (or load): `{ seconds, ticks, framesPerSecond, meanDrainGap, maxDrainGap, maxTickGap, starved, blocked, maxQueue, workPercent, dropped, usb }`. `maxDrainGap` is the number to read for audible trouble — the board plays what it was given at the delays it was given, so it survives one late tick and starves on a long one; an average hides exactly the event that is audible. `usb` is `transport.usbStats()` when the transport has one, else `null`. |

#### Muting

| Method | What it does |
|---|---|
| `setVoiceMute(chip, voice, muted)` | Hold voice `voice` (1-3) of `chip` (1-4) silent while the tune keeps playing. Masks the gate bit only; every other write (waveform, sync, filter) still reaches the chip, so an unmuted voice returns in the right state. |
| `voiceMute(chip = 1)` | The mute bitmask of one chip, bit 0 = voice 1. |
| `setChipMute(chip, muted)` | Drop a whole chip's writes, including its volume register — the only way to silence a tune that plays digi samples through `$d418`, which a voice mute cannot touch. |
| `chipMute()` | The muted-chips bitmask, bit 0 = chip 1. |

#### Songlengths (HVSC)

| Method | What it does |
|---|---|
| `md5(bytes)` | The plain MD5 of the whole file (not the PSID-MD5 variant some players use), as 32 lowercase hex characters — the HVSC Songlengths key. Computed in WASM because browsers do not expose plain MD5. |
| `loadSonglengths(text)` | Hand over the `Songlengths.md5` file's text once; kept in the WASM heap so every lookup does not re-upload it. Returns `false` if it could not be allocated. |
| `releaseSonglengths()` / `get hasSonglengths` | Free it / whether one is loaded. |
| `songLengths(key)` | One array entry per song, in milliseconds, for the 32-character `key` from `md5()`; `null` if the key is not in the database. |
| `embeddedSongLengths()` | Same shape as `songLengths()`, but from a v5 tune's own embedded table (flags bit 10) rather than the external database; `null` when the loaded tune has none. A reasonable fallback when `songLengths()` returns `null` for a file the database has never heard of. |

### Transports

A transport implements `writeCycled(reg, val, cycles)` and `flush()`, and may implement
`resetSID()` / `reset()`, `mute()` / `unmute()`, `setClock(id)` and `playbackStart()`. It may also
set `fmAsOwnMessage = true` (see `setSidConfig()` above) and implement `async readBoardConfig()`
(see `applyBoardConfig()` above). Swap by constructing the player with a different one, or by
assigning `player.transport`.

| | `USBSIDWebUSBTransport` | `ASIDMIDITransport` |
|---|---|---|
| API | WebUSB, `navigator.usb` | Web MIDI, `requestMIDIAccess({sysex:true})` |
| Timing | cycle exact, the gap travels with the write | a register snapshot per frame |
| Packet | `0x80\|(n*4)` then n x `[reg,val,cyc_hi,cyc_lo]` | `F0 2D 4E <mask><msb><values> F7` |
| Clock | `setClock(id)` | not applicable |
| FM/OPL | as ordinary chip registers, once a socket is configured | its own `0x60` SysEx message (`fmAsOwnMessage`) |
| Good for | everything, digis included | anything that is not a digi |

ASID ignores the cycle gaps because the protocol has nowhere to put them: its 0x30 timing
extension would, and is not implemented. A digi tune writing `$d418` six hundred times a frame
turns into six hundred SysEx messages, which is fine over USB MIDI and hopeless over a 31250 baud
DIN cable.

#### `USBSIDWebUSBTransport`

`new USBSIDWebUSBTransport(opts = {})` — `opts.device` reuses a `USBSIDDevice` the host page has
already opened (see "Embedding in another page" below) instead of opening a second connection to
the same board.

| Method | What it does |
|---|---|
| `async connect()` | Show the WebUSB device picker (needs a user gesture) and open the board, or attach to `opts.device` if one was given. |
| `async connectGranted()` | Attach to a board the origin has already been granted access to (`navigator.usb.getDevices()`), with no picker — what a module worker uses, since `requestDevice()` needs a document. |
| `async disconnect()` | Release the interface and close the device (a no-op when using `opts.device`, which is not this transport's to close). |
| `get isOpen` / `get productName` | |
| `writeCycled(reg, val, cycles)` / `flush()` | The transport contract. Batches several writes into one USB transfer before sending. |
| `resetSID()` / `mute()` / `unmute()` | Immediate device commands, bypassing the write batch. |
| `setClock(rateId)` | 0 default, 1 PAL, 2 NTSC, 3 DREAN. |
| `setAudioSwitch(stereo)` / `toggleAudioSwitch()` | The v1.3 PCB's audio switch, if the board has one. |
| `async readBoardConfig()` | `{ sidsSocketOne, sidsSocketTwo, fmoplSid }`, read from the board's own socket/FM-OPL configuration — what `USBSIDPlayerWeb.applyBoardConfig()` calls. |
| `async features()` / `async hasSidPlayer()` | The board's feature bitmask, and whether the onboard SID player (`ONBOARD_SIDPLAYER=1`) is built in. |

The board's **onboard** SID player — the firmware playing a tune itself, with the browser only
uploading the file and sending transport controls — is a separate feature from streaming register
writes, reached through `uploadSIDFile(bytes, subtune, fileType, onProgress)` and the
`playerLoadTune`/`playerStart`/`playerStop`/`playerPause`/`playerNext`/`playerPrev`/
`playerSocketTwo`/`playerSetPlaytime`/`playerTime`/`playerMute`/`playerMuteAll`/`playerMuteChip`/
`playerMuteState` methods. A page using it does not construct a `USBSIDPlayerWeb` at all — there is
no browser-side emulation to drive — and drives these methods on the transport directly instead.

#### `ASIDMIDITransport`

`new ASIDMIDITransport(opts = {})` — `opts.nosids` (default 1) is how many SID chips' worth of
registers to send per frame; `opts.deviceNameHint` (default `'usbsid'`) is used to pick a default
output by name.

| Method | What it does |
|---|---|
| `async connect(outputId = null)` | Request Web MIDI access and pick an output: `outputId` if given, else one whose name contains `deviceNameHint`, else the first available. |
| `outputs()` | `[{ id, name }]` of the outputs `connect()` found. |
| `selectOutput(outputId)` / `selectOutputByName(name)` | Switch outputs after connecting. |
| `async disconnect()` | |
| `get isOpen` / `get productName` | |
| `playbackStart()` / `playbackStop()` | Sends the ASID start/stop SysEx and, on stop, clears every chip's shadowed registers. |
| `writeCycled(reg, val, cycles)` / `flush()` | The transport contract; `cycles` is accepted and ignored (see above). `flush()` emits one `0x4E`/`0x50`/`0x51`/`0x52` SysEx per chip that changed this frame, and one `0x60` SysEx for any FM/OPL register pairs queued since the last flush. |
| `resetSID()` / `reset()` | `resetSID()` calls `playbackStop()`; `reset()` only clears the shadowed registers. |

### The worker variant

`web/usplayer-worker-client.js` exports `USBSIDPlayerWorker`, which presents the same method names
as `USBSIDPlayerWeb` — `loadSID`, `loadPRG`, `start`, `stop`, `pause`, `setSpeed`, `fastForward`,
`nextSubtune`, `prevSubtune`, `runStop`, `forceSocketTwo`, `setClock`, `setSidConfig`,
`applyBoardConfig`, `boardConfig`, `resetStats`, `stats`, `isPlaying`, `isPrg`, `frames`,
`sidWrites`, `droppedWrites`, `refreshHz`, `info` — made `async`, so a page can swap one class for
the other and change nothing else except adding `await`. What actually changes underneath: the
WASM engine, the emulation loop and the WebUSB connection all live in a module worker
(`web/usplayer-worker.js`), and the audio thread posts ticks straight to that worker over a
`MessageChannel`, so the main thread is never in the playback path and a busy or backgrounded tab
cannot starve the board.

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

`USBSIDPlayerWorker.available()` is `false` without `Worker`, WebUSB or Web Audio, all three of
which the worker path needs; a page should fall back to `USBSIDPlayerWeb` when it is. ASID always
falls back regardless, because Web MIDI has no worker-accessible API — `ASIDMIDITransport` only
runs on the main thread.

`new USBSIDPlayerWorker(opts = {})` accepts `opts.wasmUrl` (default `'./usbsid.mjs'`), `opts.workerUrl`
(default `'./usplayer-worker.js'` next to this file), and `opts.onLog` (a callback for the worker's
own log lines). `connect()` shows the WebUSB picker on the main thread — it needs the user gesture
and document that only the main thread has — then hands the granted device to the worker, which
reopens it with `navigator.usb.getDevices()`. `player.onState = fn` is called about ten times a
second with a state snapshot (`{ playing, prg, frames, sidWrites, dropped, queueDepth, refreshHz,
name, author, released, song, songs, stats }`) whenever the worker reports; `player.transportKind`
and `player.clockFallback` (true if the audio clock never ticked and the worker fell back to its
own timer) are set once `connect()`/`start()` have run. `terminate()` ends the worker outright.

### Embedding in another page

`web/usplayer-adapter.js` wraps a player in the interface `repo/examples/config-tool-web` expects
of a player backend (`load`, `play`, `pause`, `stop`, `setVolume`, `getSongInfo`, `paused`,
`emulator`), reusing a `USBSIDDevice` the host app has already connected rather than opening a
second one:

```js
import { USPlayerAdapter } from './usplayer-adapter.js';   // registers window.USPlayerAdapter too

const adapter = new USPlayerAdapter(emulatorKind, appsAlreadyConnectedDevice);
adapter.setHost({ log, status, registers, sidCount });   // wire it into the host app's own UI
await adapter.load(subtune, timeoutMs, url);
adapter.play();
```

Carried over from `player-repo/web/usplayer-adapter.js`, purpose-built for `config-tool-web`'s own
backend interface rather than a general-purpose wrapper — a page that is not that app is better
served by `USBSIDPlayerWeb`/`USBSIDPlayerWorker` directly.
