# MicroConsole

MicroConsole is a small cross-platform console integration project built around
[MicroRender](third_party/microrender) for graphics and
[MicroWave](third_party/microwave) for audio.

The repository does **not** fork either engine. MicroRender and MicroWave remain
pinned Git submodules; MicroConsole owns the platform glue that runs them at the
same time and is the place where console-level services can be added later.

The current program is deliberately a stress/integration demo rather than a
finished game runtime: the deterministic MicroRender stress scene runs
continuously while the MicroWave music demo plays.

## Validated baseline

As of **2026-09-01**, the combined integration is working on all three targets:

| Target | Graphics | Audio | Validated result |
| --- | --- | --- | --- |
| Pico 2 / Pimoroni Pico Plus 2 | 320x240 RGB565 ILI9341, 1024-sprite 8-row lace | 32 kHz I2S | ~110 FPS with continuous audio |
| 16-bit DOS | 320x240 RGB565 logical image -> RGB332 Mode X | 11.025 kHz Sound Blaster 8-bit DMA | ~92 FPS with continuous audio |
| Raylib desktop | 320x240 RGB565, 1024-sprite 8-row lace | 32 kHz Raylib `AudioStream` | graphics + audio verified working |

The DOS reference/diagnostic builds measure 97 FPS (`MCREF`), 94 FPS
(`MCGFX`), and 95 FPS (`MCDEMO /noaudio`) for the same 1024-sprite workload,
which puts the combined 92 FPS result within a few percent of the renderer-only
baseline.

See [docs/VALIDATION.md](docs/VALIDATION.md) for the exact test commands and
what each number means.

## Quick start

Clone the repository, then initialize the pinned dependencies:

```powershell
.\mc.bat deps
```

### Raylib

```powershell
.\mc.bat build raylib
.\mc.bat run raylib
```

Press **Space** to trigger the MicroWave synth SFX while the music demo is
playing.

### DOS

```powershell
.\mc.bat build dos
.\mc.bat run dos /sprites 1024 /frames 2100
```

The DOS build also produces two renderer-parity diagnostics:

```powershell
.\mc.bat run dosref /sprites 1024 /frames 2100
.\mc.bat run dosgfx /sprites 1024 /frames 2100
.\mc.bat run dos /sprites 1024 /frames 2100 /noaudio
```

### Pico 2

```powershell
.\mc.bat build pico max98357a
.\mc.bat run pico max98357a swd
```

Other audio presets:

```powershell
.\mc.bat build pico pcm5102a
.\mc.bat build pico ns4168
```

Flash methods are `swd` (default), `picotool`, and `manual` BOOTSEL copy.

## Dependency pins

MicroConsole is validated against these exact engine revisions:

- MicroRender: `61d8d875cbdf605edaa2b6ca2f2e5739b80ee61d`
- MicroWave: `2492cc4026a5e3cdebbac6dea91dbc9eabe88d91`

`mc.bat deps` initializes those two gitlinks, checks out the pinned commits, and
initializes the single Raylib checkout nested under MicroRender. It can also
repair the specific case where `.gitmodules` survived a copied/archive tree but
the two mode-160000 gitlink entries did not.

## What belongs here

MicroConsole owns integration and platform policy:

- selecting/building the two engines together;
- display/audio device setup;
- platform scheduling between rendering, presentation, and mixing;
- console-facing input/storage/save/power services as they are added;
- target-specific diagnostics and performance regression tests.

MicroConsole does **not** own the renderer or mixer implementation. Changes that
are generally useful to graphics belong in MicroRender; changes generally useful
to audio belong in MicroWave.

## Repository layout

```text
microconsole/
  mc.bat                  one build/run/dependency entry point
  CMakeLists.txt          Raylib combined target
  pico/                   Pico SDK project and audio-device presets
  scripts/                build, run, flash/tooling helpers
  src/
    dos/                   DOS combined frontend + Sound Blaster transport
    pico/                  RP2350 combined frontend
    raylib/                desktop combined frontend
  docs/
    ARCHITECTURE.md        ownership, data flow, platform resource model
    BUILDING.md            prerequisites and exact build/run options
    VALIDATION.md          measured baseline and regression procedure
    TROUBLESHOOTING.md     failures already encountered and their causes
  CONSOLE_HARDWARE.md      path from integration demo to handheld hardware
  third_party/
    microrender/           pinned git submodule
    microwave/             pinned git submodule
```

## Documentation

- [Building and running](docs/BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Validated performance](docs/VALIDATION.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Handheld hardware roadmap](CONSOLE_HARDWARE.md)

## Current scope

The integration layer is now validated. The next console services are expected
to be input, storage/filesystem, saves/settings, power management, and a launcher
or game packaging policy. Those should remain platform services so game code
need not know whether data came from FAT on an SD card, a host directory, or an
embedded image.
