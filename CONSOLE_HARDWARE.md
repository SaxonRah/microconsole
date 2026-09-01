# From integration demo to handheld console

MicroRender + MicroWave now provide a validated video/audio baseline. Turning
that baseline into a Game Boy-like handheld is mostly a platform-services,
power, controls, storage, and packaging problem rather than another rendering
or mixing problem.

## Current Pico resource allocation

The combined Pico target currently uses:

| Resource | Current use |
| --- | --- |
| Core 0 | game/stress update, MicroRender rasterization, MicroWave refill |
| Core 1 | ILI9341 lace presentation |
| SPI0 | ILI9341 |
| PIO1 SM0 | I2S audio |
| dynamic DMA channel | ILI9341 pixels |
| dynamic DMA channel | I2S audio |
| DMA IRQ0 | I2S block completion |
| GP4..GP9 | ILI9341 |
| GP10..GP12 | I2S audio |

That leaves the second hardware SPI controller as the natural first choice for
microSD, subject to the final PCB pinout.

## Recommended implementation order

### 1. Controls

Start with direct GPIO:

- D-pad: Up / Down / Left / Right
- A / B
- Start / Select
- optional L / R later

Use a small shared input abstraction from the beginning so Pico GPIO, Raylib
keyboard/gamepad input, and DOS keyboard/controller input produce the same
logical button state.

Prefer real external pulls where appropriate on the final hardware rather than
making game behavior depend on marginal/floating inputs.

### 2. microSD and filesystem

Use the free hardware SPI controller for microSD and FAT during development.
Good SD-resident data includes:

- game/content packages;
- levels/maps;
- large graphics/audio assets;
- save files;
- screenshots;
- logs/diagnostics.

Do **not** put filesystem reads in MicroRender or MicroWave hot paths. Storage
should feed explicit caches/buffers so SD latency cannot stall rasterization or
an audio deadline unpredictably.

A first console does not need arbitrary executable loading from SD. The easiest
first milestone is still firmware-compiled games with SD used for data/assets
and saves. A launcher/module ABI can be designed later when there is a concrete
need for independently loaded programs.

### 3. Persistent settings and saves

Reserve a small onboard nonvolatile region for data that should survive even
without an SD card:

- console settings;
- calibration;
- boot state;
- crash/recovery metadata;
- emergency save metadata.

For normal game saves, expose one platform API with atomic replacement/backup
behavior instead of letting games call FAT directly.

### 4. Battery and power path

A practical handheld needs:

- protected single-cell LiPo/Li-ion;
- charger;
- proper load-sharing/power-path behavior;
- regulator/current budget for Pico + LCD/backlight + audio peaks;
- hard switch or deliberate soft-latch design;
- battery voltage or fuel-gauge measurement;
- low-battery policy and safe-save/shutdown behavior.

Power design should be treated as a system feature. A game should be able to
receive a low-power/suspend event without knowing how the battery is measured.

### 5. Backlight and sleep

Provide:

- PWM brightness control;
- a transistor/switch that can actually remove backlight power in sleep;
- a platform sleep/wake path that coordinates display, audio, storage, and
  pending saves.

PWM-only dimming is not a substitute for being able to shut the backlight off.

### 6. Audio controls and outputs

MicroWave master gain is enough for the first volume-control implementation.
Add volume buttons or a wheel at the console layer.

For speaker output, the current MAX98357A-class I2S amplifier is appropriate.
A bridge-tied speaker output must only drive the speaker across its `+` and `-`
terminals; never connect one speaker terminal to Pico ground.

If headphones are required later, add a suitable DAC/headphone amplifier/output
stage. Do not attach headphones directly to a bridge-tied MAX98357A speaker
output.

### 7. PCB and enclosure

The final board should account for at least:

- microSD socket;
- button contacts/connectors;
- battery connector;
- speaker connection;
- backlight/power circuitry;
- SWD test pads/header;
- USB access;
- mechanical strain relief;
- enclosure screw/support locations.

Keep SWD accessible even after the product has a normal firmware update path.

## Software platform services still missing

A useful target structure is:

```text
mc_input      logical buttons / controller state
mc_storage    block + filesystem access
mc_save       atomic saves and persistent settings
mc_power      battery/backlight/suspend
mc_package    game/content package representation
mc_launcher   game selection and lifecycle
mc_log        diagnostics/crash reporting
mc_update     firmware/content update and recovery
```

These services should be platform-neutral at the game boundary. The backend can
then be:

```text
Pico    GPIO + FAT/SD + flash + battery hardware
DOS     keyboard + DOS filesystem
Raylib  keyboard/gamepad + host filesystem
```

without changing game logic.

## Useful later hardware

Not required for the first playable handheld, but worth reserving design space
for:

- RTC / wake source for clock-based games and timed wake;
- vibration motor + transistor/driver;
- dedicated fuel-gauge IC;
- headphone DAC/amp;
- USB game/save transfer services;
- recovery/update input combination or boot button.

## Architecture rule

Keep console services out of the renderer and mixer.

MicroRender should remain usable without an SD card, battery monitor, or button
API. MicroWave should remain usable without a launcher or filesystem. Games
should see stable console services and not know whether the implementation is
Pico hardware, DOS, or a desktop development host.
