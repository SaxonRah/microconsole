# Building and running MicroConsole

`mc.bat` is the canonical entry point. Run all commands from the repository
root in PowerShell or `cmd.exe`.

## 1. Initialize dependencies

MicroConsole pins MicroRender and MicroWave as Git submodules:

```powershell
.\mc.bat deps
```

Pinned revisions:

```text
MicroRender  61d8d875cbdf605edaa2b6ca2f2e5739b80ee61d
MicroWave    2492cc4026a5e3cdebbac6dea91dbc9eabe88d91
```

The dependency command:

1. verifies the repository has Git metadata and `.gitmodules`;
2. repairs missing MicroRender/MicroWave gitlink index entries when needed;
3. initializes both engine submodules;
4. checks out the exact pinned commits in detached-HEAD mode;
5. initializes MicroRender's pinned Raylib submodule.

MicroWave's nested Raylib copy is intentionally not initialized because
MicroConsole needs only one copy of the same dependency.

## 2. Raylib desktop

### Requirements

- CMake
- a C11 compiler supported by the repository (MSVC is the validated Windows
  path)
- dependencies initialized with `mc.bat deps`

### Build and run

```powershell
.\mc.bat build raylib
.\mc.bat run raylib
```

The executable is normally written to one of:

```text
build-raylib\microconsole_demo.exe
build-raylib\Release\microconsole_demo.exe
```

The desktop demo uses:

- 320x240 RGB565 logical rendering;
- 1024 stress sprites;
- 8-row alternating lace presentation;
- 3x point-filtered window output;
- 32 kHz mono MicroWave audio;
- 512-frame Raylib audio blocks.

Press **Space** to trigger the built-in MicroWave synth SFX.

Extra CMake arguments passed after `build raylib` are forwarded to the
configuration command.

## 3. 16-bit DOS

### Requirements

- Open Watcom C/C++ 1.9-compatible toolchain
- `WATCOM` environment variable pointing at the installation root
- DOSBox or DOSBox-X

Example:

```powershell
$env:WATCOM = 'C:\WATCOM'
```

`DOSBOX_EXE` may be set to an explicit emulator executable. Otherwise the
runner searches for DOSBox-X/DOSBox on `PATH` and common install locations.

### Build

```powershell
.\mc.bat build dos
```

This produces:

```text
build-dos\MCREF.EXE   exact upstream MicroRender stress frontend
build-dos\MCGFX.EXE   MicroConsole frontend with audio compiled out
build-dos\MCDEMO.EXE  MicroRender + MicroWave combined frontend
```

All three share the same separately compiled MicroRender object files. This is
intentional: Open Watcom 16-bit large-model code is sensitive enough to build
and link structure that a single giant `wcl` compile/link command produced a
large performance regression during bring-up.

The renderer objects use the same relevant flags as standalone MicroRender:

```text
-bt=dos -ml -2 -ox -s -w4
GFX_FIXED_NO_INT64
GFX_COLOR_INDEX8=0
GFX_ENABLE_TRIANGLES=1
MR_STRESS_MAX_SPRITES=1024
MR_DOS_TILE_H=16
MR_DOS_VSYNC=0
MR_DOS_PRESENT_MODE=1
```

### Run the combined demo

```powershell
.\mc.bat run dos /sprites 1024 /frames 2100
```

Useful runtime switches:

```text
/sprites N     stress sprite count, clamped to 1..1024
/frames N      frame limit; 0 runs until Escape
/noaudio       skip Sound Blaster init/service
/nohud         disable stress HUD
/notri         disable triangles
/nostats       disable stress metrics
/statsrate N   change metrics sampling interval
```

Press **Escape** to stop early.

### DOS renderer parity tests

```powershell
.\mc.bat run dosref /sprites 1024 /frames 2100
.\mc.bat run dosgfx /sprites 1024 /frames 2100
.\mc.bat run dos /sprites 1024 /frames 2100 /noaudio
.\mc.bat run dos /sprites 1024 /frames 2100
```

The runner intentionally matches MicroRender's standalone DOSBox benchmark
environment:

```text
machine=vgaonly
core=dynamic
cycles=max
frameskip=0
aspect=false
BLASTER=A220 I7 D1
```

Override the CPU budget for reproducible period-machine tests:

```powershell
$env:MC_DOSBOX_CYCLES = 'fixed 12000'
.\mc.bat run dos /sprites 1024 /frames 2100
```

`cycles=max` measures the host/DOSBox combination and should not be described as
performance of a specific historical PC. For unattended automation, set
`MC_DOSBOX_NOPAUSE=1` so the temporary DOSBox session exits without waiting for
a keypress after the program finishes.

## 4. Pico 2 / RP2350

### Validated environment

```text
Board      Pimoroni Pico Plus 2 RP2350
Pico SDK   2.2.0
Toolchain  14_2_Rel1
Ninja      1.12.1
```

The Pico environment helper comes from the pinned MicroWave checkout and uses
the Raspberry Pi Pico VS Code SDK/tool installation when present.

### Build

MAX98357A:

```powershell
.\mc.bat build pico max98357a
```

PCM5102A:

```powershell
.\mc.bat build pico pcm5102a
```

NS4168:

```powershell
.\mc.bat build pico ns4168
```

Build products are under:

```text
pico\build-<device>\microconsole_demo.elf
pico\build-<device>\microconsole_demo.uf2
```

### Flash/run

SWD through CMSIS-DAP/OpenOCD:

```powershell
.\mc.bat run pico max98357a swd
```

Picotool:

```powershell
.\mc.bat run pico max98357a picotool
```

Manual BOOTSEL copy instructions:

```powershell
.\mc.bat run pico max98357a manual
```

`run pico` rebuilds the selected preset before flashing.

### Known-good Pico wiring

ILI9341:

| Pico Plus 2 | ILI9341 |
| --- | --- |
| GP4 | MISO |
| GP5 | CS |
| GP6 | SCK |
| GP7 | MOSI |
| GP8 | RST |
| GP9 | DC |

I2S audio:

| Pico Plus 2 | MAX98357A / PCM5102A / NS4168 |
| --- | --- |
| GP10 | BCLK |
| GP11 | LRC / LRCLK |
| GP12 | DIN / DATA / SDATA |
| GND | GND |

For the MAX98357A breakout used during validation, VIN may be powered from VBUS
while the Pico is USB-powered. Connect a speaker only across the amplifier's
speaker `+` and `-` outputs; a bridge-tied speaker output is not referenced to
Pico ground.

### Default Pico runtime configuration

```text
Logical image      320x240 RGB565
Stress sprites     1024
System clock       300 MHz
ILI9341 SPI        75 MHz
Lace block         8 rows
Panel FRMCTR1      0x1B (stable/default ~70 Hz scan)
Audio              32 kHz mono I2S
Audio block         1024 frames
I2S PIO            PIO1, state machine 0
BCLK/LRCLK/DATA    GP10 / GP11 / GP12
```

CMake cache values such as `MC_AUDIO_RATE`, `MC_AUDIO_BLOCK`,
`MC_STRESS_SPRITES`, `MC_SYS_KHZ`, `MC_LCD_SPI_BAUD`, and `MC_LACE_BLOCK_H`
may be overridden by passing `-D...` arguments after the preset.

Example:

```powershell
.\mc.bat build pico max98357a -DMC_STRESS_SPRITES=512
```

Treat the validated 300 MHz / 75 MHz / FRMCTR1 `0x1B` combination as the known
stable baseline before experimenting with display clocks or panel scan timing.

## 5. Build everything available

```powershell
.\mc.bat build all
```

Raylib is always attempted. DOS is attempted when `WATCOM` is set. Pico is
attempted when the expected Pico SDK installation is present.

## 6. Clean

```powershell
.\mc.bat clean
```

This removes:

```text
build-raylib\
build-dos\
pico\build-*\
```

It does not remove or update dependency working trees.
