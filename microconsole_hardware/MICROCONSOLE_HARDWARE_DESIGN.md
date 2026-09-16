# MicroConsole Hardware Design Specification

**Revision:** Pre-routing architecture / BOM freeze draft  
**Date:** 2026-09-16  
**Goal:** one production-oriented hardware plan for the 4", 2.8", and 2.4" MicroConsole variants, optimized for hand assembly.

## 1. Design rules

- Keep the **Pimoroni Pico Plus 2** as a module.
- Keep each **ILI9341/ST7796S display as a complete module**.
- Use a common electrical display interface but a **separate verified footprint for each physical 4", 2.8", and 2.4" module**.
- Remove the Serial Wombat/PCB0029 from the final design.
- Prefer 0805/1206 passives, SOIC/SOP ICs, and TSSOP down to 0.65 mm pitch.
- Avoid QFN/DFN/BGA/WLCSP and difficult exposed-pad packages where a reasonable hand-solderable alternative exists.
- Speakers and joysticks are mechanical assemblies; verify final footprints from real samples before fabrication.
- Battery/charging changes are **PARKED** until the control/audio redesign is settled.

Status terms: **FROZEN** = good next-PCB choice; **PROTOTYPE** = architecture chosen but must be measured/tested; **TBD** = part not selected; **PARKED** = intentionally deferred.

## 2. System block diagram

```text
Pimoroni Pico Plus 2
 |
 +-- SPI0 --> ILI9341 / ST7796S display module
 |
 +-- SPI1 --> microSD
 |
 +-- I2C1 --> MCP23017 --> buttons
 |           |           --> ADS7828 --> left X/Y, right X/Y, volume, spare analog
 |
 +-- GP16 --> left stick click
 +-- GP17 --> right stick click
 |
 +-- I2S --> PCM5102A stereo DAC
              |
              +--> PAM8406 --> L 1W/8R + R 1W/8R
              |
              +--> L+R resistor sum + low-pass
                    --> PAM8302A --> center 3W-rated/8R
```

## 3. GPIO / bus map

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
| I2S LRCLK/LRC | GP11 |
| SD MISO | GP12 |
| SD CS | GP13 |
| SD SCK | GP14 |
| SD MOSI | GP15 |
| Left stick click | GP16 |
| Right stick click | GP17 |
| I2S DATA | GP20 |
| Spare / possible MCP interrupt | GP22 |
| Spare if volume moves to ADS7828 | GP28 |

I2C addresses: MCP23017 = **0x20** with A2:A0 grounded; ADS7828 = **0x48** with A1:A0 grounded.

---

## 4. Display subsystem

Use one generic schematic symbol for the common electrical interface:

`LCD_MISO, LCD_CS, LCD_SCK, LCD_MOSI, LCD_RESET, LCD_DC, VCC, GND`, plus `SD_MISO, SD_CS, SD_SCK, SD_MOSI`, and optional touch `T_CLK, T_CS, T_DIN, T_DO, T_IRQ`.

Touch-controller directions at the module are: T_CLK input, T_CS input, T_DIN input, T_DO output, T_IRQ output.

Create three physical footprints rather than one universal footprint with alternate headers:

| Console | Controller | Resolution | Proposed custom footprint |
|---|---|---:|---|
| 4" | ST7796S | 480x320 | `MicroConsole:LCD_ST7796S_4in_<SKU>` |
| 2.8" | ILI9341 | 320x240 | `MicroConsole:LCD_ILI9341_2p8_<SKU>` |
| 2.4" | ILI9341 | 320x240 | `MicroConsole:LCD_ILI9341_2p4_<SKU>` |

**Status: PROTOTYPE.** Exact module SKU and mechanics must be frozen from physical samples.

---

## 5. Digital buttons

### MCP23017

**Part:** Microchip `MCP23017-E/SO` — **FROZEN**

- 3.3 V I2C GPIO expander.
- RESET: 10k pull-up to 3.3 V.
- INTA/INTB can remain unused while polling.
- SOIC-28W, 1.27 mm pitch: excellent for hand soldering.

DigiKey: https://www.digikey.com/en/products/detail/microchip-technology/MCP23017-E-SO/894271  
Mouser: https://www.mouser.com/c/?q=MCP23017-E%2FSO

KiCad symbol: `Interface_Expansion:MCP23017_SO` (verify exact installed-library name).  
KiCad footprint: `Package_SO:SOIC-28W_7.5x17.9mm_P1.27mm`

Button map:

| MCP | Function |
|---|---|
| GPA0 | Up |
| GPA1 | Down |
| GPA2 | Left |
| GPA3 | Right |
| GPA4 | A |
| GPA5 | B |
| GPA6 | X |
| GPA7 | unused |
| GPB0 | Y |
| GPB1 | Select |
| GPB2 | Start |
| GPB3 | L1 |
| GPB4 | R1 |
| GPB5 | L2 |
| GPB6 | R2 |
| GPB7 | unused |

Buttons are active-low to GND using MCP pull-ups.

### Main tactile switch

**Candidate:** E-Switch `TL3301NF160QG` — **FROZEN candidate**

DigiKey: https://www.digikey.com/en/products?keywords=TL3301NF160QG  
Mouser: https://www.mouser.com/en/ProductDetail/E-Switch/TL3301NF160QG

KiCad symbol: `Switch:SW_Push`  
KiCad footprint: `Button_Switch_SMD:SW_Push_1P1T_NO_E-Switch_TL3301NxxxxxG`

Shoulder mechanics can change later without changing the MCP mapping.

---

## 6. Sticks and analog subsystem

### Hall/TMR joystick

**Selected joystick family:** PS5-style Hall-effect replacement mechanism.  
**Primary accepted production candidates:**

1. **Ginfull PS5 Hall-effect replacement joystick** — the user's existing, physically measured part.
2. **Favor Union FJH10K-S2** — the specific Favor Union PS5-style Hall part the user found.
3. **Favor Union FJH10K-S3D** — mechanically very similar alternate in the same FJH10K Hall family.

**Status:** FROZEN FAMILY / PROTOTYPE SKU

The user's Ginfull PS5 replacement joystick measures approximately **12.75 mm x 10.00 mm** between the relevant mounting-hole centers.

The official Favor Union drawings for both **FJH10K-S2** and **FJH10K-S3D** show the same principal PCB drilling geometry, including the approximately **12.65 mm x 10.00 mm** mounting pattern. DigiKey also lists both as active FJH10K-series, 2-axis Hall-effect joysticks with center switch, 26-degree travel, through-hole mounting, 1.7-5.5 V supply, and the same nominal 19.50 x 17.60 x 19.30 mm body envelope.

#### FJH10K-S2

DigiKey part: `4434-FJH10K-S2-ND`

DigiKey:  
https://www.digikey.com/en/products/detail/favor-electronics/FJH10K-S2/25879530

Manufacturer drawing:  
https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/6530/FJH10K-S2.pdf

#### FJH10K-S3D

DigiKey part: `4434-FJH10K-S3D-ND`

DigiKey:  
https://www.digikey.com/en/products/detail/favor-electronics/FJH10K-S3D/25879529

Manufacturer drawing:  
https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/6530/FJH10K-S3D.pdf

#### Important S2 vs S3D electrical difference

Although the mechanical drilling drawings are effectively the same, **S2 and S3D are not automatically electrically interchangeable on one fixed net assignment**.

The manufacturer drawings show the Hall-sensor pin order reversed:

```text
FJH10K-S2:
  pin 1 = VDD
  pin 2 = VOUT
  pin 3 = GND

FJH10K-S3D:
  pin 1 = GND
  pin 2 = VOUT
  pin 3 = VDD
```

This applies to the two Hall sensor blocks shown as VR1/VR2 in the drawings. Therefore:

- one **mechanical footprint geometry** can be shared;
- the schematic/net assignment must match the chosen electrical variant;
- do not assume an S3D can replace an S2 on a PCB wired specifically for S2 without swapping VDD/GND;
- the Ginfull sample must be continuity/voltage-mapped to determine whether it follows the S2 or S3D convention.

For the first MicroConsole PCB, use **FJH10K-S2 pinout as the reference** unless measurement of the Ginfull unit indicates otherwise.

Use a family-specific custom footprint:

```text
MicroConsole:Joystick_PS5_Hall_FJH10K
```

Preserve physical manufacturer pad numbers in the footprint. If both Favor Union electrical variants need direct support, use separate schematic symbols:

```text
MicroConsole:FJH10K_S2
MicroConsole:FJH10K_S3D
```

Before fabrication, verify the Ginfull, FJH10K-S2 and FJH10K-S3D for:

- mounting-hole centers;
- electrical-pin positions;
- exact VDD/GND/VOUT mapping;
- shaft center and height;
- body height above and below PCB;
- X/Y center and full-travel output voltages;
- center-switch pinout;
- travel, spring feel and thumb-cap compatibility.

### Conventional potentiometer joystick

Compatibility/fallback mechanism. User-measured mounting-hole spacing is approximately **15.25 mm x 13.00 mm**.

Proposed custom footprint: `MicroConsole:Joystick_Pot_15p25x13`

Do **not** combine these into one footprint. The exact magnetic joystick SKU is still **TBD**. Maintain a common logical symbol with `VCC, GND, X, Y, SW`, then number pads from the selected physical part. Stick clicks remain GP16/GP17.

### ADS7828 ADC replacing PCB0029

**Part:** Texas Instruments `ADS7828E/2K5` — **FROZEN architecture**

- 12-bit SAR ADC.
- 8 inputs.
- I2C.
- up to 50 kSPS.
- TSSOP-16, 0.65 mm.
- internal/external reference.

DigiKey: https://www.digikey.com/en/products/detail/texas-instruments/ADS7828E-2K5/1689481  
Mouser: https://www.mouser.com/c/?q=ADS7828E%2F2K5

KiCad symbol: `Analog_ADC:ADS7828` if available; otherwise make a project-local symbol from the TI datasheet.  
KiCad footprint: `Package_SO:TSSOP-16_4.4x5mm_P0.65mm`

Proposed channels:

| ADC channel | Function |
|---|---|
| CH0 | Left X |
| CH1 | Left Y |
| CH2 | Right X |
| CH3 | Right Y |
| CH4 | Volume |
| CH5 | Battery sense / spare |
| CH6 | Future analog trigger / spare |
| CH7 | Future analog trigger / spare |

**Prototype requirement:** measure the selected Hall/TMR stick supply and X/Y output range before freezing ADS7828 VREF. Do not assume 0-3.3 V until measured.

---

## 7. Volume control

**Part:** Bourns `PTV09A-2015F-B103` — **FROZEN candidate**

10k linear through-hole/snap-in pot, 15 mm shaft on this variant.

DigiKey: https://www.digikey.com/en/products/detail/bourns-inc/PTV09A-2015F-B103/3534257  
Mouser: https://www.mouser.com/c/?q=PTV09A-2015F-B103

KiCad symbol: `Device:R_Potentiometer`  
KiCad footprint: start from `Potentiometer_THT:Potentiometer_Bourns_PTV09A-1_Single_Vertical`, then verify the exact shaft/orientation variant.

Proposed connection:

```text
3V3 --- 10k pot --- GND
          |
          +--- ADS7828 CH4
```

This frees GP28. Add ADC filtering only after knob-noise testing.

---

## 8. Audio subsystem

### Architecture

The 4" console uses **2.1 / spatial stereo**, not discrete surround:

```text
PCM5102A
 | L/R
 +----> PAM8406 ----> Left 1W/8R
 |              \---> Right 1W/8R
 |
 +----> Rsum(L,R) --> low-pass --> PAM8302A --> Center 3W-rated/8R
```

MicroWave can perform stereo widening/crossfeed digitally. The center channel is an analog low-passed L+R sum, so no third DAC channel is needed.

### Stereo DAC

**Part:** Texas Instruments `PCM5102APWR` — **FROZEN**

DigiKey: https://www.digikey.com/en/products/detail/texas-instruments/PCM5102APWR/4341334  
Mouser: https://www.mouser.com/en/ProductDetail/Texas-Instruments/PCM5102APWR

KiCad symbol: `Audio:PCM5102A` if installed, otherwise project-local.  
KiCad footprint: `Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm`

I2S remains GP10 BCLK, GP11 LRCLK, GP20 DATA.

### Stereo L/R amplifier

**Part:** Diodes Incorporated `PAM8406DR` — **FROZEN architecture**

DigiKey: https://www.digikey.com/en/products/detail/diodes-incorporated/PAM8406DR/4033289  
Mouser: https://www.mouser.com/c/?q=PAM8406DR

KiCad symbol: `Amplifier_Audio:PAM8406D`  
KiCad footprint: `Package_SO:SOP-16_3.9x9.9mm_P1.27mm`

The amplifier can exceed the proposed 1 W speaker rating, so final gain / maximum digital volume must be limited during prototype testing.

### Center amplifier

**Part:** Diodes Incorporated `PAM8302AADCR` — **FROZEN architecture**

DigiKey: https://www.digikey.com/en/products/detail/diodes-incorporated/PAM8302AADCR/4033280  
Mouser: https://www.mouser.com/c/?q=PAM8302AADCR

KiCad symbol: `Amplifier_Audio:PAM8302AAD`  
KiCad footprint: `Package_SO:SOIC-8_3.9x4.9mm_P1.27mm`

The L/R summing resistors and low-pass filter values are **PROTOTYPE/TBD**. Never short the PCM5102A L and R outputs directly together.

### L/R speaker candidate

**Part:** PUI Audio `AS02008MR-2-LWC30` — **PROTOTYPE**

20 mm x 5 mm, 8 ohm, 1 W nominal, wired/case-mounted.

DigiKey: https://www.digikey.com/en/products/detail/pui-audio-inc/AS02008MR-2-LWC30/29263745  
Mouser: https://www.mouser.com/c/?q=AS02008MR-2-LWC30

KiCad symbol: `Device:Speaker`; no speaker PCB footprint, only a connector.

### Center speaker candidate

**Part:** PUI Audio `AS03208AS-HT` — **PROTOTYPE**

About 31.7 mm square, 8 ohm, 3 W rated, case-mounted.

DigiKey: https://www.digikey.com/en/products?keywords=AS03208AS-HT  
Mouser: https://www.mouser.com/en/ProductDetail/PUI-Audio/AS03208AS-HT

KiCad symbol: `Device:Speaker`; connector only.

The center can be DNP on 2.8" / 2.4" variants if enclosure volume is insufficient.

---

## 9. Two-pin JST-PH speaker/peripheral connectors

### Vertical header

JST `B2B-PH-K-S`

DigiKey: https://www.digikey.com/en/products/detail/jst-sales-america-inc/B2B-PH-K-S/926611  
Mouser: https://www.mouser.com/c/?q=B2B-PH-K-S

Symbol: `Connector_Generic:Conn_01x02`  
Footprint: `Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical`

### Right-angle header

JST `S2B-PH-K-S`

DigiKey: https://www.digikey.com/en/products/detail/jst-sales-america-inc/S2B-PH-K-S/926626  
Mouser: https://www.mouser.com/c/?q=S2B-PH-K-S

Symbol: `Connector_Generic:Conn_01x02`  
Footprint: `Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal`

Cable housing: JST `PHR-2`  
https://www.digikey.com/en/products/detail/jst-sales-america-inc/PHR-2/608607

Crimp contact: JST `SPH-002T-P0.5S`  
https://www.digikey.com/en/products/detail/jst-sales-america-inc/SPH-002T-P0-5S/527358

Do not use an identical PH2.0 connector for battery and speaker unless the enclosure makes accidental cross-connection impossible.

---

## 10. Passive-component standard

Standardize the board around:

- resistors: **0805, 1%, 1/8 W**
- local bypass capacitors: **0805 X7R**
- bulk capacitors: **1206 X5R/X7R**
- LEDs: **0805**
- diodes: prefer **SOD-123** where electrically appropriate

| Function | Candidate | DigiKey | KiCad |
|---|---|---|---|
| 10k resistor | Yageo `RC0805FR-0710KL` | https://www.digikey.com/en/products/detail/yageo/RC0805FR-0710KL/727535 | `Device:R` + `R_0805_2012Metric` |
| 4.7k resistor | Yageo `RC0805FR-074K7L` | https://www.digikey.com/en/products?keywords=RC0805FR-074K7L | `Device:R` + `R_0805_2012Metric` |
| 100nF bypass | KEMET `C0805C104K5RAC7210` | https://www.digikey.com/en/products/detail/kemet/C0805C104K5RAC7210/3317003 | `Device:C` + `C_0805_2012Metric` |
| 10uF bulk | KYOCERA AVX `1206ZD106KAT2A` | https://www.digikey.com/en/products/detail/kyocera-avx/1206ZD106KAT2A/564607 | `Device:C` + `C_1206_3216Metric` |
| red LED | Lite-On `LTST-C170KRKT` | https://www.digikey.com/en/products/detail/liteon/LTST-C170KRKT/386779 | `Device:LED` + `LED_0805_2012Metric` |

Default rail decoupling should be **100 nF local + appropriate 10 uF bulk**. Do not add 10 nF everywhere by default; reserve it for datasheet-required or deliberately designed filters.

---

## 11. Power / layout rules before routing

- Prefer a solid GND plane rather than splitting analog and digital grounds without a specific reason.
- Keep ADS7828 analog input traces away from Class-D speaker outputs.
- Keep PCM5102A analog L/R traces short and away from high-current speaker traces.
- Route Class-D BTL speaker outputs as short paired runs to the speaker connectors.
- BTL speaker negative terminals are **not GND**.
- Put every 100 nF bypass capacitor physically close to the associated IC supply pin(s).
- Put amplifier bulk capacitance near the amplifier, not merely somewhere on the same rail.

---

## 12. Variant matrix

| Subsystem | 4" | 2.8" | 2.4" |
|---|---|---|---|
| Pico Plus 2 | same | same | same |
| MCP23017 | same | same | same |
| ADS7828 | same | same | same |
| Hall/TMR sticks | preferred | preferred | preferred |
| Display | ST7796S | ILI9341 | ILI9341 |
| Display footprint | unique | unique | unique |
| L/R speakers | yes | yes if space permits | yes if space permits |
| Center speaker | preferred | optional/DNP | likely DNP |
| PCM5102A/PAM8406 | same | same | same |
| PAM8302A center path | fitted | optional | optional/DNP |
| Battery/power path | PARKED | PARKED | PARKED |

---

## 13. Production BOM freeze summary

| Subsystem | MPN | Status | Hand solderability |
|---|---|---|---|
| Main computer | Pimoroni Pico Plus 2 | FROZEN module | module |
| GPIO expander | MCP23017-E/SO | FROZEN | excellent |
| Buttons | TL3301NF160QG | FROZEN candidate | good |
| Stick ADC | ADS7828E/2K5 | FROZEN architecture | good with flux |
| Hall/TMR stick | Ginfull PS5 Hall variant / Favor Union FJH10K-S2; FJH10K-S3D alternate | FROZEN FAMILY / PROTOTYPE SKU | through-hole mechanism |
| Pot-stick fallback | TBD | PROTOTYPE | TBD |
| Volume pot | PTV09A-2015F-B103 | FROZEN candidate | excellent |
| Stereo DAC | PCM5102APWR | FROZEN | good with flux |
| Stereo amp | PAM8406DR | FROZEN architecture | excellent |
| Center amp | PAM8302AADCR | FROZEN architecture | excellent |
| L/R speaker | AS02008MR-2-LWC30 | PROTOTYPE | wires only |
| Center speaker | AS03208AS-HT | PROTOTYPE | wires only |
| Speaker connector | JST PH 2-pin | FROZEN | excellent |
| Battery charger / power path | TBD | PARKED | TBD |

---

## 14. Custom KiCad library work

Create/verify:

```text
MicroConsole:Pico_Plus_2
MicroConsole:LCD_ST7796S_4in_<exact-SKU>
MicroConsole:LCD_ILI9341_2p8_<exact-SKU>
MicroConsole:LCD_ILI9341_2p4_<exact-SKU>
MicroConsole:Joystick_PS5_Hall_FJH10K
MicroConsole:Joystick_Pot_15p25x13
```

Rules for every custom footprint:

- symbol pin numbers must exactly match PCB pad numbers;
- soldered but electrically unused mounting tabs should be unnumbered plated pads;
- plastic locating posts should be NPTH;
- body geometry goes on Fab/Courtyard/User layers, not `Margin`;
- never use netless F.Cu graphics as electrical bridges;
- verify against a physical sample before fabrication.

---

## 15. PARKED battery / charger work

Do not finalize or route this subsystem yet. Preserve these return-to items:

- separate MCP73833 VBAT from VBUS;
- fix/remove JP1 so it cannot short VBUS to GND;
- revisit JP2 / STAT1 / STAT2;
- choose a proper two-pin battery connector;
- decide simultaneous play+charge behavior;
- choose load-sharing / power-path topology;
- verify USB-C CC implementation;
- decide battery-to-system conversion/regulation;
- then add battery-voltage sensing.

---

## 16. Prototype tests required before final routing

1. Compare the user-owned Ginfull PS5 Hall joystick with Favor Union FJH10K-S2/FJH10K-S3D drawings or samples; verify mechanical fit and determine whether Ginfull follows the S2 or S3D VDD/GND convention.
2. Record stick VCC, center voltage, min/max X/Y and direction.
3. Freeze ADS7828 VREF after those measurements.
4. Prototype PTV09 volume through ADS7828.
5. Prototype PCM5102A -> PAM8406 -> selected L/R speakers.
6. Prototype L+R resistor sum / low-pass -> PAM8302A -> center speaker.
7. Establish a safe maximum audio gain for the 1 W side speakers.
8. Test speaker cavities in the actual enclosure.
9. Verify all three LCD module footprints against physical modules.
10. Then freeze placement, return to battery/power-path design, and route.
