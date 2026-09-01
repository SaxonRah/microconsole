# Pico 2 combined MicroRender + MicroWave demo.
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

add_executable(microconsole_demo
    "${MC_ROOT}/src/pico/main.c"
    "${MC_MR}/shared/src/mr_stress_test.c"
    "${MC_MR}/shared/src/mr_strbuf.c"
    "${MC_MR}/shared/rp2350/mr_pico_ili9341.c"
    "${MC_MW}/shared/src/mw_music_demo.c")

pico_generate_pio_header(microconsole_demo
    "${MC_MW}/microwave/mw_i2s.pio")

target_include_directories(microconsole_demo PRIVATE
    "${MC_MR}/shared/src"
    "${MC_MR}/shared/rp2350"
    "${MC_MW}/shared/src")

target_compile_definitions(microconsole_demo PRIVATE
    MC_AUDIO_DEVICE=${MC_AUDIO_DEVICE_ID}
    MC_AUDIO_RATE=${MC_AUDIO_RATE}
    MC_AUDIO_BLOCK=${MC_AUDIO_BLOCK}
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

target_link_libraries(microconsole_demo PRIVATE
    microrender::gfx microwave::snd microwave::seq
    pico_stdlib pico_multicore
    hardware_clocks hardware_dma hardware_irq hardware_pio hardware_spi)

pico_enable_stdio_usb(microconsole_demo 1)
pico_enable_stdio_uart(microconsole_demo 0)
pico_add_extra_outputs(microconsole_demo)

message(STATUS "MicroConsole Pico: audio=${MC_AUDIO_DEVICE_UPPER} ${MC_AUDIO_RATE}Hz block=${MC_AUDIO_BLOCK}; stress=${MC_STRESS_SPRITES}; sys=${MC_SYS_KHZ}kHz; SPI=${MC_LCD_SPI_BAUD}; lace=${MC_LACE_BLOCK_H}")
