# Exact reproduction — running the real driver under emulation

`hmpplay_opl3` (see the main [README](../README.md)) sounds very close to the original, but
it is **not** bit-for-bit identical, and it never can be: libADLMIDI is a clean-room
*reimplementation* of the OPL3, so it re-derives the chip register writes rather than
producing the exact ones Descent produced. That's the inherent ceiling of any
reimplementation.

If you want output that is **identical** to the original game — the same OPL register
stream, the authentic `.hmq` arrangements, and the real loop points — there is only one
way: **don't reimplement the sound code, run it.** Load Descent's actual HMI sound driver,
let *it* generate the OPL writes, and forward those writes verbatim to your OPL3 hardware.

This document explains how to build that yourself. It is a **recipe and architecture
guide, not a downloadable program** — see "Why this isn't shipped" below.

---

## The idea in one paragraph

Descent's music on a real Sound Blaster was produced by the game's HMI sound system —
`HMIMDRV.386` (the HMI MIDI driver) plus the SOS (Sound Operating System) library. DOSBox-X
reproduces the music perfectly because it runs that real code and passes the OPL writes
through untouched. You can do the same on Linux without a full DOS emulator: run **just the
driver** inside a tiny x86 user-space sandbox, intercept every OPL port write it makes, and
re-frame those writes for a RetroWave OPL3 Express. The driver thinks it's talking to a
1994 sound card; it's actually talking to your USB OPL3 board.

---

## Architecture

```
 .hmp / .hmq song  ─┐
                    │   (a small DOS/4GW host program you write, linking the SOS lib)
 HMIMDRV.386  ──────┼──►  x86 sandbox (Unicorn CPU emulator)
 SOS library  ──────┘            │
                                 │  the driver runs for real and writes to OPL ports
                                 ▼
                    trap on I/O to ports 0x388–0x38B (and the OPL3 second bank)
                                 │
                                 ▼
                    RetroWave serial framing  ──►  /dev/ttyACM0 @ 2 Mbaud  ──►  YMF262
```

The moving parts:

1. **A CPU sandbox.** A user-space x86 emulator (we used the
   [Unicorn Engine](https://www.unicorn-engine.org/)) executes the driver's instructions.
   You are *not* emulating a whole PC — only enough of one to keep the driver and a minimal
   DOS/4GW host alive.

2. **A minimal DOS + DPMI service layer.** The driver is a 32-bit protected-mode (DOS/4GW
   "LE") binary. You provide just the services it actually calls: an LE loader, a GDT/LDT
   with the expected selectors, a PSP and environment block, and handlers for the `INT 21h`
   (DOS) and `INT 31h` (DPMI) functions it uses — file open/read/close, memory allocation,
   and so on. This is far less than a real DOS; it's only what this one driver touches.

3. **The real driver, loaded and initialised.** A small host program (built with a DOS
   C compiler and linked against the SOS library) loads `HMIMDRV.386`, brings up the
   library, and starts a song — exactly as the game would.

4. **A timer heartbeat.** HMI sequences music from the PC timer interrupt (IRQ0). You inject
   periodic `INT 8` ticks into the sandbox; each tick advances the SOS sequencer, which
   emits the next batch of OPL writes. Save/restore CPU context around each injected tick so
   the interrupt is transparent to the code it interrupts.

5. **An OPL I/O trap.** Hook the sandbox's port-I/O so every `OUT` to the OPL address/data
   ports is captured instead of going nowhere. Each captured `(register, value)` pair is
   re-encoded into the RetroWave wire framing and sent over USB serial. That framing (and
   the 2 Mbaud requirement) is the same one `hmpplay_opl3` and DOSBox-X use — see the main
   README's notes and the [SudoMaker/RetroWave](https://github.com/SudoMaker/RetroWave) lib.

The output is byte-identical to what the game would have sent the chip, because it *is* what
the game's driver sent the chip.

---

## What you must supply yourself

None of these can be redistributed — you provide them from your own legitimate copy of the
game and from upstream open-source projects:

| Piece | Where it comes from | Notes |
|---|---|---|
| `HMIMDRV.386` | **Your** Descent install (or `descent.hog`) | The HMI MIDI driver. Proprietary game code — not shippable. |
| The SOS library + headers | [Wohlstand/SOSPLAY](https://github.com/Wohlstand/SOSPLAY) | `sosw1cr.lib` etc., used to drive the loaded driver. |
| A DOS C cross-compiler | [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2) | Builds the small DOS/4GW host that links the SOS lib. |
| A CPU emulator | [Unicorn Engine](https://www.unicorn-engine.org/) (`pip install unicorn`) | Runs the x86 instructions in user space. |
| The song data | **Your** `descent.hog`, via `hogtool` | `.hmp`/`.hmq` + `descent.sng`, same as for `hmpplay_opl3`. |
| RetroWave OPL3 Express | hardware | The real YMF262 target. |

---

## Hard-won gotchas (so you don't relearn them)

These are the non-obvious traps we hit getting a real DOS/4GW driver to run in a tiny
sandbox. They'll save you days:

- **The Watcom near-heap sizes itself from the DS segment limit via `LSL`.** A flat 4 GB
  limit makes `LSL` return `0xFFFFFFFF`, which the CRT reads as the `brk` error sentinel, so
  *every* `malloc` fails ("not enough memory"). Returning the right heap-type hint from
  `INT 21h`/`30h` (so the CRT uses the PSP break base instead) fixes it.
- **Selectors matter.** The DOS/4GW environment expects specific LDT selectors for DS/SS/ES,
  CS, the PSP, and the environment segment. Pointers the driver hands back are
  segment-base-relative — resolve them through the LDT, not as flat addresses.
- **The injected timer ISR must be transparent.** Save and restore the full CPU context
  around each `INT 8` you inject, and chain the original handler. A stray `retf`/`#GP` under
  load corrupts state that surfaces much later; roll back the faulting tick if it happens.
- **Case-insensitive file opens.** The driver opens files by uppercase DOS names; your
  `INT 21h` open handler should fall back to a case-insensitive search.
- **The `.hmp` format is HMI's, not standard MIDI.** Magic `HMIMIDIP` at `0x00`, track count
  (LE u32) at `0x30`, tempo at `0x38`, track data from `0x308` with 12-byte per-track
  headers; delta times are HMI little-endian VLQ (MSB-terminated, least-significant byte
  first — the opposite of standard MIDI). The bundled SOS song loaders don't recognise
  Descent's revision of the header, so your host parses the song itself and feeds events to
  the driver through the SOS MIDI entry point, timed off the SOS timer service.

---

## Result and caveats

When it works, the OPL register stream is identical to DOSBox-X's `oplemu=retrowave_opl3`
pass-through — i.e. identical to the game — including the OPL3 `.hmq` arrangements and the
real in-song loop points that a pure MIDI/`.hmp` path can't recover.

The cost is a much heavier setup (a DOS cross-compiler, the SOS lib, an emulator, your own
driver binary) and a good deal of low-level DOS/DPMI plumbing. For everyday listening,
`hmpplay_opl3` is the practical choice and gets you most of the way; reach for this only
when you specifically need byte-exact output.

---

## Why this isn't shipped

The approach inherently **runs Descent's proprietary `HMIMDRV.386` driver binary**, which is
copyrighted game code and cannot be redistributed. The game's music data (`.hmp`/`.hmq`,
`.bnk`, `descent.sng`) is likewise non-redistributable. So there is no prebuilt download
here — this guide deliberately ships **knowledge only**. Everything copyrighted stays on
your own machine, sourced from your own legal copy of Descent. The `hmpplay_opl3` player in
this repo is the redistributable option.
