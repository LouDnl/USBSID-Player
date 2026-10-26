## The embedded API

`#include <usplayer.h>` and call what the firmware calls today:

```c
load_sidtune(sidfile, sidfile_size, tuneno);   /* the buffer may be freed right after */
init_sidplayer();
start_sidplayer(false);
while (playing) loop_sidplayer();              /* one frame per call */
stop_sidplayer();
next_subtune(); previous_subtune(); force_socktwo();
```

`load_sidtune` copies the file into the player, free your buffer after the call. `load_prg(binary, binsize, loop)` is the same for a PRG or P00 container. There is no init step, it boots a machine, POKEs the program where its first two bytes say and starts it with RUN/SYS. It returns ready for `loop_sidplayer()`.

`loop_sidplayer()` runs **one frame** and returns (the old player ran one instruction). A frame always finishes, even if the tune has crashed, the VIC keeps counting whatever the CPU does.

### Streaming upload

`load_sidtune()`/`load_prg()` above need the whole file in one buffer. When you receive the file over USB in small packets, feed it in directly and skip the second copy:

```c
usplayer_upload_start();
while (more_packets) usplayer_upload_feed(packet, n);
usplayer_upload_finish_prg(false);      /* or: */
usplayer_upload_finish_tune(subtune);
```

| Function | What it does |
|---|---|
| `void usplayer_upload_start(void)` | Reset the upload buffer, call once before the first packet |
| `bool usplayer_upload_feed(const uint8_t * buf, size_t len)` | Append a packet, returns `false` when the buffer is full. Further bytes are dropped like with an oversized buffer in `load_sidtune()`/`load_prg()` |
| `void usplayer_upload_finish_prg(bool loop)` | Finish an upload as PRG, same tail as `load_prg()` |
| `void usplayer_upload_finish_tune(char subt)` | Finish an upload as SID tune, same tail as `load_sidtune()` |

`load_sidtune()`/`load_prg()` work as before when you have the whole file in memory. They use these three functions underneath.

### Playback control

| Function | What it does |
|---|---|
| `void init_sidplayer(void)` | Boot the machine, relocate the PSID driver, enter it. The slow call, see "Starting a tune is the slow part" below |
| `void start_sidplayer(bool loop)` | Start playing what `init_sidplayer()` prepared |
| `void loop_sidplayer(void)` | Run one frame and return when it is over |
| `bool stop_sidplayer(void)` | Stop, silence the SIDs, report that playback is stopped |
| `void next_subtune(void)` / `void previous_subtune(void)` | Move to the following/previous song with wrapping and a full re-initialise. **Use these for a next/previous button**, they need no song number (`usplayer_restart_song()` below does). About 14 ms on the RP2350 from the boot image |
| `bool usplayer_restart_song(uint16_t song)` | Start song `song` (1 to `usplayer_songs()`) from the beginning. Out of range returns `false` and changes nothing, it does **not** wrap |
| `void force_socktwo(void)` | Send everything to socket two instead of socket one |
| `uint32_t usplayer_playtime_ms(void)` | Milliseconds the current **song** has been playing. Resets on every load and subtune change. This is not the song length, that needs the HVSC Songlengths database |

### Muting

| Function | What it does |
|---|---|
| `void usplayer_set_voice_mute(uint8_t chip, uint8_t voice, bool muted)` | Silence one voice (1-3) of one chip (1-4). The gate bit goes out as 0, other bits (waveform, sync, filter) pass through and the voice returns in the right state. `$d41b`/`$d41c` answer as the tune expects |
| `uint8_t usplayer_voice_mute(uint8_t chip)` | Mute bits for one chip, bits 0-2 |
| `void usplayer_set_chip_mute(uint8_t chip, bool muted)` | Drop all writes of a chip incl. the volume register. Needed to silence a tune playing samples on `$d418`, a voice mute does not touch that. Per chip, not the hardware mute of the board |
| `uint8_t usplayer_chip_mute(void)` | Muted chips bitmask, bit 0 = chip 1 |

### The keyboard

Goes through the CIA1 matrix, the KERNAL scan finds these like a real key. This works on a running program, poking the keyboard buffer does not.

| Function | What it does |
|---|---|
| `bool usplayer_type(const char * text)` | Queue a line as keystrokes. A key must be held longer than one scan, a line takes a few frames per character and only advances while frames run. `false` when the queue is full |
| `bool usplayer_key_runstop(void)` | Press and release RUN/STOP to interrupt a running program |
| `void usplayer_key_set(uint8_t row, uint8_t col, bool pressed)` | Press or release a key by matrix position (0 based). Row 7 col 7 is RUN/STOP, row 7 col 4 is space |
| `void usplayer_keys_clear(void)` | Release everything and discard the queue |
| `bool usplayer_typing(void)` | A queued `usplayer_type()` line is still going in |

### State, for a display

| Function | Returns |
|---|---|
| `bool usplayer_playing(void)` / `bool usplayer_paused(void)` | |
| `bool usplayer_loaded(void)` | The last `load_sidtune()`/`load_prg()` parsed |
| `bool usplayer_is_prg(void)` | A program is loaded instead of a tune |
| `uint16_t usplayer_song(void)` / `uint16_t usplayer_songs(void)` | Current song, total songs |
| `uint32_t usplayer_frames(void)` | Frames run since load |
| `uint16_t usplayer_driver_address(void)` | Where the PSID driver was relocated in RAM |
| `const char * usplayer_tune_name(void)` / `_tune_author(void)` / `_tune_released(void)` | Strings of the SID header. A v5 variable length metadata is decoded like the classic 32 byte fields |
| `int usplayer_start_mode(void)` | `USP_START_DRIVER` (0, normal PSID-driver path), `USP_START_BASIC` (1, RSID with BASIC flag and no init address, booted and RUN without driver) or `USP_START_PRG` (2, a .prg/.p00) |
| `uint8_t usplayer_sid_count(void)` | SID chips the tune header asks for, a v5 header may ask for 15. The player wires up 4 at most (`usplayer_sid_addr`/`_pan` below only describe those 4), flag it when this figure is higher |
| `uint16_t usplayer_sid_addr(uint8_t chip)` | Address of chip `chip` (0 based), 0 past `usplayer_sid_count()` or past the 4 wired up |
| `uint8_t usplayer_sid_pan(uint8_t chip)` | v5 panning hint for chip `chip`: 0 left, 1 center, 2 right. Display only, software audio is one channel |
| `uint8_t usplayer_pan_layout(void)` / `usplayer_pan_mode(void)` | v5 flags bits 6-7 and 8-9. Layout 0 standard, 1 L/C/R, 2 center first, 3 fully centered. Mode 0 direct, 1 reverse, 2 group, 3 spread. Both 0 for a pre-v5 tune |
| `bool usplayer_has_fm_opl(void)` | v5 flags bit 11: tune uses an FM/OPL chip (SFX Sound Expander / FM YAM) beside its SIDs |
| `bool usplayer_has_embedded_songlengths(void)` | v5 flags bit 10: song lengths are appended to the file, no external Songlengths database |
| `uint32_t usplayer_embedded_songlength_ms(uint16_t song)` | Song length in ms from the embedded table, 0 when there is none. `song` is 1 based |
| `uint32_t usplayer_clock_hz(void)` | SID clock the tune was written for, in Hz |
| `bool usplayer_is_pal(void)` | Clock is PAL |
| `double usplayer_refresh_hz(void)` | Frames per second of the video model: 50.125 for PAL, 59.83 for NTSC. Pacing against rounded 50/60 drifts a frame every eight seconds |

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

`usplayer_irq_sources()` is a bitmask of the interrupt sources the tune has **armed** at this moment, read from the chips (a CIA source counts when its mask bit is set *and* its timer runs, the VIC raster source when its enable bit is set). The file is not consulted. It follows the init routine of the playing subtune and changes when a later subtune programs something else. CIA2 raises NMI on real hardware but is reported as `CIA2_*`, the mask tells what drives the tune.

`usplayer_cia_latch(cia, timer)` (`cia` 1 or 2, `timer` 0 for A / 1 for B) returns the reload **latch** of the timer, not the live counter (reading a CIA register has side effects). Divide the cycles of a frame by the latch for calls per frame, about 19654 is once a frame on PAL, half that is twice.

### Direct memory and bus access

| Function | What it does |
|---|---|
| `uint8_t emu_dma_read_ram(uint16_t address)` / `void emu_dma_write_ram(uint16_t address, uint8_t data)` | Straight at the 64 KB of RAM, no banking, no side effects |
| `uint8_t emu_read_byte(uint16_t address)` / `void emu_write_byte(uint16_t address, uint8_t data)` | Through the PLA like the CPU sees it, an I/O address reaches the chip with its side effects |

### Playback pacing

| Function | What it does |
|---|---|
| `void emu_pause_playing(bool pause)` | Pause playback, a paused player answers every other call |
| `void emu_ffwd(bool enable)` | Run without pacing |

### Configuration

The firmware does not need to configure the player. It hands over a file it has not looked inside and does not know the chip count or clock. The player reads the tune header, decides and tells the board. Nothing below has to be called for a tune to play correctly.

- **How many SIDs.** Taken from the tune and not clamped to the board. Writes of a three-SID tune to `$d420` and `$d440` are register writes on the same bus, a chip there or not.
- **The clock.** Applied at load time through `apply_clockrate()` of the firmware, taken by a weak symbol (used if the firmware links it, else a no-op). `usplayer_set_clock_follows_tune(false)` turns this off.

```c
void usplayer_set_sid_config(uint8_t numsids, uint8_t sids_socket_one,
                              uint8_t sids_socket_two, int8_t fmopl_sid);
void usplayer_set_clock_follows_tune(bool enable);
```

`usplayer_set_sid_config()` is for a caller that knows something about the board. It sets where socket two starts (for `force_socktwo()`) and which chip answers the FM/OPL addresses `$df40`/`$df50` (`fmopl_sid`, 1-based, `-1` for none). `numsids` is ignored. Calling it is optional.

### Diagnostics

| Function | What it is for |
|---|---|
| `uint32_t usplayer_sid_writes(void)` | SID writes done since the tune started |
| `uint64_t usplayer_cycles_waited(void)` | Cycles spent waiting out gaps too long to carry with a write. Tunes with a long silent passage collect these. A large count in a tune that clearly plays points at a timing problem |
| `uint64_t usplayer_cycles_paced(void)` | Cycles the player sat out and did not send to the hardware. A gap after the bus queue drained is waited out against the board clock and not forwarded as pre-delay. A tune with an idle loop paces nearly all its time, a digi tune none |
| `uint32_t usplayer_static_footprint(void)` | Total statically allocated bytes, to report the RAM the player holds |
| `uint32_t usplayer_benchmark(uint32_t cycles)` | Run the machine for `cycles` emulated PHI2 cycles with SID output disconnected, time it against the board clock and restore state. A real C64 needs 985 kcycles/s (PAL) or 1023 (NTSC), below that is how much too slow playback runs. Not free, call it from a stopped player. Returns 0 when there is no clock to time against |

### C++: the machine object

```c++
namespace usbsid {
class Machine;
Machine & usplayer_machine(void);
}
```

C++ only (outside the `extern "C"` block). Direct access to the `Machine` the player steps, for building against this player as a library.
