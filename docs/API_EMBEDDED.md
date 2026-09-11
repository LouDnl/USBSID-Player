## The embedded API

`#include <usplayer.h>` and call exactly what the firmware calls today:

```c
load_sidtune(sidfile, sidfile_size, tuneno);   /* the buffer may be freed right after */
init_sidplayer();
start_sidplayer(false);
while (playing) loop_sidplayer();              /* one frame per call */
stop_sidplayer();
next_subtune(); previous_subtune(); force_socktwo();
```

`load_sidtune` copies the file into the player, so freeing the caller's buffer immediately after
the call stays correct. `load_prg(binary, binsize, loop)` is the equivalent for a PRG or P00
container: there is no init step, so it boots a machine, POKEs the program where its first two
bytes say and starts it with RUN/SYS, returning ready for `loop_sidplayer()`.

`loop_sidplayer()` runs **one frame** and returns, where an older player ran one instruction. A
frame always finishes, even if the tune has crashed, because the VIC keeps counting regardless of
what the CPU does.

### Streaming upload

`load_sidtune()`/`load_prg()` above assume the whole file already sits in one buffer the caller
owns. A caller receiving the file over USB in small packets can feed it in directly instead,
without staging a second copy of its own first:

```c
usplayer_upload_start();
while (more_packets) usplayer_upload_feed(packet, n);
usplayer_upload_finish_prg(false);      /* or: */
usplayer_upload_finish_tune(subtune);
```

| Function | What it does |
|---|---|
| `void usplayer_upload_start(void)` | Reset the upload buffer. Call once before the first packet. |
| `bool usplayer_upload_feed(const uint8_t * buf, size_t len)` | Append one packet's worth of bytes. Returns `false` once the buffer is full; bytes past that point are dropped, the same truncation an oversized single buffer gets from `load_sidtune()`/`load_prg()`. |
| `void usplayer_upload_finish_prg(bool loop)` | Finish an upload started as a PRG — same tail as `load_prg()`. |
| `void usplayer_upload_finish_tune(char subt)` | Finish an upload started as a SID tune — same tail as `load_sidtune()`. |

`load_sidtune()`/`load_prg()` are unchanged and still work exactly as before for a caller that
already has the whole file in memory; they are built on top of these same three underneath.

### Playback control

| Function | What it does |
|---|---|
| `void init_sidplayer(void)` | Boot the machine, relocate the PSID driver, enter it. The slow call — see "Starting a tune is the slow part" below. |
| `void start_sidplayer(bool loop)` | Begin playing what `init_sidplayer()` prepared. |
| `void loop_sidplayer(void)` | Run one frame. Returns as soon as the frame is over. |
| `bool stop_sidplayer(void)` | Stop, silence the SIDs, report that playback is stopped. |
| `void next_subtune(void)` / `void previous_subtune(void)` | Move to the next/previous song, wrapping, with a full re-initialise. **Use these for a next/previous button**, not `usplayer_restart_song()` below: they need no song number from the caller, and the caller (firmware) has neither the song count nor the current song number until it asks. About 14 ms on the RP2350 from the boot image, cheap enough to always prefer over a driver-side jump. |
| `bool usplayer_restart_song(uint16_t song)` | Start song `song` (1 to `usplayer_songs()`) from its beginning. Out of range returns `false` and changes nothing — deliberately does **not** wrap, so a caller with a stale song count fails loudly instead of quietly playing the wrong song. |
| `void force_socktwo(void)` | Send everything to socket two instead of socket one. |
| `uint32_t usplayer_playtime_ms(void)` | How long the current **song** has been playing, in milliseconds. Resets on every load and every subtune change — not the song's *length*, which needs the HVSC Songlengths database. |

### Muting

| Function | What it does |
|---|---|
| `void usplayer_set_voice_mute(uint8_t chip, uint8_t voice, bool muted)` | Hold one voice (1-3) of one chip (1-4) silent. Forces the voice's gate bit to 0 on the way to the hardware; every other bit (waveform, sync, filter) still passes through, so the voice returns in the right state. `$d41b`/`$d41c` still answer as the tune expects. |
| `uint8_t usplayer_voice_mute(uint8_t chip)` | The mute bits for one chip, bits 0-2. |
| `void usplayer_set_chip_mute(uint8_t chip, bool muted)` | Drop a whole chip's writes, including its volume register — the only way to silence a tune playing samples on `$d418`, which a voice mute does not touch. Per chip, unlike the board's own hardware mute. |
| `uint8_t usplayer_chip_mute(void)` | The muted-chips bitmask, bit 0 = chip 1. |

### The keyboard

Goes through CIA1's matrix, so the KERNAL's own scan finds these exactly as it would a real key —
which is what makes them work on a running program, unlike poking the keyboard buffer.

| Function | What it does |
|---|---|
| `bool usplayer_type(const char * text)` | Queue a line as keystrokes. A key has to be held longer than one scan, so a line takes a few frames per character and only advances while frames are being run. `false` when the queue is full. |
| `bool usplayer_key_runstop(void)` | Press and release RUN/STOP — interrupts a running program. |
| `void usplayer_key_set(uint8_t row, uint8_t col, bool pressed)` | Hold a key down or let it up immediately, by matrix position (both 0-based). Row 7 col 7 is RUN/STOP, row 7 col 4 is space. |
| `void usplayer_keys_clear(void)` | Let everything up and discard anything queued. |
| `bool usplayer_typing(void)` | Whether a queued `usplayer_type()` line is still going in. |

### State, for a display

| Function | Returns |
|---|---|
| `bool usplayer_playing(void)` / `bool usplayer_paused(void)` | |
| `bool usplayer_loaded(void)` | Whether the last `load_sidtune()`/`load_prg()` parsed. |
| `bool usplayer_is_prg(void)` | Whether what is loaded is a program rather than a tune. |
| `uint16_t usplayer_song(void)` / `uint16_t usplayer_songs(void)` | Current song, total songs. |
| `uint32_t usplayer_frames(void)` | Frames run since load. |
| `uint16_t usplayer_driver_address(void)` | Where the PSID driver was relocated in RAM. |
| `const char * usplayer_tune_name(void)` / `_tune_author(void)` / `_tune_released(void)` | The SID header's own strings. A v5 header's variable length metadata is decoded the same as the classic 32 byte fields; nothing here changes shape for it. |
| `int usplayer_start_mode(void)` | `USP_START_DRIVER` (0, the normal PSID-driver path), `USP_START_BASIC` (1, an RSID with the BASIC flag and no init address — a program the machine boots and RUNs with no driver), or `USP_START_PRG` (2, a .prg/.p00). |
| `uint8_t usplayer_sid_count(void)` | How many SID chips the tune's own header asks for. A v5 header may ask for up to 15; this player wires up at most 4 regardless (`usplayer_sid_addr`/`_pan` below only ever describe those 4), so a caller showing this figure should say so when it is higher. |
| `uint16_t usplayer_sid_addr(uint8_t chip)` | Address of chip `chip` (0 based), or 0 past `usplayer_sid_count()` or past the 4 this player wires up. |
| `uint8_t usplayer_sid_pan(uint8_t chip)` | v5 panning hint for chip `chip`: 0 left, 1 center, 2 right. Not rendered by this player — the software audio path is one channel — so this is display only. |
| `uint8_t usplayer_pan_layout(void)` / `usplayer_pan_mode(void)` | v5 flags bits 6-7 and 8-9: layout is 0 standard, 1 L/C/R, 2 center first, 3 fully centered; mode is 0 direct, 1 reverse, 2 group, 3 spread. Both are 0 for a pre-v5 tune. |
| `bool usplayer_has_fm_opl(void)` | v5 flags bit 11: the tune uses an FM/OPL chip (SFX Sound Expander / FM YAM) alongside its SIDs. |
| `bool usplayer_has_embedded_songlengths(void)` | v5 flags bit 10: song lengths are appended to the end of the file rather than looked up in an external Songlengths database. |
| `uint32_t usplayer_embedded_songlength_ms(uint16_t song)` | One song's length from that embedded table, in ms, or 0 when there is none. `song` is 1 based. |
| `uint32_t usplayer_clock_hz(void)` | The SID clock the loaded tune was written for, in Hz. |
| `bool usplayer_is_pal(void)` | Whether that clock is one of the two PAL ones. |
| `double usplayer_refresh_hz(void)` | Frames per second of the tune's video model: 50.125 for PAL, 59.83 for NTSC, not the rounded 50/60 — a caller pacing against the rounded number drifts a whole frame every eight seconds. |

### Interrupt sources and CIA timing

```c
#define USP_IRQ_CIA1_TA    0x01u
#define USP_IRQ_CIA1_TB    0x02u
#define USP_IRQ_CIA1_TOD   0x04u
#define USP_IRQ_CIA2_TA    0x08u
#define USP_IRQ_CIA2_TB    0x10u
#define USP_IRQ_CIA2_TOD   0x20u
#define USP_IRQ_VIC_RASTER 0x40u
uint32_t usplayer_irq_sources(void);
uint16_t usplayer_cia_latch(uint8_t cia, uint8_t timer);
```

`usplayer_irq_sources()` is a bitmask of which interrupt sources the tune has actually **armed**
right now — read from the chips (a CIA source counts only when its mask bit is set *and* its timer
is running; the VIC raster source counts when its own enable bit is set), not guessed from the
file, so it reflects whatever the currently-playing subtune's init routine programmed and changes
if a later subtune programs something else. CIA2 raises NMI on real hardware but is reported here
under `CIA2_*` regardless, because the question being asked is what is driving the tune, not which
pin it uses.

`usplayer_cia_latch(cia, timer)` (`cia` 1 or 2, `timer` 0 for A / 1 for B) is the named timer's
reload **latch**, not its live counter (reading the counter has no fixed relationship to "how
often" and reading a CIA's own registers has side effects this does not want). Divide a frame's
cycles by the latch to get calls per frame — about 19654 is once a frame on PAL, half that is
twice.

### Direct memory and bus access

| Function | What it does |
|---|---|
| `uint8_t emu_dma_read_ram(uint16_t address)` / `void emu_dma_write_ram(uint16_t address, uint8_t data)` | Straight at the 64 KB of RAM, no banking, no side effects. |
| `uint8_t emu_read_byte(uint16_t address)` / `void emu_write_byte(uint16_t address, uint8_t data)` | Through the PLA, exactly as the CPU sees the address — an I/O address reaches the chip there, with whatever side effects that has. |

### Playback pacing

| Function | What it does |
|---|---|
| `void emu_pause_playing(bool pause)` | Pause playback. A paused player still answers every other call. |
| `void emu_ffwd(bool enable)` | Run without pacing. |

### Configuration

The firmware is not expected to configure the player, and mostly cannot: it hands over a file it
has not looked inside and does not know how many chips the tune wants or what clock it was written
for. The direction runs the other way: the player reads the tune's header, decides, and tells the
board. Nothing below has to be called for a tune to play correctly.

- **How many SIDs.** Taken from the tune, not clamped to what the board is carrying — a three-SID
  tune's writes to `$d420` and `$d440` are register writes on the same bus whether or not a chip is
  there to hear them.
- **The clock.** Applied at load time through the firmware's own `apply_clockrate()`, taken by a
  weak symbol: used if the firmware links it, a no-op otherwise.
  `usplayer_set_clock_follows_tune(false)` turns this off.

```c
void usplayer_set_sid_config(uint8_t numsids, uint8_t sids_socket_one,
                              uint8_t sids_socket_two, int8_t fmopl_sid);
void usplayer_set_clock_follows_tune(bool enable);
```

`usplayer_set_sid_config()` is for a caller that knows something about the board the player
cannot ask: it sets where socket two starts (for `force_socktwo()`) and which chip answers the
FM/OPL addresses `$df40`/`$df50` (`fmopl_sid`, 1-based, `-1` for none). `numsids` is accepted and
ignored, per the point above. Nothing has to call it.

### Diagnostics

| Function | What it is for |
|---|---|
| `uint32_t usplayer_sid_writes(void)` | SID writes performed since the tune started. |
| `uint64_t usplayer_cycles_waited(void)` | Cycles spent waiting out gaps too long to carry alongside a write. A tune with a long silent passage accumulates these normally; a large count while a tune is clearly playing points at a timing problem rather than the tune. |
| `uint64_t usplayer_cycles_paced(void)` | Cycles the player sat out itself rather than sending to the hardware, because a gap wide enough that the bus queue had already drained is waited out against the board's own clock instead of forwarded as a pre-delay. A tune with an idle loop paces nearly all its time; a digi tune paces none of it. |
| `uint32_t usplayer_static_footprint(void)` | Total statically allocated bytes, so the firmware can report how much RAM the player is holding. |
| `uint32_t usplayer_benchmark(uint32_t cycles)` | Runs the machine for `cycles` emulated PHI2 cycles with SID output disconnected, times it against the board's own clock, and restores state afterward. A real C64 needs 985 kcycles/s (PAL) or 1023 (NTSC); anything below that is how much too slow playback will run, and the ratio is exact. Not free, and not for use while playing — call it from a stopped player. Returns 0 when there is no clock to time against. |

### C++: the machine object

```c++
namespace usbsid {
class Machine;
Machine & usplayer_machine(void);
}
```

C++-only (outside the `extern "C"` block). Direct access to the `Machine` the player is stepping,
for a caller building against this player as a library rather than through the C surface above.
