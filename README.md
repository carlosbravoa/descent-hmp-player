# Descent HMP Player

A command-line player for the original `.hmp` music files from **Descent 1 & 2**, built
to send audio directly to real MIDI and OPL3 hardware. No emulation, no software
synthesis — just the game's music on the devices it was designed for.

Aimed at two targets:

- **RetroWave OPL3 Express** — direct USB serial playback on real OPL3 hardware, with
  the **correct instrument bank chosen automatically per song** (Descent uses four
  different FM banks across its soundtrack), exactly as it sounded on a real Sound Blaster
- **Roland MT-32 / MT-32pi** — native MT-32 mode with proper track selection and SysEx
  init, or GM/soundfont mode for a modern take

Also includes **`hogtool`**, a simple extractor for Descent's `.hog` archive format,
needed to pull the music and instrument banks out of the game files.

---

## Tools

| Tool | What it does |
|---|---|
| `hmpplay` | Plays `.hmp` files to any ALSA MIDI port (MT-32, GM, Roland GS, …) |
| `hmpplay_opl3` | Plays `.hmp` files directly to a RetroWave OPL3 Express via USB serial, with automatic per-song instrument banks |
| `hogtool` | Lists, extracts, and creates Descent `.hog` archives |

---

## Tested hardware

| Device | Tool | Notes |
|---|---|---|
| RetroWave OPL3 Express | `hmpplay_opl3` | Direct OPL3 FM synthesis via libADLMIDI, auto-selecting each song's authored Descent bank (Int / Ham / Rick / Asterix) |
| Roland MT-32pi (MT-32 mode) | `hmpplay --mt32` | Native MT-32 playback with SysEx init — closest to the original DOS experience |
| Roland MT-32pi (GM mode) | `hmpplay --gm` | GM/soundfont playback — sounds great, slightly less authentic |
| RetroWave OPL3 Express | `hmpplay` + [RetroWave MIDI Proxy](https://github.com/SudoMaker/RetroWaveMIDIProxy) | OPL3 via ALSA MIDI — simpler setup but uses proxy's own sound banks |

---

## Dependencies

```bash
sudo apt install libasound2-dev   # hmpplay only
sudo apt install cmake             # hmpplay_opl3 (builds the bundled libADLMIDI)
# hogtool needs nothing beyond libc
```

`hmpplay_opl3` is built by `./build.sh`, which compiles the bundled `libADLMIDI/`
(patched for the RetroWave's 2 Mbaud serial rate) and then the player. Use
`./build.sh fast` to rebuild just the player after editing it.

---

## Quick start

### 1. Extract the game files

The music, instrument banks, and the `descent.sng` song list are all packed inside the
game's `.hog` archive. Extract everything into one folder so `hmpplay_opl3` can find the
per-song bank list:

```bash
# Build hogtool
gcc -O2 -o hogtool hogtool.c

# Extract everything (songs + .bnk banks + descent.sng) into ./music/
./hogtool extract descent.hog -o ./music/
```

### 2. Play

```bash
# MT-32pi in MT-32 mode
g++ -std=c++17 -O2 -o hmpplay hmpplay.cpp -lasound
./hmpplay --mt32 -p 20:0 ./music/

# MT-32pi in GM mode
./hmpplay --gm -p 20:0 ./music/

# RetroWave OPL3 Express (direct) — bank auto-selected per song from descent.sng
./build.sh
./hmpplay_opl3 ./music/
```

---

## hmpplay

Reads `.hmp` files and sends MIDI events to any ALSA sequencer port. Understands the
HMP file's internal per-device track mapping, so it plays the tracks intended for your
specific hardware rather than sending everything at once.

### Build

```bash
g++ -std=c++17 -O2 -o hmpplay hmpplay.cpp -lasound
```

### Usage

```bash
./hmpplay [options] <file.hmp | directory> [...]
```

```bash
# List available ALSA MIDI output ports
./hmpplay --list

# Play a directory of HMP files
./hmpplay -p 128:0 ./music/

# Loop indefinitely
./hmpplay -l -p 128:0 ./music/

# Play a directory then a specific extra file
./hmpplay -p 128:0 ./music/ credits.hmp
```

### Options

| Option | Description |
|---|---|
| `-p client:port` | ALSA destination port (e.g. `128:0`). Prompts if omitted. |
| `-l` | Loop the playlist indefinitely. |
| `-t scale` | Tempo multiplier. `0.5` = half speed, `2.0` = double. Default: `1.0`. |
| `-v` | Verbose — print device track mapping and every MIDI event. |
| `-D N` | HMP device index for track selection (see table below). |
| `--mt32` | MT-32 mode: select MT-32 tracks (`-D 1`) and send SysEx init. |
| `--gm` | GM mode: select General MIDI tracks (`-D 2`), no SysEx. |
| `--list` | List available ALSA MIDI output ports and exit. |

### Keyboard controls

| Key | Action |
|---|---|
| `n` or `Space` | Next song |
| `p` | Previous song |
| `l` | Toggle loop on / off |
| `+` / `=` | Tempo +25% (max 4×) |
| `-` | Tempo −25% (min 0.25×) |
| `q` or Ctrl-C | Quit |

### Finding your ALSA port

```bash
./hmpplay --list
# or
aplaymidi -l
```

```
Available ALSA MIDI output ports:
  Port   Client Name                    Port Name
  ------  ------------------------------ ----------
  14:0   Midi Through                   Midi Through Port-0
  20:0   USB MIDI Interface             USB MIDI Interface MIDI 1
  128:0  RetroWave MIDI Proxy           RetroWave OPL3
```

### MT-32pi — native MT-32 mode

Descent's music was originally composed for the Roland MT-32. Playing it back in native
MT-32 mode is the closest you can get to the original DOS experience.

Switch your MT-32pi to MT-32 mode, connect via MIDI cable to a USB MIDI interface, then:

```bash
./hmpplay --mt32 -p 20:0 ./music/
```

`--mt32` does three things:

1. **Selects the MT-32 track arrangement** from the HMP file. Each `.hmp` contains
   separate tracks for different hardware (OPL, MT-32, GM, etc.), with patch numbers and
   voice layouts tuned for each device. Without `-D 1` you get a mix of all tracks, which
   sounds wrong on MT-32 hardware.

2. **Sends SysEx initialisation** before the first note plays:
   - Master volume = 100
   - Reverb: Hall mode, time 4, level 4
   - Partial reserve: 4 voices per part across all 8 melody parts

3. **Sends MIDI panic** (all notes off) between tracks.

### MT-32pi — GM / soundfont mode

Switch the MT-32pi to GM mode and use:

```bash
./hmpplay --gm -p 20:0 ./music/
```

`--gm` selects the General MIDI track arrangement and sends no SysEx. It sounds great —
just less strictly faithful to the original than native MT-32 mode.

### RetroWave OPL3 Express via MIDI Proxy

The [RetroWave MIDI Proxy](https://github.com/SudoMaker/RetroWaveMIDIProxy) exposes the
OPL3 Express as a standard ALSA MIDI port. Simpler to set up than `hmpplay_opl3` but uses
the proxy's own sound banks rather than Descent's original FM patches.

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
# Terminal 1
./RetroWaveMIDIProxy

# Terminal 2
./hmpplay --list
./hmpplay -p 128:0 ./music/
```

The proxy must stay running for the duration of playback. For the most authentic OPL3
sound, use `hmpplay_opl3` instead.

---

## hmpplay_opl3

Plays `.hmp` files directly to a RetroWave OPL3 Express over USB serial (`/dev/ttyACM0`),
driving the real YMF262 chip at 2 Mbaud. No MIDI proxy, no ALSA — OPL register writes go
straight to the hardware. FM synthesis is handled by [libADLMIDI](https://github.com/Wohlstand/libADLMIDI),
whose built-in **Descent banks** carry the game's own FM patches.

### The per-song bank — why this sounds right

Descent's soundtrack isn't one instrument set. The game ships **four** FM bank pairs and
assigns each song to one of them in `descent.sng` (its own song→bank list). The intro
uses the *Ham* bank, level 12 uses the *Asterix/melodic* bank, and so on. Play every song
through a single bank — as most OPL players do — and most of the soundtrack comes out on
the wrong instruments.

`hmpplay_opl3` reads `descent.sng` and switches to each song's authored bank
automatically. Just point it at the folder you extracted the HOG into:

```bash
./hmpplay_opl3 ./music/
```

Each track prints the bank it picked, e.g. `Bank: #4 — HMI (Descent:: Ham)`. The four
banks and how the songs split across them:

| `descent.sng` melodic bank | libADLMIDI built-in bank | Songs |
|---|---|---|
| `melodic.bnk` | HMI (Descent, Asterix) | 14 |
| `intmelo.bnk` | HMI (Descent:: Int)    | 8  |
| `hammelo.bnk` | HMI (Descent:: Ham)    | 4  |
| `rickmelo.bnk`| HMI (Descent:: Rick)   | 1  |

> The banks are resolved by **name** from libADLMIDI's table, not by hardcoded index —
> the index numbers in libADLMIDI's `inst_db.cpp` are internal data offsets, *not*
> `adl_setBank()` ids, a trap that silently loads a neighbouring bank.

### .hmp vs .hmq — which arrangement plays

Many songs ship in two arrangements: `.hmp` (OPL2) and `.hmq` (OPL3). They are genuinely
different — `.hmq` has more tracks, mid-song program changes, and uses channel 9 for real
GM percussion, while `.hmp` puts a melodic voice there. On an OPL3 card the game loads
`.hmq`, so **`hmpplay_opl3` prefers `.hmq` when present** (use `--hmp` to force OPL2).

Conveniently, the soundtrack splits cleanly: the 14 *Asterix* songs are `.hmp`-only, and
all 13 *Int/Ham/Rick* songs ship a `.hmq` — so every song plays in its intended form with
no mixing of arrangements.

### Build

```bash
./build.sh          # builds the bundled, baud-patched libADLMIDI, then the player
./build.sh fast     # rebuild just the player after editing it
```

### Usage

```bash
./hmpplay_opl3 ./music/                 # whole soundtrack, per-song banks (recommended)
./hmpplay_opl3 ./music/game01.hmp       # a single song
./hmpplay_opl3 -l ./music/              # loop the whole soundtrack
./hmpplay_opl3 --bank int ./music/      # force one bank for everything (A/B testing)
```

### Options

| Option | Description |
|---|---|
| `-d name` | Serial device name without `/dev/` (default: `ttyACM0`) |
| `--bank NAME` | Force one bank for every song instead of the per-song default: `int`, `ham`, `rick`, `d2`, `gm`. Handy for A/B comparisons. |
| `-b file.wopl` | Force a custom [WOPL](https://github.com/Wohlstand/OPL3BankEditor) bank file for every song |
| `--hmp` | Force the OPL2 `.hmp` arrangement even when a richer `.hmq` exists. By default `.hmq` is preferred — that's what Descent loads on an OPL3 card. |
| `-D N` | HMP device track selection: `0`=OPL (default), `1`=MT-32, `2`=GM, `3`=GS |
| `-l` | Loop the playlist indefinitely |
| `-t scale` | Tempo multiplier (default: `1.0`) |
| `-v` | Verbose |

Keyboard controls are the same as `hmpplay`.

### How close is it?

Very — but not bit-for-bit. libADLMIDI is a clean-room reimplementation of the OPL
synthesis, so a few voices can differ subtly from the original game's HMI driver. For
**byte-exact** output (and faithful `.hmq` playback with real loop points), see the
sibling **`hmisandbox`** project, which runs Descent's actual `HMIMDRV.386` driver in an
x86 sandbox and forwards its register writes to the same RetroWave hardware.

---

## hogtool

Descent packs all its game files — levels, music, bitmaps, instrument banks — into `.hog`
archives. `hogtool` lets you list and extract them on Linux without any Windows tools.

### Build

```bash
gcc -O2 -o hogtool hogtool.c
# No dependencies beyond libc
```

### Commands

```bash
# List contents
./hogtool list descent.hog

# Extract everything
./hogtool extract descent.hog -o ./out/

# Extract specific files (case-insensitive)
./hogtool extract descent.hog -o ./music/ game0.hmp game1.hmp briefing.hmp
./hogtool extract descent.hog -o ./banks/ intmelo.bnk intdrum.bnk

# Create an archive
./hogtool create mymission.hog level01.rdl mymusic.hmp
```

Filenames inside a HOG are capped at 12 characters (DOS 8.3 heritage). `hogtool` warns
and skips any file whose basename exceeds this.

---

## HMP device track mapping

Each `.hmp` file stores separate track arrangements for different hardware. The player
reads a `deviceTrackMappings` table from the HMP header and plays only the tracks
intended for your device:

| Index | Device | `hmpplay` flag | Where the instruments come from |
|---|---|---|---|
| 0 | OPL / AdLib | `-D 0` | FM patches (`hmpplay_opl3` supplies these automatically) |
| 1 | Roland MT-32 | `--mt32` | MT-32 ROM patches on the device |
| 2 | General MIDI | `--gm` | GM patches on the device |
| 3 | Roland GS | `-D 3` | GS patches on the device |
| 4 | Tandy / PS/1 | `-D 4` | — |

This index selects which *track arrangement* to play from the HMP file — it is not a bank
file you pass in. `hmpplay` sends the chosen tracks to your MIDI device (which provides
the sounds); `hmpplay_opl3` always uses the OPL arrangement and supplies the FM bank
itself. Playing `hmpplay` without a `-D` flag merges all tracks, which usually works but
may include device-specific tracks not suited to your hardware.

---

## Format notes

**HOG:**
```
[3 bytes]  magic: "DHF"
then, repeated until EOF:
  [13 bytes]  filename, null-padded (max 12 chars + null)
  [4 bytes]   data length, little-endian uint32
  [N bytes]   file data
```

**HMP** (HMI MIDI Protocol — variant of Standard MIDI):
- Magic: `HMIMIDIP` at offset `0x00`
- Track count: LE uint32 at `0x30`
- Tempo: LE uint32 at `0x38`; time division = `tempo × 1.6`
- Device track map: `[5][32]` LE uint32 array at `0x80` (5 devices × 32 track slots)
- Track data: starts at `0x308`; each track preceded by a 12-byte header
- Delta times: HMI little-endian VLQ (MSB-terminated, least-significant byte first — opposite of standard MIDI)
- Tempo: `1,605,632 µs/beat` (~37.4 BPM), matching DXX-Rebirth's hardcoded SMF value
- Track 0: conductor/setup track, always skipped during playback

**descent.sng** (the song→bank list `hmpplay_opl3` reads):
- Plain text, tab-separated, DOS CRLF line endings
- One line per song: `song.hmp` ⇥ `melodic.bnk` ⇥ `drum.bnk`
- Names exactly which of the four FM bank pairs each song was authored for

---

## Credits

- HMP parsing based on `common/misc/hmp.cpp` from [DXX-Rebirth](https://www.dxx-rebirth.com/)
  by Arne de Bruijn and the JFFEE project (GPL v2+)
- HMP header layout from [ScummVM](https://github.com/scummvm/scummvm)
  `audio/midiparser_hmp.cpp` (GPL v3+)
- HOG format from DXX-Rebirth utilities by Josh Cogliati and Bradley Bell (GPL v2+)
- RetroWave wire protocol from [SudoMaker/RetroWave](https://github.com/SudoMaker/RetroWave)
  `RetroWaveLib/` (AGPLv3)
- MT-32 SysEx structure from the Roland MT-32 MIDI Implementation manual
