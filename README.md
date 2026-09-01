# MicroConsole Demo

A deliberately small integration project for **MicroRender + MicroWave**.
Both engines are git submodules; this repository owns only the platform glue
needed to run them together.

The demo runs the same deterministic MicroRender stress workload while the
MicroWave synthesized music demo plays continuously.

Targets:

- **Pico 2 / Pimoroni Pico Plus 2**: 320x240 ILI9341, 1024-sprite lace stress
  presentation, PIO+DMA I2S audio. MAX98357A, PCM5102A and NS4168 presets.
- **Raylib**: 320x240 RGB565 stress scene with the same 8-row lace presentation
  and a Raylib AudioStream fed by MicroWave.
- **16-bit DOS**: 320x240 Mode X RGB332 presentation via MicroRender's DOS VGA
  backend and Sound Blaster 8-bit auto-init DMA fed by MicroWave.

## Dependency pins

The initial repository commit pins:

- MicroRender: `61d8d875cbdf605edaa2b6ca2f2e5739b80ee61d`
- MicroWave: `2492cc4026a5e3cdebbac6dea91dbc9eabe88d91`

Initialize them with:

```bat
.\mc.bat deps
```

`deps` intentionally initializes Raylib only through the MicroRender submodule,
rather than downloading the identical nested Raylib checkout twice.

## Build / run

```bat
.\mc.bat build raylib
.\mc.bat run raylib

.\mc.bat build dos
.\mc.bat run dos

.\mc.bat build pico max98357a
.\mc.bat run pico max98357a swd
```

Other Pico I2S sinks:

```bat
.\mc.bat build pico pcm5102a
.\mc.bat build pico ns4168
```

Pico flash methods are `swd` (default), `picotool`, and `manual`.

## Known-good Pico wiring

ILI9341 (MicroRender defaults):

| Pico Plus 2 | ILI9341 |
|---|---|
| GP4 | MISO |
| GP5 | CS |
| GP6 | SCK |
| GP7 | MOSI |
| GP8 | RST |
| GP9 | DC |

I2S audio:

| Pico Plus 2 | MAX98357A / PCM5102A / NS4168 |
|---|---|
| GP10 | BCLK |
| GP11 | LRC / LRCLK |
| GP12 | DIN / DATA / SDATA |
| GND | GND |

For the MAX98357A breakout used during bring-up, VIN can be powered from VBUS
when the Pico is USB-powered. The speaker connects only across the amplifier's
speaker + and - outputs, never from one speaker terminal to Pico ground.

## What this is testing

This is not a new renderer or mixer. It is the first combined "console" load:

1. `mr_stress_tick()` advances the deterministic graphics workload once per
   rendered frame.
2. `mr_stress_render()` rasterizes it using MicroRender.
3. MicroWave continuously fills the platform audio device using
   `mw_demo_mix()`.
4. Display and audio transfer are handled by the platform hardware while the
   CPU continues simulation/raster work.

On Pico the preset is intentionally modeled on the fast stress-lace setup:
1024 sprites, 300 MHz system clock, 75 MHz LCD SPI, 8-row alternating lace
blocks, and the ILI9341 0x10 frame-rate setting. The previous standalone result
was about 110 FPS; the combined demo reports the actual rate so the cost of
continuous audio is measurable rather than assumed.
