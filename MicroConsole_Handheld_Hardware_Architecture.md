# MicroConsole Handheld Hardware Architecture

## Status

This document describes the **current intended handheld hardware architecture**
and distinguishes proven runtime behavior from hardware that is still only
represented in the schematic.

The PCB is not laid out or routed yet. It currently consists primarily of
footprints imported with **Update PCB from Schematic**.

## Main platform

- Pimoroni Pico Plus 2 / RP2350B
- 520 KB SRAM
- 16 MB flash
- 8 MB PSRAM
- USB device support
- SWD programming/debugging

FastDoom uses PSRAM substantially (including its Doom zone and coherent display
snapshots). Small MicroConsole examples do not necessarily require PSRAM.

## Resource allocation

| Resource | Current intended/proven use |
| --- | --- |
| Core 0 | game logic, rendering, display-side work |
| Core 1 | proven FastDoom MicroWave audio producer |
| SPI0 | ST7796S LCD |
| SPI1 | microSD |
| PIO1 SM0 | I2S audio |
| I2C1 | MCP23017 + PCB0029 |
| GP16/GP17 | L3/R3 stick clicks |
| ADC2 / GP28 | physical volume |
| DMA IRQ0 | I2S completion |
| DMA IRQ1 | display completion |

Scheduling details are frontend-specific; the table captures the proven
FastDoom configuration and hardware ownership, not a requirement that every
example use identical multicore scheduling.

## GPIO allocation

| GPIO | Function |
| --- | --- |
| GP0 | spare / UART0 TX |
| GP1 | spare / UART0 RX |
| GP2 | I2C1 SDA |
| GP3 | I2C1 SCL |
| GP4 | LCD MISO |
| GP5 | LCD CS |
| GP6 | LCD SCK |
| GP7 | LCD MOSI |
| GP8 | LCD RESET |
| GP9 | LCD DC |
| GP10 | I2S BCLK |
| GP11 | I2S LRCLK |
| GP12 | microSD MISO |
| GP13 | microSD CS |
| GP14 | microSD SCK |
| GP15 | microSD MOSI |
| GP16 | L3 |
| GP17 | R3 |
| GP18 | spare |
| GP19 | spare |
| GP20 | I2S DATA |
| GP21 | spare |
| GP22 | spare / optional MCP interrupt |
| GP26 / ADC0 | spare analog |
| GP27 / ADC1 | spare analog |
| GP28 / ADC2 | volume pot |

## Video: ST7796S

The production display is ST7796S, 480x320, on SPI0:

```text
ST7796S             Pico Plus 2

MISO          <---- GP4
CS            <---- GP5
SCK           <---- GP6
MOSI          <---- GP7
RESET         <---- GP8
DC            <---- GP9
GND           <---- GND
VCC           <---- module-appropriate supply
```

FastDoom uses event-driven progressive presentation at approximately 75 MHz
actual LCD SPI clock.

The normal MicroConsole examples retain a 320x240 logical surface and scale in
the Pico presentation layer to fill the 480x320 ST7796S.

## Storage: microSD

The display module's onboard microSD socket is on SPI1:

```text
microSD              Pico Plus 2

MISO          <---- GP12
CS            <---- GP13
SCK           <---- GP14
MOSI          <---- GP15
GND           <---- GND
```

Filesystem work belongs in `mc_storage`, feeding explicit buffers/caches rather
than stalling rendering or audio hot paths.

## Audio: MAX98357A

```text
MAX98357A            Pico Plus 2

BCLK          <---- GP10
LRC/LRCLK     <---- GP11
DIN           <---- GP20
GND           <---- GND
VIN           <---- suitable supply
```

Speaker output is bridge-tied:

```text
SPK+ ---- speaker ---- SPK-
```

Do not ground either speaker lead.

Proven FastDoom audio baseline:

- 32 kHz;
- 512-frame DMA period;
- four-slot ring, target three;
- Core-1 producer;
- DMA IRQ0;
- Nuked OPL synthesis hot path in SRAM.

## Digital controls: MCP23017

One MCP23017 provides fourteen active-low button inputs.

GPA7 and GPB7 are not used as button inputs.

```text
MCP23017             Pico Plus 2

VDD           -----> 3V3
VSS           -----> GND
SDA           -----> GP2
SCL           -----> GP3
A0/A1/A2      -----> GND
RESET         -----> pull high to 3V3
```

Canonical button map:

| MCP pin | Control |
| --- | --- |
| GPA0 | D-pad Up |
| GPA1 | D-pad Down |
| GPA2 | D-pad Left |
| GPA3 | D-pad Right |
| GPA4 | A |
| GPA5 | B |
| GPA6 | X |
| GPA7 | reserved/output-only |
| GPB0 | Y |
| GPB1 | **Select** |
| GPB2 | **Start** |
| GPB3 | L1 |
| GPB4 | R1 |
| GPB5 | L2 |
| GPB6 | R2 |
| GPB7 | reserved/output-only |

Stick clicks:

```text
GP16 ---- L3 ---- GND
GP17 ---- R3 ---- GND
```

Use active-low inputs with pull-ups.

### I2C electrical requirement

MCP23017 and PCB0029 share I2C1:

```text
GP2 SDA ----+---- MCP23017 SDA
            +---- PCB0029 SDA

GP3 SCL ----+---- MCP23017 SCL
            +---- PCB0029 SCL
```

There should be one known set of external pull-ups:

```text
3V3 -- 4.7K -- SDA
3V3 -- 4.7K -- SCL
```

**Current schematic issue:** the DRC indicates the existing 4.7K parts are in
series between the Pico pins and the SDA/SCL bus nets, rather than acting as
pull-ups to 3V3. Correct that before routing.

**Current schematic issue:** MCP23017 RESET is still treated as unconnected in
the imported PCB. Pull it high to 3V3 unless deliberate reset control is added.

## Analog controls: PCB0029

```text
IO0 ---- Left X
IO1 ---- Left Y
IO2 ---- Right X
IO3 ---- Right Y
```

Each joystick:

```text
VCC ---- 3V3
GND ---- GND
VRx ---- assigned PCB0029 input
VRy ---- assigned PCB0029 input
SW  ---- GP16 or GP17
```

Keep I2C pull-ups at 3.3 V. Verify PCB0029's own module supply requirement before
final PCB power routing; the stick supply and the expander supply are separate
questions.

## Volume potentiometer

```text
3V3 -------- 10K linear pot -------- AGND
                    |
                  GP28
```

Recommended ADC filtering:

```text
GP28 ---- 100 nF ---- AGND
```

The filter capacitor is recommended but not yet represented in the current DRC.

Software should keep the full ADC resolution, filter/deadband contact noise, and
map into MicroWave's 16.16 master-volume control while preserving the existing
gain ramp.

## PCB status and DRC interpretation

The current PCB has:

- no final Edge.Cuts;
- no final placement;
- no routing;
- no planes/zones.

Therefore unconnected-item errors are expected. Courtyard, Margin, and silkscreen
violations are also mostly placement/footprint cleanup work at this stage.

Before routing:

1. correct I2C pull-up topology;
2. pull MCP23017 RESET high;
3. decide whether to add the 100 nF volume ADC capacitor;
4. Update PCB from Schematic;
5. draw final board outline;
6. place components around enclosure mechanics.

## Power subsystem still to design

A production handheld still needs a deliberate power subsystem:

- protected single-cell LiPo/Li-ion;
- charger;
- load sharing / power path;
- regulator/current-budget validation;
- hard switch or soft-latch;
- battery measurement/fuel gauge;
- low-battery policy and safe-save behavior;
- backlight PWM and hardware power-off.

Those items are roadmap requirements, not functionality already implemented in
the current schematic.

## Software architecture

```text
mc_input
    MCP23017 buttons
    GP16/GP17 stick clicks
    PCB0029 axes
    calibration
    desktop/DOS backends

mc_audio
    MicroWave
    physical volume
    I2S backend

mc_video
    MicroRender
    ST7796S presentation

mc_storage
    SPI1 microSD
    filesystem
    caching

mc_save
    atomic saves
    settings

mc_power
    battery
    backlight
    suspend/shutdown

mc_package
    content package representation

mc_launcher
    game/content lifecycle

mc_log
    diagnostics

mc_update
    firmware/content update/recovery
```

Keep platform hardware services out of MicroRender and MicroWave. Games should
consume stable MicroConsole services rather than directly owning Pico-specific
hardware.
