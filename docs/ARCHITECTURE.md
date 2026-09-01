# Architecture

## Goal

MicroConsole proves that MicroRender and MicroWave can run together as the core
video/audio services of a small game-console runtime without merging the two
engines or making either one aware of the other.

The project should remain a thin integration and platform-policy layer.

```text
                         game / console code
                                |
                  +-------------+-------------+
                  |                           |
             MicroRender                   MicroWave
             graphics/sim                  mixer/sequencer
                  |                           |
            display backend               audio backend
                  +-------------+-------------+
                                |
                          MicroConsole
                    platform scheduling/glue
```

## Ownership boundary

### MicroRender owns

- RGB565 software rendering;
- stress workload generation;
- sprites/RLE/tilemap/triangles;
- ILI9341 transfer backend;
- DOS Mode X presentation backend;
- renderer-specific performance work.

### MicroWave owns

- mixer and sample representation;
- synth/sequencer;
- music demo;
- sample packing helpers;
- reusable I2S PIO program;
- mixer-specific performance work.

### MicroConsole owns

- deciding when to render and when to service audio;
- connecting those engines to one target at the same time;
- platform resource ownership (cores, DMA channels, PIO, GPIO);
- build/run policy and cross-engine regression tests;
- future console services such as input, storage, saves, power, and launching.

A generally reusable graphics change should go upstream to MicroRender. A
generally reusable audio change should go upstream to MicroWave. Only the code
that coordinates the two belongs here.

## Shared workload

All three current frontends run the same conceptual loop:

```text
service/refill audio if needed
advance one deterministic MicroRender stress tick
render the current stress frame
present the target-specific portion of that frame
service/refill audio if needed
repeat
```

The stress workload is intentionally frame-coupled: one `mr_stress_tick()` is
performed for each rendered benchmark frame. That is benchmark policy, not the
future game simulation policy.

## Raylib frontend

`src/raylib/main.c` is the simplest integration reference.

Graphics:

1. MicroRender rasterizes a complete 320x240 RGB565 frame into `g_render`.
2. `present_lace()` copies alternating 8-row groups into `g_present`.
3. Those row groups are uploaded to a Raylib RGB565 texture.
4. The texture is drawn at 3x scale with point filtering.

Audio:

1. MicroWave renders 512 mono samples at 32 kHz.
2. `snd_pack_float()` converts the mixer samples for Raylib.
3. `UpdateAudioStream()` feeds the processed audio buffer.
4. `audio_service()` can run before and after rendering so graphics does not
   unnecessarily delay the next audio block.

This target is useful as a host-side functional reference because it has no
hardware DMA/PIO/VGA ownership problems.

## DOS frontend

DOS uses Open Watcom 16-bit large-model code and MicroRender's 320x240 Mode X
backend.

### Graphics path

```text
mr_stress_tick
       |
gfx_render_tiled_no_clear       320x16 RGB565 working tile
       |
dos_vga_flush_tile
       |
RGB565 -> fixed RGB332 palette
       |
320x240 unchained VGA / Mode X
```

A full 320x240 RGB565 framebuffer would be 150 KiB, so the optimized DOS path
renders/presents 16-row tiles rather than requiring one ordinary 16-bit memory
object to hold the entire image.

### Audio path

The current DOS transport in `src/dos/mc_sb.c` uses:

```text
MicroWave                    11,025 Hz mono
    |
512-sample mix block
    |
snd_pack_u8
    |
1,024-byte double buffer
    |
8237 8-bit auto-init DMA
    |
Sound Blaster DSP
```

`mc_sb_service()` polls the DMA count once per graphics frame and refills the
half that is no longer being played. This is deliberately small and has proved
adequate at the validated ~92 FPS combined rate. It is not an interrupt-driven
Sound Blaster subsystem.

### Three-executable parity harness

The DOS build retains three frontends because they are valuable regression
controls:

```text
MCREF.EXE   upstream MicroRender dos_stress_app.c
MCGFX.EXE   MicroConsole main loop; audio removed at compile time
MCDEMO.EXE  full MicroRender + MicroWave integration
```

All three link the same separately compiled MicroRender `.obj` files. This was
necessary to recover renderer parity: a previous one-shot `wcl` compile/link
structure cut the combined renderer rate roughly in half even with audio
disabled.

The runner also forces the known-fast Mode X emulator setup:

```text
machine=vgaonly
core=dynamic
```

Do not remove the reference targets merely because the current numbers are
good; they make future regressions attributable.

## Pico 2 frontend

The Pico frontend is the most console-like target because rendering,
presentation, and audio transfer overlap in hardware.

### Resource map

| Resource | Owner | Use |
| --- | --- | --- |
| Core 0 | MicroConsole/MicroRender/MicroWave | stress update, rasterization, audio refill |
| Core 1 | MicroConsole display presenter | waits on and sends alternating LCD lace groups |
| SPI0 | MicroRender ILI9341 backend | LCD command/pixel traffic |
| one dynamically claimed DMA channel | ILI9341 backend | 16-bit LCD pixel transfer |
| PIO1 SM0 | MicroWave I2S program | BCLK/LRCLK/DATA generation |
| one dynamically claimed DMA channel | MicroConsole audio | I2S sample stream |
| DMA IRQ0 | MicroConsole audio | block completion/refill request |
| GP4..GP9 | ILI9341 | MISO, CS, SCK, MOSI, RST, DC |
| GP10..GP12 | I2S | BCLK, LRCLK, DATA |

The LCD initializes first and claims an unused DMA channel. Audio then claims a
different unused channel, avoiding hard-coded channel collisions.

### Graphics pipeline

Two full 320x240 RGB565 buffers are reserved:

```text
core 0                               core 1
------                               ------
render frame N+1 into buffer B       present lace groups from frame N / buffer A
        |                                      |
        +-------------- swap ------------------+
```

Only alternating 8-row groups are sent each presentation. This is the same
high-throughput lace idea used by the validated MicroRender Pico stress path.
Core 1 owns the panel while presenting so core 0 can rasterize the next frame.

The renderer/presentation workload measures about 110 FPS even though the
ILI9341 itself scans its GRAM near 70 Hz; 110 FPS is a throughput result, not
110 distinct images visibly scanned by the LCD.

### Audio pipeline

MicroWave uses two 1024-frame mix/I2S buffers at 32 kHz.

```text
MicroWave mix buffer -> packed 32-bit I2S words -> DMA -> PIO1 SM0 -> codec/amp
```

On DMA completion the IRQ switches to the prepared buffer and marks the old one
for refill. Core 0 calls `audio_service()` around rendering/presentation work.
If the next buffer is unexpectedly not ready, the IRQ repeats the current block
instead of stopping I2S clocks and increments `g_audio_underruns`.

The serial diagnostic line includes the underrun counter:

```text
stress frame=... fps=... avg=... audio=... underrun=...
```

A stable run should keep `underrun=0`.

### Warm SWD resets

`recover_warm_boot()` resets core 1 and clears/aborts stale DMA IRQ/channel state
before reinitialization. This matters because the normal development path uses
SWD programming/reset without necessarily power-cycling the RP2350 or LCD.

### Clock policy

The validated Pico baseline is:

```text
clk_sys   300 MHz
clk_peri  sourced from clk_sys
SPI       75 MHz
ILI9341 FRMCTR1 RTNA 0x1B
```

After changing `clk_sys`, MicroConsole explicitly re-sources `clk_peri` so the
SPI baud generator can actually derive the requested 75 MHz rate.

Do not confuse renderer throughput with panel scan rate. Raising the ILI9341
FRMCTR1 scan rate to `0x13`/`0x10` was tested during MicroRender development and
produced a brighter/washed-out image; `0x1B` is the stable baseline.

## Future console platform layer

The next services should sit alongside video/audio rather than inside either
engine:

```text
mc_input      buttons / controller abstraction
mc_storage    block/filesystem abstraction
mc_save       atomic saves + settings
mc_power      battery/backlight/sleep policy
mc_package    game/assets package policy
mc_launcher   game selection / lifecycle
```

The important rule is that game code should consume these services through
stable APIs. A game should not need separate logic for FAT-on-SD, a Windows
folder, DOS files, or an embedded Pico image.
