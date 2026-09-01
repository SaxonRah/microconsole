# Validated integration baseline

This file records measurements for the combined MicroRender + MicroWave project
so later console work has a concrete regression baseline.

## Baseline date and dependency revisions

Validated **2026-09-01** with:

```text
MicroRender  61d8d875cbdf605edaa2b6ca2f2e5739b80ee61d
MicroWave    2492cc4026a5e3cdebbac6dea91dbc9eabe88d91
```

These are integration measurements, not general claims about every machine or
future engine revision.

## Pico 2

Hardware/configuration:

```text
Board             Pimoroni Pico Plus 2 RP2350
Display           ILI9341 320x240 RGB565
Stress sprites    1024
Presentation      8-row lace, core-1 presenter
clk_sys           300 MHz
LCD SPI           75 MHz
Panel RTNA        0x1B
Audio             32 kHz mono I2S
Audio block       1024 frames
I2S pins          GP10 BCLK, GP11 LRCLK, GP12 DATA
```

Command:

```powershell
.\mc.bat build pico max98357a
.\mc.bat run pico max98357a swd
```

Measured result:

```text
~110 FPS with continuous MicroWave audio
```

Sound was clean and the renderer retained the standalone MicroRender stress-lace
throughput. The serial `underrun` counter should remain zero during a healthy
run.

The physical panel scans its own GRAM near 70 Hz; the ~110 FPS figure describes
render/presentation throughput, not 110 independently visible LCD scans.

## DOS

Runner configuration:

```text
machine=vgaonly
core=dynamic
cycles=max
frameskip=0
aspect=false
BLASTER=A220 I7 D1
```

Workload:

```text
320x240 logical RGB565
16-row tiled Mode X presentation
1024 stress sprites
2100 frames
```

Commands and measured average FPS:

| Command | Purpose | Average |
| --- | --- | ---: |
| `.\mc.bat run dosref /sprites 1024 /frames 2100` | exact upstream MicroRender frontend | 97 FPS |
| `.\mc.bat run dosgfx /sprites 1024 /frames 2100` | MicroConsole loop, audio not compiled/linked | 94 FPS |
| `.\mc.bat run dos /sprites 1024 /frames 2100 /noaudio` | combined executable, audio inactive | 95 FPS |
| `.\mc.bat run dos /sprites 1024 /frames 2100` | renderer + continuous MicroWave/SB audio | 92 FPS |

The combined audio cost in this run is only a few FPS. Audio was clean.

For comparison, the pinned standalone MicroRender checkout measured about
119 FPS at 512 sprites and greater than 100 FPS at 1024 sprites in its own
stress run on the same development machine. The purpose of `MCREF` is to keep
that comparison available from inside the MicroConsole build itself.

### Why the DOS reference binaries stay

Two integration mistakes caused large false regressions during bring-up:

1. inheriting a generic DOSBox configuration instead of forcing the standalone
   MicroRender `vgaonly`/dynamic-core benchmark environment;
2. compiling/linking the entire 16-bit combined program through one large `wcl`
   invocation instead of separately compiling the renderer objects as
   standalone MicroRender does.

At one point the graphics-only combined build was around 22 FPS. The final
object-parity build is back in the mid-90s. `MCREF`, `MCGFX`, and `/noaudio`
make a future recurrence easy to localize.

## Raylib

Command:

```powershell
.\mc.bat build raylib
.\mc.bat run raylib
```

Validated behavior:

- MicroRender 1024-sprite stress scene renders correctly;
- 8-row lace texture updates work;
- MicroWave music plays continuously;
- Space triggers the synth SFX.

No specific desktop FPS is recorded here because the host window/render/audio
stack is not the performance target of this integration exercise.

## Regression procedure

After changing scheduling, storage, input, build flags, or a dependency pin,
rerun at least:

```powershell
# Desktop functional smoke test
.\mc.bat build raylib
.\mc.bat run raylib

# DOS renderer/link/audio isolation
.\mc.bat build dos
.\mc.bat run dosref /sprites 1024 /frames 2100
.\mc.bat run dosgfx /sprites 1024 /frames 2100
.\mc.bat run dos /sprites 1024 /frames 2100 /noaudio
.\mc.bat run dos /sprites 1024 /frames 2100

# Pico combined hardware test
.\mc.bat build pico max98357a
.\mc.bat run pico max98357a swd
```

Watch for:

- a large gap between `dosref` and `dosgfx` -> frontend regression;
- a large gap between `dosgfx` and `MCDEMO /noaudio` -> link/layout regression;
- a large gap between `MCDEMO /noaudio` and `MCDEMO` -> audio service/mix cost;
- Pico `underrun` increasing -> audio refill deadline failure;
- Pico washed-out/white display -> panel/clock/init regression.

For reproducible DOS CPU comparisons, use a fixed `MC_DOSBOX_CYCLES` value.
`cycles=max` intentionally measures the current host/emulator combination.
