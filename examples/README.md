# Console-era examples

Nine MicroConsole programs, each reproducing one display technique from a real
console and pairing it with a sound technique from the same machine.

They are **MicroConsole programs, not MicroRender modes.** Nothing here adds a
mode register, a scanline callback, or a chip emulation to either engine.
MicroRender still only knows how to rasterize RGB565 into a tile; MicroWave
still only knows how to fill a mix block. Every effect below is assembled out
of the primitives the two engines already ship, in the layer that
`docs/ARCHITECTURE.md` says is allowed to have policy.

That constraint is the subject, not a workaround. A raster split is what you
get when the thing driving the renderer is willing to change its mind between
scanlines. Mode 7 is what you get when it recomputes an affine matrix per row.
Neither needs the renderer to know the word "mode", which is exactly why these
could be written without touching MicroRender at all.

## The set

Grouped by generation, oldest first within each group.

| id | hardware | display technique | sound technique |
| --- | --- | --- | --- |
| `a2600-beam` | Atari VCS / 2600 (1977), TIA | no frame buffer: 20-bit mirrored playfield, per-line GRP writes, HMOVE comb | polynomial counters, 5-bit pitch divider, audible detuning in cents |
| `nes-split` | Famicom / NES (1983), 2C02 + 2A03 | mid-frame raster splits, 16x16 attribute clash, 8-sprite line limit | 240 Hz frame sequencer, sweep unit, 16-step triangle, 1-bit DPCM |
| `sms-lock` | Master System (1985), VDP + SN76489 | scroll-lock regions, left-column blanking, chip-wide sprite zoom, RGB222 | periodic noise clocked from tone 3, 60 Hz software envelopes, arpeggio chords |
| `gb-window` | Game Boy DMG (1989) | window layer, per-scanline STAT/SCX ripple, BGP fades, simulated LCD ghosting | wave-RAM morphing, hardware envelope and sweep, 7-bit short-LFSR noise |
| `pce-wavetable` | PC Engine / TG-16 (1987), HuC6270/60/80 | per-line CRAM rewriting via RCR, three scroll splits, 32x64 sprites | six 5-bit wavetable channels, channel-2 LFO crossing into FM, DDA drums |
| `md-linescroll` | Mega Drive (1988), VDP + YM2612 | per-line H-scroll table, per-column VSRAM shear, shadow/highlight, dither transparency | 2-operator FM with feedback, channel-6 8-bit DAC drums, PSG hats |
| `snes-mode7` | SNES (1990), S-PPU + S-DSP | per-scanline affine matrices, 1/distance projection, horizon fog | 4-point Gaussian resampling, echo unit with an 8-tap FIR in the feedback path |
| `snes-colormath` | SNES (1990), S-PPU + S-DSP | main/subscreen add and subtract, HDMA window shapes, per-layer mosaic | pitch modulation between voices, shared noise source, ADSR |
| `neogeo-zoom` | Neo Geo (1990), LSPC + YM2610 | per-sprite shrink tables dropping lines and columns, chained sprite backgrounds | 4-bit ADPCM-A at a fixed rate, pitched ADPCM-B, AY envelope as a waveform |

The Atari 2600 opens the list because everything after it is a reaction to what
it did not have. The PC Engine opens the 16-bit group because it shipped an
8-bit CPU behind a video chip nobody would call 8-bit, which is a fact the
generation labels never handled well.

Home computers were considered and left out. The C64's raster bars and sprite
multiplexing and the Amiga's Copper and Paula are the obvious omissions, and
either would slot straight in as another `ex_*.c` — the brief was consoles.

## Building and running

### Raylib and the headless tool

Add one line to the root `CMakeLists.txt`, after Raylib is added:

```cmake
add_subdirectory(examples)
```

Then the usual:

```powershell
.\mc.bat build raylib
```

Two binaries come out. `microconsole_examples` is the Raylib frontend:

```
--example ID     start on one example
--volume 0..100
--list           print the ids and exit
```

In the window: `[` and `]` step through the examples, `1`-`9` jump straight to
one, `TAB` toggles the overlay and the example's own debug readout, arrow keys
and `Z` interact, `SPACE` fires the example's one-shot, `-`/`+` and `M` are the
volume controls the stock demo already uses.

`mc_example_capture` is the headless tool, described below. Both land in
`build-raylib/examples/` (CMake puts an executable in the binary directory
matching the `add_subdirectory` that declared it), or
`build-raylib/examples/Release/` under MSVC.

### DOS

One extra script, kept separate from `scripts/mc_build.bat` on purpose -- that
script's `:dos` section links MCREF, MCGFX and MCDEMO from one shared set of
MicroRender objects so their frame rates stay comparable, and adding targets
with different flags into the middle of it would break the property it exists
to protect.

```powershell
.\scripts\mc_examples_dos.bat
```

That produces `MCEXDEMO.EXE` (Mode X plus Sound Blaster) and `MCEXCAP.EXE`
(headless capture). It needs one small addition to `src/dos/mc_sb.c` and
`mc_sb.h`, included here: `mc_sb_set_scene()` and `mc_sb_mixer()`. The Sound
Blaster DMA plumbing is MicroConsole's platform glue and should not care which
program is filling the block -- the Raylib frontend already passes whatever
mix scene it likes to `snd_render_one_block()`. The default remains the shared
music demo, so `MCDEMO` is byte-for-byte unchanged.

```
MCEXDEMO /list
MCEXDEMO /example snes-mode7
MCEXDEMO /example nes-split /frames 400 /noaudio
```

Controls are the same as the Raylib build: `[` `]` switch, TAB overlay, arrows
and Z interact, SPACE sfx, `-`/`+` and M for volume, ESC to exit. Input comes
from the INT 9 handler rather than the BIOS queue, because several examples
take held direction keys and the BIOS reports typematic repeat rather than key
state.

The link needs `-zt0`, which puts every data object in a far segment instead
of DGROUP. The nine examples each carry a few KB of palettes, tile art,
wavetables and shrink tables; individually all small, together well past the
64 KB near-data limit. The engines are still built without it so their objects
stay comparable with the stress build.

### Pico

Not wired yet. Two things are already known: the Mode 7 projection fix below
was a 32-bit overflow and mattered there too, and `snes-mode7`'s texture wants
to live somewhere other than SRAM -- the board is a Pico Plus 2 with 8 MB of
PSRAM, and `PICO_BOARD` is already `pimoroni_pico_plus2_rp2350`, so the chip
select is defined and nothing currently initialises it.

## The capture and parity harness

`tools/mc_example_capture.c` runs an example for N deterministic frames and
writes selected frames as PPM and the accompanying audio as a WAV, with no
window, GPU or sound card involved.

Its second job is the reason it exists. Every captured frame is rendered
**twice** — once against a full-height 320x240 tile, the way the Raylib and
Pico frontends drive the renderer, and once through
`gfx_render_tiled_no_clear()` in 16-row bands, the way the DOS frontend does —
and the two images are compared pixel for pixel.

```
mc_example_capture --frames 300 --shot 150
mc_example_capture --example snes-mode7 --frames 240 --shot 60 --shot 180
mc_example_capture --quiet            # parity only, for CI
```

This is not decoration. A per-scanline effect is exactly the kind of code that
quietly assumes it can see the whole frame, and all three ways of getting that
wrong work perfectly on a full-height tile and produce garbage in 16-row bands.
The harness caught two real bugs while these were being written:

- The NES example accumulated its dropped-sprite counter *during* render, so
  the on-screen number differed between the two shapes. Fixing it meant moving
  OAM evaluation into `tick()`, which is also what the real PPU does.
- The Master System example did not paint the border region, and the DOS path
  renders through `no_clear` over a single reused band buffer — so untouched
  pixels showed whatever the previous band left there.

Both would have shown up first on the DOS build. Wire it into CTest and they
show up on any machine:

```cmake
enable_testing()
add_test(NAME example_tile_parity COMMAND mc_example_capture --quiet)
```

## Portability, and how it was checked

The Raylib harness proves an example renders the same picture under a
full-height tile and under 16-row bands. It cannot prove anything about a
16-bit `int`, because the machine it runs on does not have one -- and that is
the entire risk of the DOS target. An expression that overflows a 16-bit int
does not warn. It quietly computes something else.

So `tools/mc_example_capture_dos.c` renders the same deterministic frames on
DOS, writes a PPM and a raw S16 stream, and the host diffs them. It never holds
a whole frame: 320x240 RGB565 is 150 KiB, which is both larger than a real-mode
segment and most of the memory the program has, so the PPM is streamed straight
out of the flush callback as `gfx_render_tiled_no_clear()` delivers bands top
to bottom.

```
MCEXCAP nes-split 85 80          on DOS, writes OUT.PPM and OUT.RAW
mc_example_capture --example nes-split --frames 85 --shot 80 --rate 22050
```

All nine are currently bit-identical across the two: 0 of 76,800 pixels and 0
of 31,195 samples. Getting there found eight classes of bug that no compiler
mentioned:

| what | where |
| --- | --- |
| `(int32_t)(x << 8)` -- the shift happens in `int` before the cast, so any note above ~127 Hz wrapped | 33 sites, every example that plays a pitch |
| `int` locals holding samples -- full scale fills the type exactly, and the gain multiply that always follows wrapped | all nine, plus every return type in `mc_ex_chip.h` |
| `mc_sin`/`mc_cos` returning `int` -- Q15 sits at the type's edge and `* k >> 15` is what every caller does next | shared helpers |
| `ly * 256`, `t * 210`, `t * t` in a sky gradient | `pce-wavetable`, 5,763 pixels wrong |
| `rate * 190` for an echo length -- 4.2 million, so the delay line was nonsense and the echo became a buzz | `snes-mode7` |
| an ADPCM predictor clamped against a bound its type could not reach | `neogeo-zoom` (Watcom did catch this one, as W124) |
| `128 * 256` -- misses a 16-bit int by exactly one | `neogeo-zoom` |
| `sin_a * scale * 90` -- overflows a **32-bit** long, not just 16 | `snes-mode7` |

The last one is the reason this was worth doing. It is a latent bug on the
Pico as well, and a 64-bit host `long` was hiding it completely. Nothing but a
cross-target run finds that.

Two rules fell out, and both are now in `mc_example.h`:

- Anything that can hold a full-scale sample or a Q15 trig result is
  `int32_t`, never `int`.
- Shift before multiplying, so intermediates stay inside 32 bits.

### One deliberate divergence

`snes-mode7`'s field is 256x256 on 32-bit targets and 128x128 on DOS.
`uint8_t tex[65536]` is not merely a big array on a 16-bit int, it is an array
dimension that does not fit in the type used to express it. `M7_TEX` is
`#ifndef`-overridable so the cross-target diff can be run like for like. The
field just wraps twice as often, which is the hardware's own behaviour anyway.

## Speed on DOS

Measured under one DOSBox configuration (`core=dynamic`, `cycles=max`), with
the 1024-sprite stress scene built from the same tree as a control:

| | fps | vs MCREF |
| --- | --- | --- |
| MCREF (1024-sprite stress) | 38.5 | 1.00 |
| `nes-split` | 38.1 | 0.99 |
| `pce-wavetable` | 32.0 | 0.83 |
| `a2600-beam` | 30.6 | 0.79 |
| `neogeo-zoom` | 28.8 | 0.75 |
| `sms-lock` | 27.5 | 0.71 |
| `md-linescroll` | 22.8 | 0.59 |
| `snes-colormath` | 19.5 | 0.51 |
| `snes-mode7` | 18.9 | 0.49 |
| `gb-window` | 18.4 | 0.48 |

The absolute numbers are worthless -- DOSBox at `cycles=max` is not a 386, and
its dynamic core does not model VGA write timing, which is precisely what a
per-pixel renderer spends its time on. The **ratio** is the useful part, and it
says these cost between one and two stress frames each. `docs/VALIDATION.md`
records MCREF at 97 fps on real hardware, so the honest expectation is
somewhere in the 45-95 range, with the same ordering. That wants confirming on
the real machine before anyone believes it.

`gb-window` being the slowest is not the renderer: it runs a full 160x144
per-pixel PPU pass *and* a per-pixel LCD ghosting pass in `tick()`, every
frame, before drawing anything. If it needs to be cheaper, the ghosting is the
thing to drop first.

## Writing another one

`mc_example.h` has the full contract. The short version:

- **`init(w, h, mixer, start_frame)`** — build every sprite, tile and wavetable.
  Allocate nothing; everything lives in file-scope storage, the same rule
  `mr_stress_test_t` and `mw_demo_t` follow. `start_frame` is where the mixer
  already is, because the audio clock does not rewind across a switch.
- **`tick(input)`** — one 60 Hz step, deterministic and integer-only.
- **`render(r)`** — two hard requirements, both checked by the harness. It must
  produce the same picture under one full-height tile and under 16-row bands,
  so keep no state that outlives a row. And it must write **every pixel** of
  every row it is handed, because the DOS frontend reuses one band buffer. An
  example with a border paints the border.
- **`mix(m, user)`** — signature matches `snd_render_one_block()`'s `mix_scene`
  so a frontend passes it straight through. Generated audio goes through
  `snd_touch_block()` and `snd_block_add()`, never `m->block` directly, which
  is what `snd.h` asks for and what keeps it correct if anyone enables the
  wide accumulator.

Shared helpers live in `mc_ex_gfx.c` (tile-aware scanline access, RGB565 colour
math, per-channel quantization for period palettes, integer trig) and
`mc_ex_chip.c` (oscillators, LFSRs tapped the way the real parts were, a
Chamberlin SVF, delay lines, 4-bit PSG attenuation). Neither belongs upstream:
they are decisions about how *these programs* want to draw and sound, and
pushing them into the engines would give them opinions they have spent their
whole design avoiding.

Then add the file to `MC_EXAMPLE_SOURCES` in `examples/CMakeLists.txt`, to the
two `for %%F in (...)` lists in `scripts/mc_examples_dos.bat`, its accessor to
`mc_example.h`, and its entry to the table in `mc_example.c`.

And run both harnesses. The host one catches tile-shape bugs; the DOS one
catches arithmetic that a 32-bit int was hiding.

### One convention worth knowing

`mc_osc_t`'s phase is **0.32** — the whole 32-bit range is exactly one cycle,
so it wraps for free on overflow. Every waveform reads the top bits and nothing
else: a pulse takes 16, the sine table takes 8, a 32-entry wavetable takes 5.
A generator that wants to advance a counter at a rate (a DPCM bit clock, a DAC
feed) should keep its own 16.16 accumulator rather than borrowing `osc.step`,
and two of these examples do exactly that.

## A note on stereo

All three MicroConsole frontends currently initialize the mixer mono. Paula-style
hard panning and the Mega Drive's stereo were headline features of that
hardware, so `mc_out()` takes a pan and applies it when `m->channels >= 2`. It
is written and correct but silent on the current targets. If the Pico goes
stereo it starts working with no changes here; if that is not wanted, the pan
argument can come out.

## Accuracy

These are illustrations, not emulators. The mechanisms are real and the
constraints are the ones the hardware actually imposed — the eight-sprite
scanline limit, the 20-bit playfield, the 32 available pitches, the four-bit
wave RAM, the line-select shrink table — but the numbers are chosen to read
clearly at 320x240 rather than to match a particular chip revision. Where
something is approximated the comment in the file says so, most notably the
SNES Gaussian kernel (generated rather than the 512-entry ROM table) and the
YM2610's ADPCM step ladder (the IMA table its decoder is a close relative of).

Every example is integer-only and seeded, so two builds at one mix rate produce
identical frames and identical samples. That is the property the harness
depends on, and it is the same argument `mw_music_demo.c` makes for being
generated rather than loaded.
