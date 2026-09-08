# Console-era examples.
#
# Included from the root CMakeLists.txt with one line, after the engines and
# Raylib have been added:
#
#     add_subdirectory(examples)
#
# It expects MC_MR and MC_MW to already be set, which they are by the time the
# root file finishes wiring the two submodules.
#
# Two targets are produced:
#
#   microconsole_examples   the Raylib frontend that cycles all nine
#   mc_example_capture      headless PPM/WAV capture and the tile-parity check
#
# The capture tool deliberately does not link Raylib. It exists so the examples
# can be built and verified on a machine with no window system, no GPU and no
# sound card, which is what makes the parity check usable from CI.

if(NOT DEFINED MC_MR OR NOT DEFINED MC_MW)
    message(FATAL_ERROR
        "examples/CMakeLists.txt expects MC_MR and MC_MW from the root project")
endif()

set(MC_EXAMPLE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/mc_example.c"
    "${CMAKE_CURRENT_LIST_DIR}/mc_ex_gfx.c"
    "${CMAKE_CURRENT_LIST_DIR}/mc_ex_chip.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_a2600_beam.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_nes_split.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_sms_lock.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_gb_window.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_pce_wavetable.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_md_linescroll.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_snes_mode7.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_snes_colormath.c"
    "${CMAKE_CURRENT_LIST_DIR}/ex_neogeo_zoom.c")

# mr_strbuf is MicroRender's bounded string builder, used for the on-screen
# readouts. It is compiled in rather than linked because MicroRender only
# exports the renderer itself as a library, which is the correct boundary.
set(MC_EXAMPLE_SUPPORT "${MC_MR}/shared/src/mr_strbuf.c")

add_library(microconsole_examples_lib STATIC
    ${MC_EXAMPLE_SOURCES}
    ${MC_EXAMPLE_SUPPORT})

target_include_directories(microconsole_examples_lib PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${MC_MR}/shared/src"
    "${MC_MW}/shared/src")

target_link_libraries(microconsole_examples_lib PUBLIC
    microrender::gfx
    microwave::snd)

# The examples are held to the same warning set the two engines build with.
# They are meant to be read as reference code, and reference code that only
# compiles cleanly at -Wall is not reference code.
if(MSVC)
    target_compile_options(microconsole_examples_lib PRIVATE /O2 /W4)
else()
    target_compile_options(microconsole_examples_lib PRIVATE
        -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef)
endif()

# --------------------------------------------------------------------------
# Raylib frontend
# --------------------------------------------------------------------------

if(TARGET raylib)
    add_executable(microconsole_examples
        "${CMAKE_CURRENT_LIST_DIR}/../src/raylib/mc_examples_main.c")
    target_link_libraries(microconsole_examples PRIVATE
        microconsole_examples_lib
        raylib)
    if(MSVC)
        target_compile_options(microconsole_examples PRIVATE /O2 /W4)
    else()
        target_compile_options(microconsole_examples PRIVATE
            -O2 -Wall -Wextra -Wpedantic)
    endif()
else()
    message(STATUS
        "examples: Raylib target not found, building the capture tool only")
endif()

# --------------------------------------------------------------------------
# Headless capture and tile-parity harness
# --------------------------------------------------------------------------

add_executable(mc_example_capture
    "${CMAKE_CURRENT_LIST_DIR}/../tools/mc_example_capture.c")
target_link_libraries(mc_example_capture PRIVATE microconsole_examples_lib)
if(MSVC)
    target_compile_options(mc_example_capture PRIVATE /O2 /W4)
else()
    target_compile_options(mc_example_capture PRIVATE -O2 -Wall -Wextra)
endif()

# Run every example through both rendering shapes and fail the build step if
# any frame differs. Cheap enough to run on every build; add it to CTest with
#     enable_testing()
#     add_test(NAME example_tile_parity COMMAND mc_example_capture --quiet)
