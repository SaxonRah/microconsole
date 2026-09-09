# MicroConsole Handheld Hardware Architecture

## Overview

This document defines the current hardware architecture for the MicroConsole handheld based on:

- Pimoroni Pico Plus 2 / RP2350B
- MicroRender for video
- MicroWave for audio
- ILI9341 SPI display with onboard microSD slot
- MAX98357A I2S amplifier
- MCP23017 for 14 digital buttons, with L3/R3 on native Pico GPIO
- PCB0029 / Serial Wombat-style ADC GPIO expander for analog sticks
- One analog potentiometer for physical volume control

The design keeps video, audio, storage, controls, and analog input on separate hardware resources so they do not interfere with each other.

---

## Main Processing Platform

### Pimoroni Pico Plus 2

Primary MCU board:

- RP2350B
- 520 KB SRAM
- 16 MB onboard flash
- 8 MB PSRAM
- USB device support
- SWD programming/debugging

The current MicroConsole examples use about half of internal SRAM, so PSRAM is not currently required for the runtime.

---

## Hardware Resource Allocation

| Resource | Purpose |
|---|---|
| Core 0 | Game logic, MicroRender rasterization, MicroWave audio refill |
| Core 1 | ILI9341 presentation / display transfer support |
| SPI0 | ILI9341 display |
| SPI1 | microSD card |
| PIO1 | I2S audio |
| I2C1 | MCP23017 + analog GPIO/ADC expander |
| Native GPIO | GP16/GP17 for L3/R3 stick clicks |
| ADC2 | Physical volume potentiometer |
| DMA | LCD transfers + I2S audio |

---

## Pico Plus 2 Pin Allocation

| Pico GPIO | Function |
|---|---|
| GP0 | Spare / UART0 TX |
| GP1 | Spare / UART0 RX |
| GP2 | I2C1 SDA |
| GP3 | I2C1 SCL |
| GP4 | ILI9341 MISO |
| GP5 | ILI9341 CS |
| GP6 | ILI9341 SCK |
| GP7 | ILI9341 MOSI |
| GP8 | ILI9341 RESET |
| GP9 | ILI9341 DC |
| GP10 | I2S BCLK |
| GP11 | I2S LRCLK / LRC |
| GP12 | microSD MISO |
| GP13 | microSD CS |
| GP14 | microSD SCK |
| GP15 | microSD MOSI |
| GP16 | L3 / left-stick click |
| GP17 | R3 / right-stick click |
| GP18 | Spare |
| GP19 | Spare |
| GP20 | I2S DATA / DIN |
| GP21 | Spare |
| GP22 | Spare / optional MCP23017 interrupt |
| GP26 / ADC0 | Spare analog |
| GP27 / ADC1 | Spare analog |
| GP28 / ADC2 | Volume potentiometer |

Important change from the earlier audio wiring:

```text
Old I2S DATA: GP12
New I2S DATA: GP20
```

This frees GP12-GP15 as a complete SPI1 bus for the microSD card.

---

# Video

## ILI9341

The display remains on SPI0 using the existing MicroRender wiring.

```text
ILI9341             Pico Plus 2

SDO / MISO   -----> GP4
CS           -----> GP5
SCK          -----> GP6
SDI / MOSI   -----> GP7
RESET        -----> GP8
DC           -----> GP9
GND          -----> GND
VCC          -----> appropriate display supply
LED          -----> existing backlight supply
```

Current MicroConsole settings:

- 320x240 RGB565
- SPI0
- 75 MHz SPI
- DMA-backed transfers
- tiled/lace presentation

MicroRender remains responsible only for rendering and presentation.

---

# Storage

## microSD

The microSD socket on the ILI9341 module uses SPI1 rather than sharing the LCD bus.

```text
microSD              Pico Plus 2

MISO          -----> GP12
CS            -----> GP13
SCK           -----> GP14
MOSI          -----> GP15
GND           -----> GND
```

The SD card should be used for:

- game/content packages
- textures and graphics
- audio assets
- level data
- save files
- screenshots
- logs

Filesystem access must stay outside MicroRender and MicroWave hot paths.

Storage should feed explicit caches and buffers rather than allowing unpredictable SD latency during rendering or audio deadlines.

---

# Audio

## MAX98357A

The MAX98357A remains the default speaker amplifier.

```text
MAX98357A            Pico Plus 2

BCLK          -----> GP10
LRC / LRCLK   -----> GP11
DIN           -----> GP20
GND           -----> GND
VIN           -----> VSYS / suitable supply
```

Speaker wiring:

```text
MAX98357A SPK+ -----> Speaker +
MAX98357A SPK- -----> Speaker -
```

Do not connect either speaker terminal to ground.

MicroWave currently uses:

- standard 3-wire I2S
- 32 kHz sample rate
- mono mix duplicated into both I2S channels
- PIO-generated I2S
- DMA audio output
- 16.16 master-volume control

PCM5102A and NS4168 remain compatible alternate I2S outputs.

---

# Digital Controls

## MCP23017

One MCP23017 provides 14 digital button inputs over I2C.

`GPA7` and `GPB7` are deliberately **not used as inputs**. On affected MCP23017 revisions these pins can disturb SDA/I2C operation when used as inputs, so the design treats them as output-only/reserved.

The two remaining controller buttons, L3 and R3, connect directly to native Pico GPIOs GP16 and GP17.

```text
MCP23017             Pico Plus 2

VDD           -----> 3V3
VSS           -----> GND
SDA           -----> GP2
SCL           -----> GP3

A0            -----> GND
A1            -----> GND
A2            -----> GND
RESET         --10K-> 3V3
```

`GPA7` and `GPB7` must remain configured as outputs or otherwise unused as inputs.

Default address:

```text
0x20
```

Buttons are wired active-low.

Each button connects:

```text
MCP23017 input ---- button ---- GND
```

The MCP23017 internal pull-ups are enabled.

## Button Map

| MCP23017 Pin | Control |
|---|---|
| GPA0 | D-pad Up |
| GPA1 | D-pad Down |
| GPA2 | D-pad Left |
| GPA3 | D-pad Right |
| GPA4 | A |
| GPA5 | B |
| GPA6 | X |
| GPB0 | Y |
| GPB1 | Start |
| GPB2 | Select |
| GPB3 | L1 |
| GPB4 | R1 |
| GPB5 | L2 |
| GPB6 | R2 |
| GPA7 | Reserved / output-only |
| GPB7 | Reserved / output-only |

The two stick-click buttons are connected directly to the Pico:

```text
GP16 ---- L3 switch ---- GND
GP17 ---- R3 switch ---- GND
```

GP16 and GP17 use internal pull-ups and are active-low, matching the MCP23017 button convention.

The MCP23017 can initially be polled rather than interrupt-driven.

### Native Stick-Click Inputs

L3 and R3 do not pass through the MCP23017:

```text
GP16 ---- L3 switch ---- GND
GP17 ---- R3 switch ---- GND
```

Configure GP16 and GP17 as active-low GPIO inputs with pull-ups.

GP22 remains available if an MCP23017 interrupt line is desired later.

---

# Analog Controls

## Two Analog Sticks

Each analog stick requires:

- X axis
- Y axis
- push-button switch

The stick push-buttons are handled directly by the Pico as L3 and R3 on GP16 and GP17.

The four analog axes use an external ADC/GPIO expander.

## PCB0029 / Serial Wombat ADC Expander

Recommended mapping:

```text
PCB0029 / ADC Expander

IO0 -----> Left Stick X
IO1 -----> Left Stick Y
IO2 -----> Right Stick X
IO3 -----> Right Stick Y
IO4 -----> Spare
IO5 -----> Spare
IO6 -----> Spare
IO7 -----> Spare
```

Both sticks use 3.3 V.

```text
LEFT STICK

VCC -----> 3V3
GND -----> GND
VRx -----> ADC Expander IO0
VRy -----> ADC Expander IO1
SW  -----> Pico GP16
```

```text
RIGHT STICK

VCC -----> 3V3
GND -----> GND
VRx -----> ADC Expander IO2
VRy -----> ADC Expander IO3
SW  -----> Pico GP17
```

The remaining analog channels may later be used for:

- analog triggers
- battery monitoring
- sensors
- additional controls

---

# Shared I2C Bus

The MCP23017 and analog expander share I2C1.

```text
Pico GP2 / SDA ----+---- MCP23017 SDA
                   |
                   +---- ADC Expander SDA

Pico GP3 / SCL ----+---- MCP23017 SCL
                   |
                   +---- ADC Expander SCL
```

Use one known set of external pull-ups:

```text
SDA ---- 4.7K ---- 3V3
SCL ---- 4.7K ---- 3V3
```

Avoid stacking unnecessary pull-up resistors from multiple breakout boards.

---

# Physical Volume Control

## Potentiometer

Use a 10K linear potentiometer connected to the Pico's native ADC.

```text
3V3  -------- Pot high terminal

GP28 / ADC2 -- Pot wiper

AGND -------- Pot low terminal
```

Recommended filtering:

```text
GP28 / ADC2 ---- 100 nF ---- AGND
```

Use AGND for the low side of the potentiometer.

Do not use ADC_VREF as the potentiometer supply.

---

## Volume Processing

The potentiometer should use the full 12-bit ADC range rather than converting immediately to the existing integer `0..100` serial-volume scale.

ADC range:

```text
0 .. 4095
```

MicroWave master volume range:

```text
0 .. 65536
```

The ADC reading should be mapped directly into MicroWave's 16.16 master-volume mechanism.

The existing square-law volume behavior should be preserved so the physical control feels similar to the current software control.

Conceptually:

```c
uint32_t adc;
uint32_t control;
int32_t volume;

control =
    (uint32_t)(((uint64_t)adc * SND_VOL_UNITY + 2047u) / 4095u);

volume =
    (int32_t)(((uint64_t)control * (uint64_t)control) >> 16);

snd_set_master_volume(&g_mixer, volume);
```

This gives roughly 4096 physical control positions instead of only 101 integer percentage positions.

The existing MicroWave volume ramp should remain enabled to prevent audible zipper noise and clicks.

The ADC value should also be filtered slightly to suppress potentiometer contact noise.

---

# High-Level Architecture

```text
                     PIMORONI PICO PLUS 2
                          RP2350B
                             |
       +---------------------+----------------------+
       |                     |                      |
      SPI0                  SPI1                   I2C1
    GP4-GP9              GP12-GP15              GP2-GP3
       |                     |                      |
    ILI9341               microSD          +-------+-------+
                                             |             |
                                         MCP23017      ADC Expander
                                         14 buttons      4 axes
                                             |
                                       D-pad / ABXY
                                       Start / Select
                                       L1/R1/L2/R2

                         GP16 ---------------- L3
                         GP17 ---------------- R3


                      PIO1 I2S
                GP10 / GP11 / GP20
                         |
                     MAX98357A
                         |
                       Speaker


                   RP2350 ADC2
                       GP28
                         |
                  10K Volume Pot
```

---

# Software Architecture

Hardware-specific code should remain outside the rendering and audio engines.

Recommended platform modules:

```text
mc_input
    MCP23017 14-button input bank
    native GP16/GP17 stick-click inputs
    analog stick axes
    desktop keyboard/gamepad
    DOS controls

mc_audio
    MicroWave
    physical volume potentiometer
    I2S backend

mc_video
    MicroRender
    ILI9341 presentation

mc_storage
    SPI microSD
    filesystem
    caching

mc_save
    atomic save handling
    settings

mc_power
    battery monitoring
    backlight
    sleep
    shutdown

mc_launcher
    game/content selection

mc_log
    diagnostics

mc_update
    firmware/content update
```

MicroRender should not know about:

- buttons
- SD cards
- batteries
- volume controls

MicroWave should not know about:

- launchers
- filesystems
- game packages

Games should consume platform-neutral MicroConsole services.

---

# Reserved Expansion

The current layout deliberately keeps several Pico pins unused.

Potential future uses include:

- battery voltage / fuel gauge
- backlight PWM
- vibration motor
- headphone detect
- headphone DAC/amplifier
- RTC
- power switch / soft-latch
- MCP23017 interrupt
- recovery input
- additional analog controls
- UART debugging

Currently useful spare GPIOs include:

```text
GP0
GP1
GP18
GP19
GP21
GP22
GP26 / ADC0
GP27 / ADC1
```

---

# Current Design Summary

The intended first handheld configuration is:

- Pimoroni Pico Plus 2
- ILI9341 320x240 display on SPI0
- microSD on SPI1
- MAX98357A on I2S
- MCP23017 providing 14 button inputs
- two analog sticks through an external ADC expander
- stick-click buttons directly on GP16 and GP17
- 10K analog volume potentiometer on GP28 / ADC2
- USB + SWD retained for development and recovery
- no PSRAM requirement for the current MicroConsole examples

The architecture keeps each high-bandwidth or timing-sensitive subsystem on its own hardware resource and leaves additional Pico GPIO available for future console features.

The final 16-button controller input count is:

```text
MCP23017 inputs: 14
Native Pico GPIO: 2  (L3, R3)
--------------------
Total buttons:     16
```

`GPA7` and `GPB7` are not used as inputs.
