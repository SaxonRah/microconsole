# Troubleshooting

These are failures already encountered while bringing up the combined project.
They are documented so the same symptoms are not debugged from scratch later.

## Dependencies: `pathspec ... did not match any file(s) known to git`

Symptom:

```text
error: pathspec 'third_party/microrender' did not match any file(s) known to git
```

Cause: a copied/archive tree retained `.gitmodules` but lost the mode-160000
submodule entries from the Git index.

Fix:

```powershell
.\mc.bat deps
```

`deps` detects and repairs exactly that condition before running `git submodule
update`.

## Raylib link errors for `mr_strbuf_*`

The reusable MicroRender stress HUD calls `mr_strbuf_char`, `mr_strbuf_str`, and
`mr_strbuf_u32`. The combined target must compile/link
`shared/src/mr_strbuf.c` along with `mr_stress_test.c`.

The current CMake files already do this. If those symbols reappear as unresolved
externals, first check that a new target did not omit `mr_strbuf.c`.

## Open Watcom tries to compile `wcl.exe` as a source file

Symptom looked like:

```text
wcc C:\WATCOM\binnt\wcl.exe ...
Error: Compiler returned a bad status compiling "...wcl.exe"
```

The reliable Open Watcom 1.9 invocation used by this project is to put the
Watcom directories on `PATH` and invoke `wcc`/`wcl` by name. Do not replace the
working DOS build with an absolute quoted `wcl.exe` driver path without testing
it on the validated toolchain.

## DOS graphics suddenly run at a fraction of reference speed

First run the parity suite:

```powershell
.\mc.bat run dosref /sprites 1024 /frames 2100
.\mc.bat run dosgfx /sprites 1024 /frames 2100
.\mc.bat run dos /sprites 1024 /frames 2100 /noaudio
.\mc.bat run dos /sprites 1024 /frames 2100
```

Interpretation:

```text
dosref fast, dosgfx slow       -> MicroConsole frontend regression
dosgfx fast, /noaudio slow     -> 16-bit link/layout regression
/noaudio fast, audio slow      -> Sound Blaster/MicroWave service cost
all MicroConsole targets slow  -> build flags or DOSBox environment
```

The known-good DOSBox settings are:

```text
machine=vgaonly
core=dynamic
cycles=max
frameskip=0
aspect=false
```

`machine=vgaonly` matters for the unchained VGA Mode X path. The runner creates a
temporary config with these values automatically.

The DOS renderer must also be compiled to separate `.obj` files and then linked.
A one-shot combined `wcl` build caused a roughly 2x graphics regression during
bring-up even when audio was disabled.

## DOS sound is badly broken at very low graphics FPS

The current Sound Blaster backend is polling-based. A 512-sample half at
11,025 Hz lasts about 46 ms. If graphics becomes slower than that for long
enough, more than one DMA half transition can occur between polls and audio can
miss its refill cadence.

At the validated ~92 FPS combined rate the current transport sounds clean. If a
future workload intentionally runs much slower, either service Sound Blaster
more frequently than once per rendered frame or replace the transport with an
IRQ-driven refill path.

## Pico flashes successfully but neither display nor audio starts

The ILI9341 backend expects a populated `mr_pico_ili9341_t` context before
`mr_pico_ili9341_init()` is called. The combined frontend therefore statically
initializes:

- SPI instance;
- MISO/CS/SCK/MOSI/RST/DC pins;
- requested SPI baud;
- offsets/DMA state.

An earlier combined frontend passed an all-zero context into the LCD driver and
failed before audio initialization, producing exactly this symptom.

If this returns, compare `src/pico/main.c`'s `g_lcd` initializer against the
pinned MicroRender Pico frontend before changing unrelated audio code.

## Pico audio works but the LCD is pale/washed-out/near white

Keep the validated panel timing:

```text
MR_ILI9341_FRMCTR1_DIVA=0x00
MR_ILI9341_FRMCTR1_RTNA=0x1B
```

Do **not** set RTNA to `0x13` or `0x10` merely because the renderer reports
100-110 FPS. Those faster panel scan settings were tested and make the physical
ILI9341 panel noticeably brighter/washed out; `0x10` can appear nearly white.

The renderer can process/present ~110 frames of work per second while the panel
itself continues scanning near 70 Hz.

## Pico uses the right code but LCD SPI seems capped/slow

SPI derives from `clk_peri`. After setting the RP2350 system clock to 300 MHz,
MicroConsole explicitly re-sources `clk_peri` from `clk_sys` so the SPI divider
can reach the validated 75 MHz rate.

If clock setup is changed, verify both `clk_sys` and the actual
`g_lcd.spi_baud_hz` printed by the firmware rather than trusting the requested
CMake value.

## Pico works from a cold boot but behaves strangely after SWD flashing

SWD programming is a warm reset. The current frontend calls
`recover_warm_boot()` before peripheral initialization to:

- reset core 1;
- disable stale DMA IRQ enables;
- abort active DMA channels;
- clear pending DMA interrupt state.

Do not remove that reset path unless the replacement has equivalent ownership
cleanup.

When diagnosing panel state specifically, a physical power cycle remains a
useful control because the ILI9341 is a separate device and may not lose power
when the RP2350 resets.
