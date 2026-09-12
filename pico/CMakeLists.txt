# Pico 2 combined MicroRender + MicroWave demo and console-era examples.
#
# An explicit PICO_SDK_PATH must win over the VS Code extension helper.
# pico-vscode.cmake selects the hard-coded sdkVersion below, which is useful
# for the normal zero-configuration 2.2.0 workflow but must not overwrite a
# caller that deliberately selected a newer SDK (FastDoom PSRAM uses 2.3.0+).
if(DEFINED ENV{PICO_SDK_PATH} AND NOT "$ENV{PICO_SDK_PATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{PICO_SDK_PATH}" MC_EXPLICIT_PICO_SDK_PATH)
    set(PICO_SDK_PATH "${MC_EXPLICIT_PICO_SDK_PATH}" CACHE PATH
        "Path to the Raspberry Pi Pico SDK" FORCE)
    message(STATUS
        "MicroConsole Pico: honoring explicit PICO_SDK_PATH=${PICO_SDK_PATH}")
else()
    if(WIN32)
        set(USERHOME $ENV{USERPROFILE})
    else()
        set(USERHOME $ENV{HOME})
    endif()

    set(sdkVersion 2.3.0)
    set(toolchainVersion 14_2_Rel1)
    set(picotoolVersion 2.3.0)
    set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
    if(EXISTS ${picoVscode})
        include(${picoVscode})
    endif()
endif()

set(PICO_BOARD pimoroni_pico_plus2_rp2350 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.16)

set(MC_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(MC_MR "${MC_ROOT}/third_party/microrender")
set(MC_MW "${MC_ROOT}/third_party/microwave")
include("${MC_MR}/microrender/pico_sdk_import.cmake")
project(microconsole_demo C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(MC_AUDIO_DEVICE "MAX98357A" CACHE STRING "MAX98357A, PCM5102A, or NS4168")
string(TOUPPER "${MC_AUDIO_DEVICE}" MC_AUDIO_DEVICE_UPPER)
if(MC_AUDIO_DEVICE_UPPER STREQUAL "MAX98357A")
    set(MC_AUDIO_DEVICE_ID 1)
elseif(MC_AUDIO_DEVICE_UPPER STREQUAL "PCM5102A")
    set(MC_AUDIO_DEVICE_ID 2)
elseif(MC_AUDIO_DEVICE_UPPER STREQUAL "NS4168")
    set(MC_AUDIO_DEVICE_ID 3)
else()
    message(FATAL_ERROR "MC_AUDIO_DEVICE must be MAX98357A, PCM5102A or NS4168")
endif()

set(MC_AUDIO_RATE 32000 CACHE STRING "I2S sample rate")
set(MC_AUDIO_BLOCK 1024 CACHE STRING "I2S DMA/mix block frames")

# mc_run.bat passes the startup volume through the environment rather than
# through a nested -DNAME=VALUE batch argument. FORCE is intentional: it also
# replaces a stale value in an existing Pico CMake cache.
set(_MC_AUDIO_VOLUME 100)
if(DEFINED ENV{MC_AUDIO_VOLUME_OVERRIDE})
    set(_MC_AUDIO_VOLUME "$ENV{MC_AUDIO_VOLUME_OVERRIDE}")
endif()
set(MC_AUDIO_VOLUME "${_MC_AUDIO_VOLUME}" CACHE STRING
    "Initial master volume, 0..100" FORCE)
if(MC_AUDIO_VOLUME LESS 0 OR MC_AUDIO_VOLUME GREATER 100)
    message(FATAL_ERROR "MC_AUDIO_VOLUME must be 0..100")
endif()

set(MC_STRESS_SPRITES 1024 CACHE STRING "MicroRender stress sprites")
set(MC_SYS_KHZ 300000 CACHE STRING "RP2350 system clock")
set(MC_FASTDOOM_SYS_KHZ 378000 CACHE STRING "FastDoom RP2350 performance clock")
set(MC_FASTDOOM_AUDIO_BLOCK 512 CACHE STRING "FastDoom I2S DMA period frames")
set(MC_FASTDOOM_AUDIO_RING 4 CACHE STRING "FastDoom queued audio slots")
set(MC_FASTDOOM_AUDIO_AHEAD 3 CACHE STRING "FastDoom target ready audio slots")
set(MC_FASTDOOM_PERI_KHZ 300000 CACHE STRING "FastDoom peripheral clock")

set(MC_FASTDOOM_LCD_PANEL "ILI9341" CACHE STRING
    "FastDoom LCD controller: ILI9341 or ST7796S")
set_property(CACHE MC_FASTDOOM_LCD_PANEL PROPERTY STRINGS ILI9341 ST7796S)
string(TOUPPER "${MC_FASTDOOM_LCD_PANEL}" MC_FASTDOOM_LCD_PANEL_UPPER)
if(MC_FASTDOOM_LCD_PANEL_UPPER STREQUAL "ILI9341")
    set(MC_FASTDOOM_LCD_PANEL_ST7796S 0)
elseif(MC_FASTDOOM_LCD_PANEL_UPPER STREQUAL "ST7796S")
    set(MC_FASTDOOM_LCD_PANEL_ST7796S 1)
else()
    message(FATAL_ERROR
        "MC_FASTDOOM_LCD_PANEL must be ILI9341 or ST7796S, got '${MC_FASTDOOM_LCD_PANEL}'")
endif()

set(MC_LCD_SPI_BAUD 75100000 CACHE STRING "LCD SPI clock ceiling" FORCE)
set(MC_LACE_BLOCK_H 8 CACHE STRING "Alternating lace block height")
set(MC_I2S_BCLK 10 CACHE STRING "I2S BCLK GPIO")
set(MC_I2S_LRCLK 11 CACHE STRING "I2S LRCLK GPIO")
set(MC_I2S_DATA 20 CACHE STRING "I2S DATA GPIO" FORCE)
message(STATUS "MicroConsole Pico: forcing I2S DATA to GP${MC_I2S_DATA} (GP12 reserved for SD MISO)")

set(MC_SD_MISO 12 CACHE STRING "microSD SPI1 MISO GPIO")
set(MC_SD_CS 13 CACHE STRING "microSD chip-select GPIO")
set(MC_SD_SCK 14 CACHE STRING "microSD SPI1 SCK GPIO")
set(MC_SD_MOSI 15 CACHE STRING "microSD SPI1 MOSI GPIO")
set(MC_SD_BAUD 12500000 CACHE STRING "microSD SPI clock")

set(MR_BUILD_ENGINE_HELPERS OFF CACHE BOOL "" FORCE)

# Existing examples still use the sequencer. FastDoom additionally needs the
# validated MUS -> MIDI -> strict GENMIDI OPL -> Nuked OPL path.
set(MW_SND_U8 OFF CACHE BOOL "" FORCE)
set(MW_BUILD_SEQUENCER ON CACHE BOOL "" FORCE)
set(MW_BUILD_MIDI ON CACHE BOOL "" FORCE)
set(MW_BUILD_MUS ON CACHE BOOL "" FORCE)
set(MW_BUILD_MIDI_FM OFF CACHE BOOL "" FORCE)
set(MW_BUILD_GENMIDI OFF CACHE BOOL "" FORCE)
set(MW_BUILD_GENMIDI_OPL ON CACHE BOOL "" FORCE)
set(MW_BUILD_PACK OFF CACHE BOOL "" FORCE)

add_subdirectory("${MC_MR}/shared" "${CMAKE_BINARY_DIR}/microrender-shared")
add_subdirectory("${MC_MW}/shared" "${CMAKE_BINARY_DIR}/microwave-shared")

# FastDoom's Pico path is compute-bound in MUS/GENMIDI/Nuked.  The MicroWave
# project intentionally does not force a global optimization level, so make
# the RP2350 code-generation policy explicit here without changing MicroWave's
# desktop/DOS builds.
foreach(_mc_mw_perf_target
        microwave_snd
        microwave_midi
        microwave_mus
        microwave_nuked_opl3
        microwave_genmidi_opl)
    if(TARGET ${_mc_mw_perf_target})
        target_compile_options(${_mc_mw_perf_target} PRIVATE -O3)
    endif()
endforeach()

# FastDoom Pico production baseline: the per-sample Nuked OPL path lives
# in SRAM. This removes XIP/QMI stalls that caused audio underruns while
# the ST7796S presenter and PSRAM-backed Doom workload were active.
target_compile_definitions(microwave_nuked_opl3 PRIVATE
    MC_OPL3_TIME_CRITICAL=1)
target_compile_definitions(microwave_genmidi_opl PRIVATE
    MC_OPL3_TIME_CRITICAL=1)
message(STATUS "MicroConsole FastDoom OPL3 hot path: SRAM")


set(MC_PICO_FATFS_SD "${MC_ROOT}/third_party/pico-fatfs-sd")
if(EXISTS "${MC_PICO_FATFS_SD}/src/CMakeLists.txt")
    add_subdirectory(
        "${MC_PICO_FATFS_SD}/src"
        "${CMAKE_BINARY_DIR}/pico-fatfs-sd")
else()
    message(STATUS
        "MicroConsole Pico FatFS SD: dependency missing "
        "(FastDoom Pico build command will fetch the pinned checkout)")
endif()

# ---------------------------------------------------------------------------
# FastDoom Pico audio transport compile target.
#
# This is intentionally a static library for now. It lets the real Pico SDK
# compile the DMA/PIO transport before the remaining FastDoom Pico platform
# pieces (ILI9341 presentation, SD/FatFS WAD access and controls) are linked
# into the final executable. Undefined mc_fd_audio_render()/core symbols are
# resolved later by that executable; static archive creation does not require
# them yet.
# ---------------------------------------------------------------------------
add_library(microconsole_fastdoom_audio_pico STATIC
    "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_audio_pico.c")

pico_generate_pio_header(microconsole_fastdoom_audio_pico
    "${MC_MW}/microwave/mw_i2s.pio")

target_include_directories(microconsole_fastdoom_audio_pico PRIVATE
    "${MC_ROOT}/games/fastdoom/src"
    "${MC_MW}/shared/src")

target_compile_definitions(microconsole_fastdoom_audio_pico PRIVATE
    MC_AUDIO_DEVICE=${MC_AUDIO_DEVICE_ID}
    MC_AUDIO_VOLUME=${MC_AUDIO_VOLUME}
    MC_I2S_BCLK=${MC_I2S_BCLK}
    MC_I2S_LRCLK=${MC_I2S_LRCLK}
    MC_I2S_DATA=${MC_I2S_DATA}
    MC_FD_AUDIO_RATE=${MC_AUDIO_RATE}
    MC_FD_PICO_DMA_BLOCK=${MC_FASTDOOM_AUDIO_BLOCK}
    MC_FD_PICO_RING_COUNT=${MC_FASTDOOM_AUDIO_RING}
    MC_FD_PICO_RING_TARGET=${MC_FASTDOOM_AUDIO_AHEAD})

target_compile_options(microconsole_fastdoom_audio_pico PRIVATE -O3)

target_link_libraries(microconsole_fastdoom_audio_pico PRIVATE
    microwave::snd
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_pio)

add_library(microconsole::fastdoom_audio_pico ALIAS
    microconsole_fastdoom_audio_pico)


# ---------------------------------------------------------------------------
# FastDoom PSRAM probe (Pico SDK 2.3.0+).
#
# SDK 2.2.0 does not provide hardware_psram, so this target is deliberately
# conditional. Existing 2.2.0 demo/example/audio builds remain unchanged.
# With SDK 2.3.0+ on Pimoroni Pico Plus 2, hardware_psram initializes the
# board's on-board PSRAM before main() and the probe verifies a Doom-sized zone.
# ---------------------------------------------------------------------------
if(TARGET hardware_psram)
    add_executable(microconsole_fastdoom_psram_probe
        "${MC_ROOT}/src/pico/mc_fastdoom_psram_probe.c")

    target_compile_definitions(microconsole_fastdoom_psram_probe PRIVATE
        MC_FD_PSRAM_ZONE_BYTES=6291456u)

    target_link_libraries(microconsole_fastdoom_psram_probe PRIVATE
        pico_stdlib
        hardware_psram)

    pico_enable_stdio_usb(microconsole_fastdoom_psram_probe 1)
    pico_enable_stdio_uart(microconsole_fastdoom_psram_probe 0)
    pico_add_extra_outputs(microconsole_fastdoom_psram_probe)

    target_link_options(microconsole_fastdoom_psram_probe PRIVATE
        "-Wl,--print-memory-usage")

    message(STATUS
        "MicroConsole FastDoom PSRAM: target=microconsole_fastdoom_psram_probe "
        "(6 MiB zone verification)")
else()
    message(STATUS
        "MicroConsole FastDoom PSRAM: skipped (hardware_psram requires Pico SDK 2.3.0+)")
endif()

function(mc_configure_pico_target target)
    pico_generate_pio_header(${target}
        "${MC_MW}/microwave/mw_i2s.pio")

    target_include_directories(${target} PRIVATE
        "${MC_ROOT}/examples"
        "${MC_MR}/shared/src"
        "${MC_MR}/shared/rp2350"
        "${MC_MW}/shared/src")

    target_compile_definitions(${target} PRIVATE
        MC_AUDIO_DEVICE=${MC_AUDIO_DEVICE_ID}
        MC_AUDIO_RATE=${MC_AUDIO_RATE}
        MC_AUDIO_BLOCK=${MC_AUDIO_BLOCK}
        MC_AUDIO_VOLUME=${MC_AUDIO_VOLUME}
        MC_STRESS_SPRITES=${MC_STRESS_SPRITES}
        MC_SYS_KHZ=${MC_SYS_KHZ}
        MC_LACE_BLOCK_H=${MC_LACE_BLOCK_H}
        MC_I2S_BCLK=${MC_I2S_BCLK}
        MC_I2S_LRCLK=${MC_I2S_LRCLK}
        MC_I2S_DATA=${MC_I2S_DATA}
        MR_STRESS_MAX_SPRITES=1024
        MR_STRESS_DEFAULT_SPRITES=1024
        MR_STRESS_FAST_METRICS=1
        MR_STRESS_ENABLE_TRIANGLES=1
        MR_LCD_PIN_MISO=4
        MR_LCD_PIN_CS=5
        MR_LCD_PIN_SCK=6
        MR_LCD_PIN_MOSI=7
        MR_LCD_PIN_RST=8
        MR_LCD_PIN_DC=9
        MR_LCD_SPI_BAUD=${MC_LCD_SPI_BAUD}
        MR_ILI9341_MADCTL=0xE8
        MR_ILI9341_FRMCTR1_DIVA=0x00
        MR_ILI9341_FRMCTR1_RTNA=0x1B)

    target_link_libraries(${target} PRIVATE
        microrender::gfx microwave::snd microwave::seq
        pico_stdlib
        hardware_clocks hardware_dma hardware_irq hardware_pio hardware_spi)

    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 0)
    pico_add_extra_outputs(${target})
endfunction()

# Existing stress / combined demo.
add_executable(microconsole_demo
    "${MC_ROOT}/src/pico/main.c"
    "${MC_MR}/shared/src/mr_stress_test.c"
    "${MC_MR}/shared/src/mr_strbuf.c"
    "${MC_MR}/shared/rp2350/mr_pico_ili9341.c"
    "${MC_MW}/shared/src/mw_music_demo.c")
mc_configure_pico_target(microconsole_demo)
target_link_libraries(microconsole_demo PRIVATE pico_multicore)

# Console-era example registry shared with Raylib and DOS.
set(MC_EXAMPLE_SOURCES
    "${MC_ROOT}/examples/mc_example.c"
    "${MC_ROOT}/examples/mc_ex_gfx.c"
    "${MC_ROOT}/examples/mc_ex_chip.c"
    "${MC_ROOT}/examples/ex_a2600_beam.c"
    "${MC_ROOT}/examples/ex_nes_split.c"
    "${MC_ROOT}/examples/ex_sms_lock.c"
    "${MC_ROOT}/examples/ex_gb_window.c"
    "${MC_ROOT}/examples/ex_pce_wavetable.c"
    "${MC_ROOT}/examples/ex_md_linescroll.c"
    "${MC_ROOT}/examples/ex_snes_mode7.c"
    "${MC_ROOT}/examples/ex_snes_colormath.c"
    "${MC_ROOT}/examples/ex_neogeo_zoom.c")

add_executable(microconsole_examples
    "${MC_ROOT}/src/pico/mc_examples_main.c"
    ${MC_EXAMPLE_SOURCES}
    "${MC_MR}/shared/src/mr_strbuf.c"
    "${MC_MR}/shared/rp2350/mr_pico_ili9341.c")
mc_configure_pico_target(microconsole_examples)
target_link_libraries(microconsole_examples PRIVATE pico_multicore)

# Make the real RP2350 FLASH/SRAM footprint visible in every build log.
target_link_options(microconsole_examples PRIVATE
    "-Wl,--print-memory-usage")


# ---------------------------------------------------------------------------
# First real FastDoom Pico 2 target.
#
# The generated source overlay is prepared by:
#   .\mc.bat build fastdoom-pico max98357a
#
# Normal Pico demo/example builds do not require FastDoom. If the overlay does
# not exist, this target is simply omitted.
# ---------------------------------------------------------------------------
set(MC_FD "${MC_ROOT}/build-fastdoom-src/FASTDOOM")

if(EXISTS "${MC_FD}/d_main.c")
    set(MC_FD_SOURCES
        "${MC_FD}/d_main.c"
        "${MC_FD}/am_map.c"
        "${MC_FD}/d_net.c"
        "${MC_FD}/dutils.c"
        "${MC_FD}/f_finale.c"
        "${MC_FD}/f_wipe.c"
        "${MC_FD}/g_game.c"
        "${MC_FD}/hu_lib.c"
        "${MC_FD}/hu_stuff.c"
        "${MC_FD}/i_file.c"
        "${MC_FD}/i_gamma.c"
        "${MC_FD}/i_random.c"
        "${MC_FD}/info.c"
        "${MC_FD}/m_bench.c"
        "${MC_FD}/m_menu.c"
        "${MC_FD}/m_misc.c"
        "${MC_FD}/math.c"
        "${MC_FD}/p_ceilng.c"
        "${MC_FD}/p_doors.c"
        "${MC_FD}/p_enemy.c"
        "${MC_FD}/p_floor.c"
        "${MC_FD}/p_inter.c"
        "${MC_FD}/p_lights.c"
        "${MC_FD}/p_map.c"
        "${MC_FD}/p_maputl.c"
        "${MC_FD}/p_mobj.c"
        "${MC_FD}/p_plats.c"
        "${MC_FD}/p_pspr.c"
        "${MC_FD}/p_saveg.c"
        "${MC_FD}/p_setup.c"
        "${MC_FD}/p_sight.c"
        "${MC_FD}/p_spec.c"
        "${MC_FD}/p_switch.c"
        "${MC_FD}/p_telept.c"
        "${MC_FD}/p_tick.c"
        "${MC_FD}/p_user.c"
        "${MC_FD}/r_bsp.c"
        "${MC_FD}/r_data.c"
        "${MC_FD}/r_draw.c"
        "${MC_FD}/r_main.c"
        "${MC_FD}/r_plane.c"
        "${MC_FD}/r_segs.c"
        "${MC_FD}/r_things.c"
        "${MC_FD}/sounds.c"
        "${MC_FD}/st_lib.c"
        "${MC_FD}/st_stuff.c"
        "${MC_FD}/tables.c"
        "${MC_FD}/v_video.c"
        "${MC_FD}/w_wad.c"
        "${MC_FD}/wi_stuff.c"
        "${MC_FD}/z_zone.c")

    set(MC_FD_PORT_SOURCES
        ${MC_FD_SOURCES}
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_compat_data.c"
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_fastmath.c"
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_render.c"
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_sound_mw.c"
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_pico.c"
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_pico_fs.c"
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_pico_wad.c"
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_pico_main.c")

    set(MC_FD_ST7796S_SCANOUT
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_scanout_st7796s.c")

    add_executable(microconsole_fastdoom_pico
        ${MC_FD_PORT_SOURCES}
        "${MC_ROOT}/games/fastdoom/src/mc_fastdoom_pico_sd.c"
        "${MC_FD_ST7796S_SCANOUT}"
        "${MC_MR}/shared/rp2350/mr_pico_ili9341.c")

    set_property(SOURCE "${MC_FD_ST7796S_SCANOUT}" APPEND PROPERTY
        INCLUDE_DIRECTORIES
            "${MC_ROOT}/games/fastdoom/src"
            "${MC_MR}/shared/src"
            "${MC_MR}/shared/rp2350")

    set_property(SOURCE "${MC_FD_ST7796S_SCANOUT}" APPEND PROPERTY
        COMPILE_OPTIONS
            -O3
            -Wall
            -Wextra)

    # IMPORTANT:
    # Pico SDK interface libraries inject SDK C and ASM sources directly into
    # this executable. FastDoom compatibility headers must therefore be scoped
    # to the FastDoom/MicroConsole C files instead of target-wide; otherwise
    # compat/io.h, compat/unistd.h, FastDoom's math.h, etc. shadow newlib while
    # compiling the SDK, and -include would even feed C declarations to .S.
    set_property(SOURCE ${MC_FD_PORT_SOURCES} APPEND PROPERTY
        INCLUDE_DIRECTORIES
            "${MC_ROOT}/games/fastdoom/compat"
            "${MC_ROOT}/games/fastdoom/src"
            "${MC_FD}"
            "${MC_MR}/shared/src"
            "${MC_MR}/shared/rp2350"
            "${MC_MW}/shared/src")

    set_property(SOURCE ${MC_FD_PORT_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS
            -include
            "${MC_ROOT}/games/fastdoom/compat/mc_fastdoom_force_compat.h"
            -O3
            -fno-strict-aliasing
            -fsigned-char
            -Wall
            -Wextra
            -Wno-unknown-pragmas
            -Wno-unused-parameter)

    target_compile_definitions(microconsole_fastdoom_pico PRIVATE
        MC_FASTDOOM_PICO=1
        MC_FD_AUDIO_RATE=${MC_AUDIO_RATE}
        MC_FD_AUDIO_BLOCK=256
        MC_FD_PSRAM_ZONE_BYTES=6291456u
        MC_I2S_DATA=${MC_I2S_DATA}
        MC_SYS_KHZ=${MC_FASTDOOM_SYS_KHZ}
        MC_FD_PERI_KHZ=${MC_FASTDOOM_PERI_KHZ}
        MC_FD_OPL_POST_GAIN_SHIFT=3
        MC_FD_RENDER_VALIDATE=0
        MC_FD_LCD_PANEL_ST7796S=${MC_FASTDOOM_LCD_PANEL_ST7796S}
        MC_SD_MISO=${MC_SD_MISO}
        MC_SD_CS=${MC_SD_CS}
        MC_SD_SCK=${MC_SD_SCK}
        MC_SD_MOSI=${MC_SD_MOSI}
        MC_SD_BAUD=${MC_SD_BAUD}
        USE_BACKBUFFER=1
        SCREENWIDTH=320
        SCREENHEIGHT=200
        DEBUG_ENABLED=0
        ASSERT_ENABLED=0
        BOUNDS_CHECK_ENABLED=0
        MR_LCD_PIN_MISO=4
        MR_LCD_PIN_CS=5
        MR_LCD_PIN_SCK=6
        MR_LCD_PIN_MOSI=7
        MR_LCD_PIN_RST=8
        MR_LCD_PIN_DC=9
        MR_LCD_SPI_BAUD=${MC_LCD_SPI_BAUD}
        MR_LCD_PANEL_ST7796S=${MC_FASTDOOM_LCD_PANEL_ST7796S}
        MR_ILI9341_MADCTL=0xE8
        MR_ILI9341_FRMCTR1_DIVA=0x00
        MR_ILI9341_FRMCTR1_RTNA=0x1B)

    target_link_libraries(microconsole_fastdoom_pico PRIVATE
        microconsole::fastdoom_audio_pico
        microwave::mus
        microwave::genmidi_opl
        microrender::gfx
        pico-fatfs-sd
        pico_stdlib
        pico_multicore
        hardware_clocks
        hardware_dma
        hardware_irq
        hardware_pio
        hardware_psram
        hardware_spi
        hardware_vreg
        hardware_watchdog)

    pico_enable_stdio_usb(microconsole_fastdoom_pico 1)
    pico_enable_stdio_uart(microconsole_fastdoom_pico 0)
    pico_add_extra_outputs(microconsole_fastdoom_pico)

    target_link_options(microconsole_fastdoom_pico PRIVATE
        "-Wl,--print-memory-usage")

    message(STATUS
        "MicroConsole FastDoom Pico: target=microconsole_fastdoom_pico "
        "(real Cortex-M33 game + SPI1/FatFS SD backend, "
        "panel=${MC_FASTDOOM_LCD_PANEL_UPPER})")
else()
    message(STATUS
        "MicroConsole FastDoom Pico: skipped (run .\\mc.bat build fastdoom-pico max98357a)")
endif()

message(STATUS "MicroConsole Pico: audio=${MC_AUDIO_DEVICE_UPPER} ${MC_AUDIO_RATE}Hz block=${MC_AUDIO_BLOCK} volume=${MC_AUDIO_VOLUME}%; stress=${MC_STRESS_SPRITES}; sys=${MC_SYS_KHZ}kHz; SPI=${MC_LCD_SPI_BAUD}; lace=${MC_LACE_BLOCK_H}")
message(STATUS
    "MicroConsole FastDoom perf: sys=${MC_FASTDOOM_SYS_KHZ}kHz "
    "audio=${MC_FASTDOOM_AUDIO_BLOCK}f ring=${MC_FASTDOOM_AUDIO_RING} "
    "ahead=${MC_FASTDOOM_AUDIO_AHEAD} OPL-postgain=x8 checks=off O3 "
    "panel=${MC_FASTDOOM_LCD_PANEL_UPPER}")
message(STATUS "MicroConsole Pico examples: target=microconsole_examples (tiled SRAM frontend, no PSRAM)")
message(STATUS "MicroConsole FastDoom Pico audio: target=microconsole_fastdoom_audio_pico (PIO/DMA transport compile check)")
