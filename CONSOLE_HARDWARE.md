# MicroConsole Hardware Status and Bring-Up Plan

## Current status

MicroConsole now has a validated Pico Plus 2 / RP2350B software baseline:

- MicroRender video
- MicroWave audio
- FastDoom integration
- ST7796S 480x320 LCD
- MAX98357A I2S speaker output
- microSD wiring reserved on SPI1
- MCP23017 digital controls
- PCB0029 / Serial Wombat analog-axis expansion
- physical volume potentiometer

The PCB is **not yet laid out or routed**. The current `.kicad_pcb` is primarily
the result of **Update PCB from Schematic**, with footprints placed only well
enough to inspect connectivity. Large numbers of unrouted DRC items are therefore
expected until placement, board outline, planes, and routing are completed.

## Current Pico resource allocation

| Resource | Current use |
| --- | --- |
| Core 0 | Game logic / rendering and display-side work |
| Core 1 | Proven FastDoom MicroWave audio producer |
| SPI0 | ST7796S LCD |
| SPI1 | microSD |
| PIO1 SM0 | I2S audio |
| DMA IRQ0 | I2S block completion |
| DMA IRQ1 | display DMA completion |
| I2C1 | MCP23017 + PCB0029 |
| ADC2 | physical volume potentiometer |

The exact CPU split is frontend-specific. The table above describes the proven
FastDoom Pico configuration; smaller examples do not need to reproduce that
scheduling internally.

## GPIO allocation

| Pico GPIO | Function |
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
| GP11 | I2S LRCLK / LRC |
| GP12 | microSD MISO |
| GP13 | microSD CS |
| GP14 | microSD SCK |
| GP15 | microSD MOSI |
| GP16 | L3 / left-stick click |
| GP17 | R3 / right-stick click |
| GP18 | spare |
| GP19 | spare |
| GP20 | I2S DATA / DIN |
| GP21 | spare |
| GP22 | spare / optional MCP23017 interrupt |
| GP26 / ADC0 | spare analog |
| GP27 / ADC1 | spare analog |
| GP28 / ADC2 | volume potentiometer |

## Display

Production handheld display:

```text
ST7796S 480x320           Pico Plus 2

MISO                <---- GP4
CS                  <---- GP5
SCK                 <---- GP6
MOSI                <---- GP7
RESET               <---- GP8
DC                  <---- GP9
GND                 <---- GND
VCC                 <---- module-appropriate supply
```

The working MicroRender / FastDoom path runs the LCD at approximately 75 MHz
actual SPI clock. FastDoom uses event-driven progressive presentation. Temporal
lace modes remain useful MicroRender experiments, but they are not the production
FastDoom presentation mode.

The normal MicroConsole examples remain logically 320x240 and are scaled by the
shared Pico presentation layer to fill the ST7796S 480x320 panel.

## microSD

The display module's microSD socket uses SPI1:

```text
microSD                  Pico Plus 2

MISO                <---- GP12
CS                  <---- GP13
SCK                 <---- GP14
MOSI                <---- GP15
GND                 <---- GND
```

Storage should feed explicit caches/buffers. Filesystem access should not be
performed from MicroRender or MicroWave hot paths.

## Audio

```text
MAX98357A                 Pico Plus 2

BCLK                <---- GP10
LRC / LRCLK         <---- GP11
DIN                 <---- GP20
GND                 <---- GND
VIN                 <---- suitable module supply
```

Speaker:

```text
MAX98357A SPK+ ---- speaker +
MAX98357A SPK- ---- speaker -
```

Never connect either bridge-tied speaker terminal to Pico ground.

The proven FastDoom audio configuration is:

- 32 kHz
- 512-frame DMA period
- four-slot ring, target three
- Core-1 producer
- Nuked OPL hot path in SRAM
- DMA IRQ0 for audio completion

## Digital controls

One MCP23017 supplies 14 active-low button inputs. GPA7 and GPB7 are reserved /
not used as inputs.

Canonical map:

| MCP23017 pin | Control |
| --- | --- |
| GPA0 | D-pad Up |
| GPA1 | D-pad Down |
| GPA2 | D-pad Left |
| GPA3 | D-pad Right |
| GPA4 | A |
| GPA5 | B |
| GPA6 | X |
| GPA7 | reserved / output-only |
| GPB0 | Y |
| GPB1 | Select |
| GPB2 | Start |
| GPB3 | L1 |
| GPB4 | R1 |
| GPB5 | L2 |
| GPB6 | R2 |
| GPB7 | reserved / output-only |

Stick clicks:

```text
GP16 ---- L3 switch ---- GND
GP17 ---- R3 switch ---- GND
```

Configure those native inputs active-low with pull-ups.

### MCP23017 bus and power

```text
MCP23017              Pico Plus 2

VDD              ---- 3V3
VSS              ---- GND
SDA              ---- GP2
SCL              ---- GP3
A0               ---- GND
A1               ---- GND
A2               ---- GND
RESET            ---- pull high to 3V3
```

Use the MCP23017 internal GPIO pull-ups for the active-low buttons.

### IMPORTANT schematic correction still required

The intended I2C pull-ups are:

```text
3V3
 |
4.7K
 |
 +---- SDA ---- GP2 / MCP23017 / PCB0029

3V3
 |
4.7K
 |
 +---- SCL ---- GP3 / MCP23017 / PCB0029
```

The current DRC shows the two 4.7K resistors electrically **in series** between
the Pico GPIO pins and the bus nets instead of connected from SDA/SCL to 3V3.
That should be corrected in the schematic before routing.

The MCP23017 RESET pin should also be pulled high to 3V3 (for example 10K) unless
a deliberate reset-control circuit is added. The current PCB import still treats
that pin as unconnected.

## Analog sticks

Four axes use PCB0029 / Serial Wombat:

```text
IO0 ---- Left Stick X
IO1 ---- Left Stick Y
IO2 ---- Right Stick X
IO3 ---- Right Stick Y
```

Sticks themselves run from 3.3 V:

```text
LEFT STICK
VCC ---- 3V3
GND ---- GND
VRx ---- IO0
VRy ---- IO1
SW  ---- GP16

RIGHT STICK
VCC ---- 3V3
GND ---- GND
VRx ---- IO2
VRy ---- IO3
SW  ---- GP17
```

PCB0029 shares I2C1 with the MCP23017. Keep the I2C bus pull-ups at 3.3 V. Verify
the PCB0029 module's own supply requirement before finalizing its VBUS/3V3 power
connection; do not infer that from the joystick supply voltage.

## Physical volume

```text
3V3 -------- 10K linear pot high
GP28 ------- pot wiper
AGND ------- pot low
```

Recommended addition:

```text
GP28 ---- 100 nF ---- AGND
```

The 100 nF capacitor is a recommendation; it is not yet represented in the
current board DRC.

Use the full ADC range and map it into MicroWave's 16.16 master-volume control.
Keep a small ADC filter/deadband and the existing audio gain ramp.

## PCB state

Current PCB state:

- schematic imported;
- footprints present;
- no final board outline;
- no intentional final placement;
- no routing;
- no planes/zones;
- no final silkscreen cleanup.

Therefore a DRC with many unconnected pads and mechanical overlaps is expected.
At this stage the DRC is useful mainly for verifying net assignments.

Recommended order:

1. finish schematic electrical corrections;
2. Update PCB from Schematic;
3. define enclosure and `Edge.Cuts`;
4. place controls, screen, Pico, SD, audio, and power around the enclosure;
5. resolve mechanical/courtyard conflicts;
6. route critical buses and power;
7. add ground/power zones;
8. clean silkscreen;
9. run final DRC.

## Power, battery, and enclosure work still missing

The current board is not yet a complete battery-powered handheld power design.
A final handheld still needs:

- protected single-cell LiPo/Li-ion strategy;
- charger;
- load sharing / power path;
- regulator/current-budget validation;
- hard switch or deliberate soft-latch;
- battery measurement/fuel gauge;
- low-battery and safe-save behavior;
- backlight PWM and true backlight power-off;
- battery connector;
- SWD access;
- USB access;
- speaker mounting;
- enclosure screw/support locations.

## Platform-service architecture

Keep console services outside MicroRender and MicroWave:

```text
mc_input      controls and calibration
mc_storage    SD/block/filesystem access
mc_save       atomic saves/settings
mc_power      battery/backlight/suspend
mc_package    game/content packaging
mc_launcher   game selection/lifecycle
mc_log        diagnostics
mc_update     firmware/content update/recovery
```

Games should consume stable platform-neutral services. MicroRender should not
depend on input/storage/power, and MicroWave should not depend on launcher or
filesystem policy.
