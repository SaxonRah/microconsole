# From demo board to handheld console

MicroRender and MicroWave cover the two hardest high-bandwidth platform
services: video and audio. A Game Boy-like handheld still needs the following.

## Must-have hardware

1. **Controls** - D-pad, A/B, Start/Select, and optionally shoulder buttons.
   Use direct GPIO for the first prototype; move to a small GPIO expander only
   if pin pressure becomes real. Buttons should have real external pulls where
   needed, especially on affected early RP2350 silicon.
2. **Nonvolatile game storage** - microSD on the second hardware SPI controller
   is the obvious development choice. Keep it for games, levels, save files,
   screenshots, music/sample packs, and diagnostics. Do not make the renderer
   or mixer depend on filesystem latency in their hot loops.
3. **Battery + power path** - single-cell protected LiPo/Li-ion, charger,
   load-sharing/power-path behavior, hard power switch or soft-latch, and a
   regulator sized for Pico + LCD backlight + audio amplifier peaks.
4. **Battery measurement** - resistor divider into an ADC input or a fuel-gauge
   IC. A handheld needs low-battery warning and orderly save/shutdown policy.
5. **Display/backlight power** - PWM backlight control and, on a final PCB, a
   switch/transistor so sleep can actually turn the panel/backlight off.
6. **Audio controls** - volume buttons or wheel. Digital master gain in
   MicroWave is enough initially; an analog volume path is optional.
7. **Real PCB and connectors** - SD socket, battery connector, speaker header,
   buttons, debug/SWD pads, USB-C access, and strain relief matter more in a
   handheld than another software feature.

## Very useful next

- **Save-state / settings flash area** for tiny data that should survive with no
  SD card inserted. The Pico Plus 2 already has ample QSPI flash, so reserve a
  small region rather than adding EEPROM immediately.
- **RTC / wake source** if you want clock-based games, sleep/wake, or a console
  clock. Not required for a first unit.
- **Haptics**: small vibration motor plus transistor/driver.
- **Headphone output**: PCM5102A plus an appropriate headphone amp/output stage
  if speaker-only is not enough. Do not drive headphones directly from a
  bridge-tied MAX98357A speaker output.
- **USB device services**: game upload, save backup, serial console, screenshots,
  and eventually a tiny mass-storage/update mode.
- **Boot/menu firmware**: game picker, settings, save management, firmware/game
  versioning, crash screen, and update/recovery path.

## Software still missing

- input abstraction shared by DOS/Raylib/Pico
- filesystem + SD block layer and a simple game package format
- save-game API with atomic/backup writes
- asset streaming/cache policy
- launcher/menu and executable/game-module policy
- sleep/power management
- persistent settings
- error/crash logging
- packaging/update/recovery tooling

The important architectural rule is the same one MicroRender/MicroWave already
follow: SD, input, saves, and power management should be platform services.
Games should not know whether their file came from FAT on SD, a host folder, or
an embedded test image.
