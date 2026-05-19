# descent-tools

Linux command-line utilities for working with **Descent 1 & 2** game files.

- **`hmpplay`** — plays `.hmp` music files directly to an external MIDI device via ALSA
- **`hmpplay_opl3`** — plays `.hmp` music files directly to a RetroWave OPL3 Express via USB serial, using Descent's own FM instrument banks
- **`hogtool`** — extracts and creates `.hog` archive files (Descent's big-file format)

No game engine, no SDL, no FluidSynth. These tools talk directly to your hardware.

---

## Tested hardware

| Device | Tool | Connection | Notes |
|---|---|---|---|
| Roland MT-32pi | `hmpplay` | MIDI cable → USB MIDI interface | Native MT-32 mode or GM/soundfont mode |
| RetroWave OPL3 Express | `hmpplay_opl3` | USB-C → `/dev/ttyACM0` | Direct OPL3 FM synthesis with Descent's own instrument banks |
| RetroWave OPL3 Express | `hmpplay` | Via [RetroWave MIDI Proxy](https://github.com/SudoMaker/RetroWaveMIDIProxy) | OPL3 via ALSA MIDI port (less accurate — proxy uses its own banks) |

Any device that appears as an ALSA sequencer port will work with `hmpplay`.

---

## Dependencies

```bash
sudo apt install libasound2-dev   # for hmpplay
# hmpplay_opl3 and hogtool need nothing beyond libc/libc++
```

---

## hmpplay

Reads `.hmp` files and sends MIDI events directly to any ALSA sequencer port. Supports
MT-32 native mode, General MIDI, and per-device track selection based on the HMP file's
internal device mapping table.

### Build

```bash
g++ -std=c++17 -O2 -o hmpplay hmpplay.cpp -lasound
```

### Quick start

```bash
# List available ALSA MIDI output ports
./hmpplay --list

# Play a single file (prompts for port if -p is not given)
./hmpplay game0.hmp

# Play all HMP files in a directory (sorted alphabetically)
./hmpplay -p 128:0 ./music/

# Loop indefinitely
./hmpplay -l -p 128:0 ./music/
```

### Options

| Option | Description |
|---|---|
| `-p client:port` | ALSA destination port (e.g. `128:0`). If omitted, lists ports and prompts. |
| `-l` | Loop — repeat the playlist indefinitely until `q` or Ctrl-C. |
| `-t scale` | Tempo multiplier. `0.5` = half speed, `2.0` = double. Default: `1.0`. |
| `-v` | Verbose — print device track mapping and every MIDI event. |
| `-D N` | HMP device index for track selection: `0`=OPL `1`=MT-32 `2`=GM `3`=Roland GS `4`=Tandy. |
| `--mt32` | Shorthand for `-D 1` plus MT-32 SysEx init (volume, reverb, partial reserve). |
| `--gm` | Shorthand for `-D 2` (GM tracks, no SysEx). |
| `--list` | List available ALSA MIDI output ports and exit. |

### Keyboard controls

These work while a file is playing — no Enter needed.

| Key | Action |
|---|---|
| `n` or `Space` | Skip to next song |
| `p` | Go back to previous song |
| `l` | Toggle loop on / off |
| `+` or `=` | Increase tempo by 25% (max 4×) |
| `-` | Decrease tempo by 25% (min 0.25×) |
| `q` or Ctrl-C | Quit |

### Getting the music files

The `.hmp` files are packed inside the game's `.hog` archives. Use `hogtool` to extract them:

```bash
./hogtool extract descent.hog -o ./music/
./hmpplay -p 128:0 ./music/
```

Descent 1 ships `game0.hmp` through `game9.hmp` plus `briefing.hmp`, `credits.hmp`, and
`descent.hmp`. Descent 2 has a similar set.

### Finding your ALSA port

```bash
./hmpplay --list
# or
aplaymidi -l
```

Example output:
```
Available ALSA MIDI output ports:
  Port   Client Name                    Port Name
  ------  ------------------------------ ----------
  14:0   Midi Through                   Midi Through Port-0
  20:0   USB MIDI Interface             USB MIDI Interface MIDI 1
  128:0  RetroWave MIDI Proxy           RetroWave OPL3
```

Pass the `client:port` number with `-p`, e.g. `-p 20:0`.

### MT-32pi

Descent's music was originally composed for the Roland MT-32, so native MT-32 mode gives
the most authentic MIDI sound — the instrument assignments, patch numbers, and reverb
settings all match exactly what the game intended.

**Switch your MT-32pi to MT-32 mode**, connect via MIDI cable to a USB MIDI interface, then:

```bash
./hmpplay --mt32 -p 20:0 ./music/
```

`--mt32` does three things automatically:

1. **Selects device 1 tracks** from the HMP file. Each HMP contains separate track
   arrangements per device (OPL, MT-32, GM, etc.). The MT-32 tracks use MT-32 ROM patch
   numbers and are voiced for its 8-voice polyphony model. Playing GM tracks through an
   MT-32 gives wrong instruments; `--mt32` picks the right ones.

2. **Sends SysEx initialisation** before the first note:
   - Master volume = 100
   - Reverb: Hall, time 4, level 4
   - Partial reserve: 4 voices per part across all 8 melody parts

3. Sends a full MIDI panic (all notes off) between tracks.

**GM/soundfont mode** on the MT-32pi also works well. Switch the MT-32pi to GM mode and use:

```bash
./hmpplay --gm -p 20:0 ./music/
```

`--gm` selects device 2 tracks (the GM-specific arrangement) without sending any SysEx.
This is what you were doing before and it sounds good — `--mt32` just sounds more like
the original DOS experience.

### RetroWave OPL3 Express via MIDI Proxy

The [RetroWave MIDI Proxy](https://github.com/SudoMaker/RetroWaveMIDIProxy) exposes the
OPL3 Express as a standard ALSA MIDI port. This is simpler to set up than `hmpplay_opl3`
but uses the proxy's own GM→OPL3 sound bank rather than Descent's authentic FM patches.

**Build the proxy:**

```bash
sudo apt install cmake qt5-default libqt5core5a

git clone https://github.com/SudoMaker/RetroWaveMIDIProxy.git
cd RetroWaveMIDIProxy
mkdir build && cd build
cmake ..
make -j$(nproc)
```

**Run the proxy, then play:**

```bash
# Terminal 1 — start the proxy
./RetroWaveMIDIProxy

# Terminal 2 — play
./hmpplay --list
./hmpplay -p 128:0 ./music/
```

The port number assigned to the proxy may vary; use `--list` to confirm it. The proxy
must stay running for the duration of playback.

For the most accurate OPL3 sound from Descent, use `hmpplay_opl3` instead (see below).

---

## hmpplay_opl3

Plays `.hmp` files directly to a RetroWave OPL3 Express via USB serial (`/dev/ttyACM0`),
using Descent's own FM instrument banks (`intmelo.bnk` / `intdrum.bnk`) extracted from
the game HOG. No MIDI proxy needed — register writes go straight to the YMF262 chip.

This produces the most authentic OPL3 sound possible: the exact same FM patches the game
used on a real Sound Blaster.

### Build

```bash
g++ -std=c++17 -O2 -o hmpplay_opl3 hmpplay_opl3.cpp
# No external dependencies
```

### Setup: extract the instrument banks

The FM patch banks are packed inside the game HOG archive alongside the music:

```bash
./hogtool extract descent.hog  -o ./banks/ intmelo.bnk intdrum.bnk
./hogtool extract descent2.hog -o ./banks/ intmelo.bnk intdrum.bnk
```

Several bank variants ship with Descent. The right ones for OPL hardware:

| File | Contents |
|---|---|
| `intmelo.bnk` | Melodic FM patches for OPL/AdLib ("int" = internal FM synthesis) |
| `intdrum.bnk` | Percussion FM patches for OPL/AdLib |
| `melodic.bnk` / `drum.bnk` | MT-32 / General MIDI patches (wrong for OPL) |
| `hammelo.bnk` / `hamdrum.bnk` | HMI AdLib Module variant patches |

### Quick start

```bash
./hmpplay_opl3 -m ./banks/intmelo.bnk -r ./banks/intdrum.bnk ./music/

# Loop
./hmpplay_opl3 -l -m ./banks/intmelo.bnk -r ./banks/intdrum.bnk ./music/
```

### Options

| Option | Description |
|---|---|
| `-d device` | Serial device (default: `/dev/ttyACM0`) |
| `-m melodic.bnk` | Melodic instrument bank (use `intmelo.bnk`) |
| `-r drums.bnk` | Percussion instrument bank (use `intdrum.bnk`) |
| `-D N` | HMP device index: `0`=OPL (default) `1`=MT-32 `2`=GM `3`=GS `4`=Tandy |
| `-l` | Loop playlist indefinitely |
| `-t scale` | Tempo multiplier (default: `1.0`) |
| `-v` | Verbose: print device track mapping and every MIDI event |

Keyboard controls are the same as `hmpplay` (`n`/`Space` next, `p` prev, `l` loop, `+`/`-` tempo, `q` quit).

### How it works

The RetroWave OPL3 Express connects over USB as a serial device at 2 Mbaud. `hmpplay_opl3`
speaks the RetroWave wire protocol directly: OPL3 register writes are framed in a 7-of-8
bit encoding and sent as raw bytes, bypassing ALSA entirely. The `intmelo.bnk` / `intdrum.bnk`
patches from the HOG are parsed and loaded into an in-memory voice allocator (18 two-operator
voices across both OPL3 register sets), which maps HMP MIDI events to YMF262 register writes
in real time.

---

## hogtool

Creates and extracts `.hog` archives — the big-file format used by Descent 1 & 2 to pack
all game assets (levels, music, bitmaps, banks, etc.) into a single file.

### Build

```bash
gcc -O2 -o hogtool hogtool.c
```

No dependencies beyond libc.

### Commands

#### List contents

```bash
./hogtool list <archive.hog>
```

```
Filename          Size
-------------  ----------
game0.hmp           34827
game1.hmp           28022
briefing.hmp        19834
level01.rdl        184320
descent.pig       4823041

5 file(s), 5090044 bytes total
```

#### Extract files

```bash
# Extract everything to a directory (created automatically)
./hogtool extract descent.hog -o ./out/

# Extract only specific files (case-insensitive)
./hogtool extract descent.hog -o ./music/ game0.hmp briefing.hmp credits.hmp

# Extract instrument banks for hmpplay_opl3
./hogtool extract descent.hog -o ./banks/ intmelo.bnk intdrum.bnk
```

#### Create an archive

```bash
./hogtool create mymission.hog level01.rdl level01.rl2 mymusic.hmp
```

Filenames inside a HOG are limited to 12 characters (DOS 8.3 heritage). `hogtool` will
warn and skip any file whose basename exceeds this limit.

### Typical workflows

**Extract and play with MT-32pi (native MT-32):**
```bash
./hogtool extract descent.hog -o ./music/
./hmpplay --mt32 -p 20:0 ./music/
```

**Extract and play with MT-32pi (GM/soundfont mode):**
```bash
./hogtool extract descent.hog -o ./music/
./hmpplay --gm -p 20:0 ./music/
```

**Extract and play with OPL3 Express (direct, most authentic FM sound):**
```bash
./hogtool extract descent.hog -o ./music/
./hogtool extract descent.hog -o ./banks/ intmelo.bnk intdrum.bnk
./hmpplay_opl3 -m ./banks/intmelo.bnk -r ./banks/intdrum.bnk ./music/
```

---

## HMP device track mapping

Each HMP file contains separate track arrangements optimised for different hardware. The
`deviceTrackMappings` table at header offset `0x80` lists which tracks to play per device:

| Index | Device | `hmpplay` flag | Instrument banks |
|---|---|---|---|
| 0 | OPL / AdLib | `-D 0` | `intmelo.bnk` + `intdrum.bnk` |
| 1 | Roland MT-32 | `--mt32` | MT-32 ROM patches (no BNK needed) |
| 2 | General MIDI | `--gm` | GM patches on device |
| 3 | Roland GS | `-D 3` | `hammelo.bnk` + `hamdrum.bnk` |
| 4 | Tandy / PS/1 | `-D 4` | — |

Playing without a `-D` flag sends all tracks merged, which works but may include
device-specific tracks not meant for your hardware.

---

## HOG format reference

```
[3 bytes]  magic: "DHF"
then, repeated until EOF:
  [13 bytes]  filename, null-padded (max 12 characters + null terminator)
  [4 bytes]   data length, little-endian uint32
  [N bytes]   file data
```

## HMP format reference

HMP (HMI MIDI Protocol) is a variant of Standard MIDI File (SMF):

- File magic: `HMIMIDIP` (8 bytes at offset 0)
- Track count: 4-byte LE int at offset `0x30`
- Tempo field: 4-byte LE int at offset `0x38`; time division = `tempo × 1.6`
- Device track map: `deviceTrackMappings[5][32]` at offset `0x80` (5 devices × 32 track slots, little-endian uint32 each)
- Track data starts at offset `0x308`; each track has a 12-byte header `[u32][length_u32][u32]`
- Delta times use HMI variable-length encoding: **little-endian**, MSB-terminated (opposite of standard MIDI VLQ)
- Effective tempo: `1,605,632 µs/beat` (~37.4 BPM), matching the value DXX-Rebirth hardcodes when converting HMP to SMF (`FF 51 03 18 80 00`)
- Track 0 is a conductor/setup track; music tracks start at index 1

---

## Credits

- HMP format research and parsing based on `common/misc/hmp.cpp` from
  [DXX-Rebirth](https://www.dxx-rebirth.com/), originally by Arne de Bruijn
  and the JFFEE project (GPL v2+)
- HMP header layout (`deviceTrackMappings`) from [ScummVM](https://github.com/scummvm/scummvm)
  `audio/midiparser_hmp.cpp` (GPL v3+)
- HOG format documented via DXX-Rebirth utilities by Josh Cogliati and Bradley Bell (GPL v2+)
- RetroWave serial wire protocol from [SudoMaker/RetroWave](https://github.com/SudoMaker/RetroWave)
  `RetroWaveLib/` (AGPLv3)
- MT-32 SysEx structure from the Roland MT-32 MIDI Implementation manual
- ALSA sequencer playback, OPL3 voice allocation, keyboard control, directory handling,
  and `hogtool` are original work
