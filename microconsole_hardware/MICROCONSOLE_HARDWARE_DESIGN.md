# MicroConsole Hardware Design Specification

**Revision:** Current PCB / manufacturing-prep specification  
**Date:** 2026-09-19  
**Repository:** `SaxonRah/microconsole`  
**Hardware source state:** commit `58de74ed0c2ec7d6a54e9369b385123422352d09` (`dnp`)  
**Primary BOM:** `microconsole_hardware/MICROCONSOLE_BOM.csv`

---

## 1. Purpose and source of truth

This document describes the **current MicroConsole PCB that actually exists in KiCad**, not an earlier architecture proposal.

The current production target is the **4.0-inch MicroConsole** built around:

- Pimoroni Pico Plus 2 / RP2350;
- LCDWIKI MSP4021 4.0-inch ST7796S SPI display module;
- MCP23017 digital-button expansion;
- ADS7828 analog acquisition for the two joysticks and volume control;
- PCM5102A audio DAC;
- PAM8406 stereo L/R amplifier;
- PAM8302A center-channel amplifier;
- MCP73833 Li-Ion/Li-Po charger and discrete battery/system power path.

The KiCad design, PCB, and authoritative BOM take precedence over older notes.

### Current project files

```text
microconsole_hardware/
├── MICROCONSOLE_BOM.csv
├── MICROCONSOLE_BOM.xlsx
├── MICROCONSOLE_HARDWARE_DESIGN.md
└── mc_hw/
    ├── top.kicad_sch
    ├── top.kicad_pcb
    ├── top.kicad_pro
    ├── mc_hw.kicad_sch      # Pico + display
    ├── buttons.kicad_sch
    ├── audio.kicad_sch
    ├── battery.kicad_sch
    ├── ERC.rpt
    ├── DRC.rpt
    └── libs/
```

Root hierarchical sheets:

- `PicoScreen` → `mc_hw.kicad_sch`
- `Audio` → `audio.kicad_sch`
- `Battery` → `battery.kicad_sch`
- `Buttons` → `buttons.kicad_sch`

---

## 2. Current validation state

At source commit `58de74e`:

```text
ERC:
0 errors
0 warnings
no ignored checks

DRC:
0 violations
0 unconnected pads
0 footprint errors
no ignored checks
```

A clean DRC does **not** mean the current USB-C connector selection is approved. `J2` remains intentionally deferred until the exact connector, matching symbol, and verified footprint are frozen.

### One manufacturing-flag check still required

As of `58de74e`:

- `A2` is marked **DNP in the schematic**;
- the `A2` PCB footprint instance does not yet carry the PCB `dnp` attribute.

Before generating the final pick-and-place file, verify `A2` is also DNP on the PCB.

---

## 3. Manufacturing strategy

The board is deliberately split between:

1. **factory-assembled SMT / normal PCB-assembly parts**, and
2. **manual-install / DNP mechanical or difficult-to-source parts**.

The Chinese PCB assembler should source and place the normal passives, semiconductors, ICs, ferrites, LEDs, MOSFETs, and—after it is frozen—the USB-C receptacle.

The user will install the mechanical modules, controls, joysticks, speakers/connectors, and other through-hole specialty parts.

### 3.1 Factory-populated parts

Factory assembly should include:

- all resistors `R3–R45`;
- all capacitors used by the current design;
- `D1–D5`;
- `FB1`, `FB4`;
- `Q1`, `Q2`;
- `U1`, `U3`, `U4`, `U5`, `U6`, `U7`;
- `J2` **only after the exact USB-C part is frozen**.

### 3.2 DNP / manually installed parts

These remain in the engineering BOM and remain physically present on the PCB, but are marked **Do Not Populate** for factory assembly:

```text
A2
DS1
LS1
LS2
LS3
RV1
SW2-SW15
SW16
SW17
SW19
TH1
U2
```

Meaning:

| References | Manual-install item |
|---|---|
| A2 | Pimoroni Pico Plus 2 |
| DS1 | LCDWIKI MSP4021 display module |
| SW2-SW15 | E-Switch tactile buttons |
| SW16, SW17 | Hall-effect joysticks |
| RV1 | volume potentiometer |
| SW19 | power slide switch |
| U2 | JST-PH battery header |
| TH1 | thermistor / thermistor interface |
| LS1, LS3 | L/R speaker connector positions |
| LS2 | center-speaker solder connection |

DNP means:

```text
Do not populate = YES
In BOM          = YES
On board        = YES
```

Do **not** exclude these parts from the engineering BOM or PCB.

---

## 4. System architecture

```text
                         +---------------------------+
                         | Pimoroni Pico Plus 2 A2  |
                         | RP2350B module            |
                         +-------------+-------------+
                                       |
            +--------------------------+-------------------------+
            |                          |                         |
            | SPI display              | I2C1                    | I2S
            v                          v                         v
 +---------------------+     +--------------------+    +------------------+
 | LCDWIKI MSP4021 DS1 |     | MCP23017 U3       |    | PCM5102A U5     |
 | ST7796S 480x320     |     | digital buttons   |    | stereo DAC       |
 | XPT2046 touch       |     +--------------------+    +---------+--------+
 | microSD             |                                      |
 +---------------------+                            +----------+----------+
                                                    |                     |
                                                    v                     v
                                           +----------------+    +----------------+
                                           | PAM8406 U7     |    | PAM8302A U6   |
                                           | stereo Class-D |    | mono Class-D   |
                                           +-------+--------+    +-------+--------+
                                                   |                     |
                                              Left / Right          Center speaker

                         I2C1
                           |
                           v
                    +-------------+
                    | ADS7828 U4  |
                    | 8ch 12-bit  |
                    +------+------+ 
                           |
              +------------+-------------+
              |            |             |
          Left stick   Right stick    Volume pot
          SW16         SW17           RV1


 USB-C J2
    |
 USB-VBUS
    |
 MCP73833 U1 charger
    |
   BAT+
    |
 battery / MOSFET power path
    |
 VSYS_BAT
    |
 system + audio power
```

---

## 5. Pico and bus allocation

The design uses the following Pico-side functional allocation.

| Function | Pico GPIO |
|---|---:|
| I2C1 SDA | GP2 |
| I2C1 SCL | GP3 |
| LCD MISO | GP4 |
| LCD CS | GP5 |
| LCD SCK | GP6 |
| LCD MOSI | GP7 |
| LCD RESET | GP8 |
| LCD DC | GP9 |
| I2S BCLK | GP10 |
| I2S LRCLK / LRC | GP11 |
| SD MISO | GP12 |
| SD CS | GP13 |
| SD SCK | GP14 |
| SD MOSI | GP15 |
| Left stick click | GP16 |
| Right stick click | GP17 |
| I2S DATA | GP20 |
| Spare / possible MCP interrupt | GP22 |

Volume is routed as the analog net `VOL_ADC` into the ADS7828 rather than consuming a dedicated Pico ADC input.

### I2C

Current I2C devices:

| Device | Function | Address configuration |
|---|---|---|
| U3 MCP23017 | button GPIO | A2:A0 grounded → `0x20` |
| U4 ADS7828 | analog inputs | A1:A0 grounded → `0x48` |

I2C pull-ups use 4.7 kΩ resistors.

---

## 6. Main computer module

### A2 — Pimoroni Pico Plus 2

**Manufacturer:** Pimoroni  
**MPN:** `PIM724`  
**Value:** `Pimoroni Pico Plus 2`  
**Footprint:** `project_parts:RaspberryPi_Pico_Common_Unspecified`  
**Assembly:** **DNP / manual install**

The PCB intentionally uses the Pico module as a removable/hand-installed module rather than asking the PCB assembler to source and mount it.

The project-local footprint was created from the common Pico footprint used by the board. Its geometry should not be casually regenerated because KiCad has previously churned internal UUID/group metadata even when visible geometry did not change.

### A2 KiCad handling rule

When updating the PCB from the schematic:

```text
Replace footprints with those specified in schematic = OFF
```

unless a footprint change is actually intended.

Before final CPL generation, verify the PCB instance itself is marked DNP.

---

## 7. Display subsystem

### DS1 — LCDWIKI MSP4021

**Manufacturer:** LCDWIKI  
**MPN:** `MSP4021`  
**Display:** 4.0-inch 480×320 TFT  
**LCD controller:** ST7796S  
**Touch:** XPT2046 resistive touch  
**Storage:** onboard microSD interface  
**Supply:** module supports 3.3–5 V  
**Footprint:** `project_parts:LCD_MSP4021_4.0in_SPI_ST7796_1x14_P2.54mm`  
**Assembly:** **DNP / manual install**

The current board is specifically laid out around this 4-inch module.

The custom footprint includes the physical module envelope, mounting holes, display geometry, and connection points.

### Current scope

The 2.8-inch and 2.4-inch ILI9341 concepts are **future variants**. They are not part of the current PCB manufacturing freeze and should not be described as interchangeable with DS1 without a separate mechanically verified board/footprint.

---

## 8. Digital buttons

### U3 — MCP23017

**Manufacturer:** Microchip Technology  
**MPN:** `MCP23017-E/SO`  
**Footprint:** `project_parts:SOIC-28_L18.0-W7.5-P1.27-LS10.3-BL`  
**Assembly:** factory SMT

The MCP23017 provides the console's digital button expansion over I2C.

Button inputs are active-low and use the expander's pull-up capability.

### Button allocation

| MCP pin | Function |
|---|---|
| GPA0 | Up |
| GPA1 | Down |
| GPA2 | Left |
| GPA3 | Right |
| GPA4 | A |
| GPA5 | B |
| GPA6 | X |
| GPA7 | intentionally unused |
| GPB0 | Y |
| GPB1 | Select |
| GPB2 | Start |
| GPB3 | L1 |
| GPB4 | R1 |
| GPB5 | L2 |
| GPB6 | R2 |
| GPB7 | intentionally unused |

GPA7 and GPB7 remain unused.

### SW2-SW15 — tactile buttons

**Manufacturer:** E-Switch  
**MPN:** `TL3301NF160QG`  
**Footprint:** `project_parts:KEY-SMD_4P-L6.0-W6.0-P4.50-LS10.0`  
**Quantity:** 14  
**Assembly:** **DNP / manual install**

Although these switches are SMD, they are intentionally DNP because they are mechanical controls that will be sourced and installed manually.

---

## 9. Joystick and analog subsystem

### Approved joystick families

Only these two joystick options are approved:

1. **Favor Union / Favor Electronics `FJH10K-S2`**
2. **GINFULL `L-5C`**

`FJH10K-S3D` is **not an approved part** and must not appear in the design or BOM as an alternate.

### SW16 / SW17

Current schematic identity:

**Primary manufacturer:** Favor Electronics / Favor Union  
**Primary MPN:** `FJH10K-S2`  
**Approved alternate:** GINFULL `L-5C`  
**Footprint:** `project_parts:GINFULL HallEffect Joystick PTH`  
**Assembly:** **DNP / manual install**

The shared footprint is intended for the physically compatible approved joystick family.

### Current custom-footprint drilling

The current footprint uses the corrected hole classes:

- Hall/signal pins: 1.0 mm drills;
- center-switch pins: 1.2 mm drills;
- large mechanical/mounting posts: 1.5 mm drills.

Pad centers must not be moved without re-verifying both approved joystick samples.

### Stick signal mapping

The four analog axes are routed to ADS7828 inputs:

| ADS7828 channel | Function |
|---|---|
| CH0 | Left stick X |
| CH1 | Left stick Y |
| CH2 | Right stick X |
| CH3 | Right stick Y |
| CH4 | Volume potentiometer |
| CH5 | spare / future analog |
| CH6 | spare / future analog |
| CH7 | spare / future analog |

Stick-click switches are read directly by the Pico:

- left click → GP16;
- right click → GP17.

### U4 — ADS7828

**Manufacturer:** Texas Instruments  
**MPN:** `ADS7828E/2K5`  
**Footprint:** `project_parts:TSSOP-16_L5.0-W4.4-P0.65-LS6.4-BL`  
**Assembly:** factory SMT

The analog rail is identified separately as `3v3_ADC` where used.

---

## 10. Volume control

### RV1 — Bourns PTV09A

**Manufacturer:** Bourns  
**MPN:** `PTV09A-2015F-B103`  
**Value:** 10 kΩ linear  
**Footprint:** `project_parts:PTV09A2015FB103`  
**Assembly:** **DNP / manual install**

The volume control produces the `VOL_ADC` net and is read through ADS7828 channel 4.

The potentiometer remains in the engineering BOM even though it is excluded from factory placement.

---

## 11. Audio subsystem

### Audio architecture

```text
Pico I2S
   |
   v
PCM5102A U5
   |
   +----------------------+
   |                      |
   v                      v
PAM8406 U7           L/R analog sum/filter
   |                      |
L speaker              PAM8302A U6
R speaker                  |
                        center speaker
```

This is a stereo-plus-center architecture. It is not a discrete three-channel digital DAC system; the center channel is derived from L/R analog audio.

### U5 — PCM5102A stereo DAC

**Manufacturer:** Texas Instruments  
**MPN:** `PCM5102APWR`  
**Footprint:** `project_parts:TSSOP-20_L6.5-W4.4-P0.65-LS6.4-BL`  
**Assembly:** factory SMT

I2S:

- BCLK → GP10;
- LRCLK → GP11;
- DATA → GP20.

### U7 — stereo L/R amplifier

**Manufacturer:** Diodes Incorporated  
**MPN:** `PAM8406DR`  
**Footprint:** `project_parts:SOIC-16_L9.9-W3.9-P1.27-LS6.0-BL`  
**Assembly:** factory SMT

The output is BTL. Speaker negative outputs are **not ground**.

### U6 — center amplifier

**Manufacturer:** Diodes Incorporated  
**MPN:** `PAM8302AADCR`  
**Footprint:** `project_parts:SOP-8_L4.9-W3.9-P1.27-LS6.0-BL`  
**Assembly:** factory SMT

### LS1 / LS3 — left and right speakers

**Speaker:** PUI Audio `AS02008MR-2-LWC30`  
**Rating used by design:** 8 Ω, 1 W  
**Assembly:** **DNP / manual/off-board**

The PCB positions use:

**Board header:** Molex `53047-0210`  
**Pitch:** 1.25 mm PicoBlade  
**Footprint:** `Connector_Molex:Molex_PicoBlade_53047-0210_1x02_P1.25mm_Vertical`

Both the speakers and the board headers are intended to be hand-installed rather than sourced by the SMT assembler.

### LS2 — center speaker

**Speaker:** PUI Audio `AS03208AS-HT`  
**Rating used by design:** 8 Ω, 3 W  
**PCB interface:** direct wire-pad footprint  
**Footprint:** `project_parts:Speaker_WirePads_1x02_P3.50mm_D1.0mm`  
**Assembly:** **DNP / manual/off-board**

### Audio layout rules

- keep PCM5102A analog outputs short;
- keep analog audio away from Class-D switching outputs;
- route BTL speaker pairs together;
- never connect a BTL `-` output to GND;
- keep local decoupling physically close to each IC;
- keep amplifier bulk capacitance local to the amplifier supply path.

---

## 12. Battery, charging, and system power

The current power design is no longer merely a parked architecture. It is implemented in the current schematic and PCB.

### Important canonical nets

```text
USB-VBUS
BAT+
SYS_RAW
VSYS_BAT
PWR_SW_GATE
```

`VSYS_BAT` is the canonical system/battery rail name.

There is no separate `AMP_VDD` rail in the current design.

### J2 — USB-C receptacle

**Assembly intent:** factory-installed  
**Status:** **DEFERRED exact component selection**

The current KiCad symbol/footprint pair is:

```text
Connector_USB:USB_C_Receptacle_HCTL_HC-TYPE-C-16P-01A
```

This is a placeholder/current-board implementation, not the final approved connector.

The exact factory-sourced USB-C receptacle must be selected with:

- a matching KiCad symbol;
- a mechanically verified footprint;
- verified locating-post dimensions;
- verified shell-tab geometry;
- verified signal-pad numbering;
- verified board-edge position.

The design has previously exposed symbol/footprint and hole-clearance concerns around J2. A currently clean DRC must not be treated as proof that the part is mechanically qualified.

**Do not release the final manufacturing package until J2 is frozen.**

### U1 — battery charger

**Manufacturer:** Microchip Technology  
**MPN:** `MCP73833-FCI/UN`  
**Package:** MSOP-10  
**Assembly:** factory SMT

`USB-VBUS` feeds the charger input. `BAT+` is the charger/battery-side net.

### U2 — battery connector

**Manufacturer:** JST  
**MPN:** `S2B-PH-K-S(LF)(SN)`  
**Value:** `S2B-PH-K-S`  
**Footprint:** `project_parts:CONN-TH_S2B-PH-K-S-LF-SN`  
**Assembly:** **DNP / manual install**

### Q1 / Q2 — P-channel MOSFETs

**Manufacturer:** Diodes Incorporated  
**MPN:** `DMP2035U-7`  
**Package:** SOT-23  
**Assembly:** factory SMT

Current intended package mapping:

```text
pin 1 = Gate
pin 2 = Source
pin 3 = Drain
```

### D1

**Manufacturer:** Nexperia  
**MPN:** `PMEG40T30ERX`  
**Device:** PMEG40T30ER Schottky  
**Footprint:** Nexperia CFP3 / SOD-123W  
**Assembly:** factory SMT

### D5

**Manufacturer:** Diodes Incorporated  
**MPN:** `BAT54C-7-F`  
**Package:** SOT-23  
**Assembly:** factory SMT

### TH1 — battery thermistor

**Manufacturer:** Vishay / BCcomponents  
**MPN:** `NTCLE201E3103SBA`  
**Type:** 10 kΩ NTC  
**Assembly:** **DNP / manual/off-board**

The current PCB interface is represented by a 1×02 2.54 mm vertical-header footprint.

The exact board-header MPN is not frozen. It may be omitted entirely if the thermistor is soldered directly into the two plated holes.

### SW19 — power switch

**Supplier identity:** SparkFun Electronics  
**Supplier SKU used as MPN field:** `COM-00102`  
**Type:** SPDT slide switch  
**Footprint:** `PCM_SparkFun-Switch:Slide_SPDT_PTH_11.6x4.0mm`  
**Assembly:** **DNP / manual install**

---

## 13. Passive-component standard

The current PCB has been standardized around the actual values and footprints below.

### 13.1 Resistors

All fixed resistors are:

```text
Package: 0603 / 1608 metric
Manufacturer: YAGEO
Tolerance: 1%
Power: 0.1 W
Footprint: Resistor_SMD:R_0603_1608Metric
Assembly: factory SMT
```

Exact frozen MPNs:

| Value | References | YAGEO MPN |
|---:|---|---|
| 470 Ω | R18, R19 | `RC0603FR-07470RL` |
| 1 kΩ | R6, R7, R8, R12, R13, R14, R15 | `RC0603FR-071KL` |
| 2 kΩ | R23, R28 | `RC0603FR-072KL` |
| 3.32 kΩ | R9 | `RC0603FR-073K32L` |
| 4.7 kΩ | R21, R30, R45 | `RC0603FR-074K7L` |
| 5.1 kΩ | R3, R4 | `RC0603FR-075K1L` |
| 10 kΩ | R11, R16, R17, R25, R26, R27 | `RC0603FR-0710KL` |
| 27 kΩ | R22, R29 | `RC0603FR-0727KL` |
| 33 kΩ | R20 | `RC0603FR-0733KL` |
| 68 kΩ | R32 | `RC0603FR-0768KL` |
| 82 kΩ | R33 | `RC0603FR-0782KL` |
| 100 kΩ | R5, R24, R31 | `RC0603FR-07100KL` |

Do not revert these to the older 0805 resistor plan.

### 13.2 100 nF capacitors

References:

```text
C5 C7 C8 C10 C11 C12 C14 C16
C33 C36 C40 C44 C46 C48 C50 C51
```

Frozen part:

```text
Manufacturer: KEMET
MPN: C0805C104K5RAC7210
Capacitance: 100 nF
Voltage: 50 V
Tolerance: 10%
Dielectric: X7R
Package: 0805
Footprint: project_parts:C_0805_2012Metric_KEMET_IPC-B
Assembly: factory SMT
```

### 13.3 10 µF bulk capacitors

References:

```text
C4 C13 C15 C17 C26 C27 C29
C35 C38 C41 C45 C47 C49
```

Frozen part:

```text
Manufacturer: KYOCERA AVX
MPN: 1206ZD106KAT2A
Capacitance: 10 µF
Voltage: 10 V
Tolerance: 10%
Dielectric: X5R
Package: 1206
Footprint: project_parts:C1206
Assembly: factory SMT
```

### 13.4 Miscellaneous 0805 capacitors

| Refs | Value | Manufacturer | MPN | Rating |
|---|---:|---|---|---|
| C18, C19 | 2.2 nF | KEMET | `C0805C222J5GECTU` | 50 V, 5%, C0G/NP0 |
| C23 | 33 nF | KEMET | `C0805C333K5RECAUTO` | 50 V, 10%, X7R |
| C31, C59 | 0.22 µF | TDK | `C2012X7R1H224K125AA` | 50 V, 10%, X7R |
| C32, C72 | 0.47 µF | TDK | `C2012X7R1H474K125AB` | 50 V, 10%, X7R |
| C9, C34, C37, C39, C73 | 1 µF | Murata | `GRM21BR71C105KA01L` | 16 V, 10%, X7R |
| C22, C28 | 2.2 µF | Murata | `GRM21BR71C225KA12L` | 16 V, 10%, X7R |

All use `Capacitor_SMD:C_0805_2012Metric`.

### 13.5 Ferrite beads

References:

```text
FB1
FB4
```

Frozen part:

```text
Manufacturer: TDK
MPN: MPZ1608S601ATA00
Package: 0603
Assembly: factory SMT
```

### 13.6 Status LEDs

| Ref | Color | Manufacturer | MPN | Package |
|---|---|---|---|---|
| D2 | red | Lite-On | `LTST-C190KRKT` | 0603 |
| D3 | green | Lite-On | `LTST-C190KGKT` | 0603 |
| D4 | orange | Lite-On | `LTST-C190KFKT` | 0603 |

Assembly: factory SMT.

---

## 14. Major IC / semiconductor freeze

| Ref | Function | Manufacturer | MPN | Factory |
|---|---|---|---|---|
| U1 | Li-Ion/Li-Po charger | Microchip | `MCP73833-FCI/UN` | yes |
| U3 | GPIO expander | Microchip | `MCP23017-E/SO` | yes |
| U4 | 8-channel ADC | Texas Instruments | `ADS7828E/2K5` | yes |
| U5 | stereo DAC | Texas Instruments | `PCM5102APWR` | yes |
| U6 | center amp | Diodes Inc. | `PAM8302AADCR` | yes |
| U7 | stereo amp | Diodes Inc. | `PAM8406DR` | yes |
| Q1, Q2 | P-channel MOSFET | Diodes Inc. | `DMP2035U-7` | yes |
| D1 | Schottky | Nexperia | `PMEG40T30ERX` | yes |
| D5 | dual Schottky | Diodes Inc. | `BAT54C-7-F` | yes |

---

## 15. Grounding and layout rules

The current design should continue to follow these rules:

- use a solid ground strategy unless a specific datasheet requirement demands otherwise;
- keep analog stick/volume traces away from Class-D switching outputs;
- keep PCM5102A L/R analog traces short;
- route BTL speaker outputs as paired runs;
- never treat a Class-D negative speaker output as GND;
- keep bypass capacitors close to IC power pins;
- keep bulk capacitance local to the relevant amplifier/power load;
- avoid unnecessary rerouting now that ERC/DRC are clean;
- do not change custom mechanical footprints without checking the real part.

---

## 16. KiCad library policy

Project-local libraries:

```text
libs/bom2ecad/project_parts.kicad_sym
libs/bom2ecad/project_parts.pretty
```

Use project-local footprints where the exact current board geometry has been qualified.

Important project-local parts include:

```text
RaspberryPi_Pico_Common_Unspecified
LCD_MSP4021_4.0in_SPI_ST7796_1x14_P2.54mm
GINFULL HallEffect Joystick PTH
PTV09A2015FB103
CONN-TH_S2B-PH-K-S-LF-SN
KEY-SMD_4P-L6.0-W6.0-P4.50-LS10.0
C1206
C_0805_2012Metric_KEMET_IPC-B
```

### Footprint rules

- preserve pad centers unless a verified mechanical correction requires movement;
- electrical pad numbering must agree with the schematic;
- locating posts belong in NPTH where appropriate;
- soldered mechanical tabs may be unnumbered plated pads;
- body outlines belong on Fab/Courtyard/User layers;
- no electrical connectivity through copper graphics;
- verify mechanical assemblies against real parts before fabrication.

---

## 17. BOM and assembly-output policy

`MICROCONSOLE_BOM.csv` is the authoritative engineering BOM.

Do not revive the old draft BOM.

The manufacturing release should produce separate views:

### Factory assembly BOM

Contains only parts the assembler should source/place:

```text
resistors
capacitors
ferrite beads
LEDs
diodes
MOSFETs
U1
U3-U7
J2 after final USB-C freeze
```

### Factory CPL / pick-and-place

Must exclude every DNP part.

Before export, explicitly verify:

```text
A2 is DNP on PCB
DS1 is DNP
SW2-SW17 are DNP
RV1 is DNP
SW19 is DNP
U2 is DNP
TH1 is DNP
LS1-LS3 are DNP
```

### Manual-assembly BOM

Contains the hand-installed parts:

```text
A2   Pimoroni Pico Plus 2
DS1  LCDWIKI MSP4021
SW2-SW15 tactile switches
SW16/SW17 Hall joysticks
RV1  volume potentiometer
SW19 power switch
U2   battery connector
TH1  thermistor/interface
LS1/LS3 L/R speaker connectors + speakers
LS2 center speaker
```

### Engineering BOM

Contains **everything**, including DNP parts and J2 while it is deferred.

---

## 18. Remaining open items

The electrical PCB is currently ERC/DRC clean, but the project is not yet ready for blind manufacturing release.

### 18.1 USB-C J2 — required before fabrication

Freeze one exact USB-C receptacle that the selected Chinese assembler can source.

Then:

1. create/use the exact matching KiCad symbol;
2. create/use the exact matching footprint;
3. verify all signal-pad numbers;
4. verify shield tabs and locating posts;
5. verify board-edge location;
6. update PCB from schematic;
7. rerun ERC and DRC;
8. visually inspect the final footprint against the manufacturer drawing.

J2 remains **factory-populated**, not DNP.

### 18.2 A2 DNP propagation

Ensure the Pico footprint itself carries the PCB DNP attribute before final CPL export.

### 18.3 Mechanical prototype checks

Before a larger PCB order:

- test both approved joystick families for mechanical fit;
- verify stick center and full-travel analog values;
- verify joystick direction/orientation in firmware;
- verify MSP4021 mounting and enclosure clearance;
- verify volume-shaft alignment;
- verify power-switch alignment;
- verify battery-connector access;
- verify speaker connector/cavity arrangement;
- verify center-speaker fit;
- verify thermistor lead/interface arrangement.

### 18.4 Audio prototype checks

- establish safe maximum level for the 1 W L/R speakers;
- confirm center-channel summing balance;
- listen for noise from Class-D output routing;
- verify volume-control range and ADC stability.

---

## 19. Pre-manufacturing release checklist

Before generating the board-house package:

- [ ] exact J2 MPN selected;
- [ ] exact J2 footprint/symbol installed;
- [ ] J2 mechanically checked;
- [ ] A2 PCB footprint marked DNP;
- [ ] all intended manual parts still DNP;
- [ ] all intended SMT parts still populated;
- [ ] ERC = 0 errors / 0 warnings;
- [ ] DRC = 0 violations;
- [ ] unconnected pads = 0;
- [ ] footprint errors = 0;
- [ ] authoritative BOM regenerated/checked;
- [ ] factory BOM excludes DNP;
- [ ] CPL excludes DNP;
- [ ] Gerbers reviewed;
- [ ] drill files reviewed;
- [ ] board outline reviewed;
- [ ] polarity/orientation checked for diodes, LEDs, ICs, USB-C, and battery connector;
- [ ] manual-assembly BOM retained separately;
- [ ] fabrication archive tagged with the exact Git commit.

Expected manufacturing package:

```text
Gerbers
Excellon drill files
factory BOM
CPL / pick-and-place
manual-assembly BOM
full engineering BOM
assembly drawing / DNP reference
source commit identifier
```

---

## 20. Current project status summary

The MicroConsole hardware has moved beyond architecture selection into **manufacturing preparation**.

Current state:

- schematic complete enough to pass ERC with no errors or warnings;
- PCB currently passes DRC with no violations;
- fixed resistors standardized to 0603;
- capacitor families frozen;
- major ICs and semiconductors frozen;
- joystick choice narrowed to exactly two approved families;
- screen and Pico modules frozen;
- manual-install / DNP population strategy established;
- authoritative BOM created;
- USB-C connector is the remaining significant component/footprint freeze before a final assembler package.

The next hardware milestone is:

```text
freeze J2
→ verify DNP/CPL behavior
→ generate factory BOM + CPL + Gerbers/drills
→ review manufacturing package
→ prototype order
```
