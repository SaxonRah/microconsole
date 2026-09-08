# Pico 2 combined MicroRender + MicroWave demo and console-era examples.
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if(EXISTS ${picoVscode})
    include(${picoVscode})
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
set(MC_LCD_SPI_BAUD 75000000 CACHE STRING "ILI9341 SPI clock")
set(MC_LACE_BLOCK_H 8 CACHE STRING "Alternating lace block height")
set(MC_I2S_BCLK 10 CACHE STRING "I2S BCLK GPIO")
set(MC_I2S_LRCLK 11 CACHE STRING "I2S LRCLK GPIO")
set(MC_I2S_DATA 12 CACHE STRING "I2S DATA GPIO")

set(MR_BUILD_ENGINE_HELPERS OFF CACHE BOOL "" FORCE)
set(MW_BUILD_SEQUENCER ON CACHE BOOL "" FORCE)
set(MW_BUILD_PACK OFF CACHE BOOL "" FORCE)
add_subdirectory("${MC_MR}/shared" "${CMAKE_BINARY_DIR}/microrender-shared")
add_subdirectory("${MC_MW}/shared" "${CMAKE_BINARY_DIR}/microwave-shared")

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

message(STATUS "MicroConsole Pico: audio=${MC_AUDIO_DEVICE_UPPER} ${MC_AUDIO_RATE}Hz block=${MC_AUDIO_BLOCK} volume=${MC_AUDIO_VOLUME}%; stress=${MC_STRESS_SPRITES}; sys=${MC_SYS_KHZ}kHz; SPI=${MC_LCD_SPI_BAUD}; lace=${MC_LACE_BLOCK_H}")
message(STATUS "MicroConsole Pico examples: target=microconsole_examples (tiled SRAM frontend, no PSRAM)")
