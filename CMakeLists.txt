cmake_minimum_required(VERSION 3.16)
project(microconsole_demo_raylib C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(MC_MR "${CMAKE_CURRENT_LIST_DIR}/third_party/microrender")
set(MC_MW "${CMAKE_CURRENT_LIST_DIR}/third_party/microwave")

if(NOT EXISTS "${MC_MR}/shared/src/gfx.c" OR NOT EXISTS "${MC_MW}/shared/src/snd.c")
    message(FATAL_ERROR "Submodules missing. Run: mc.bat deps")
endif()

set(MR_BUILD_ENGINE_HELPERS OFF CACHE BOOL "" FORCE)
set(MW_BUILD_SEQUENCER ON CACHE BOOL "" FORCE)
set(MW_BUILD_PACK OFF CACHE BOOL "" FORCE)
add_subdirectory("${MC_MR}/shared" "${CMAKE_BINARY_DIR}/microrender-shared")
add_subdirectory("${MC_MW}/shared" "${CMAKE_BINARY_DIR}/microwave-shared")

set(MC_RAYLIB "${MC_MR}/third_party/raylib")
if(NOT EXISTS "${MC_RAYLIB}/CMakeLists.txt")
    message(FATAL_ERROR "Raylib missing. Run: mc.bat deps")
endif()
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${MC_RAYLIB}" "${CMAKE_BINARY_DIR}/raylib" EXCLUDE_FROM_ALL)

add_executable(microconsole_demo
    src/raylib/main.c
    "${MC_MR}/shared/src/mr_stress_test.c"
    "${MC_MR}/shared/src/mr_strbuf.c"
    "${MC_MW}/shared/src/mw_music_demo.c")

target_include_directories(microconsole_demo PRIVATE
    "${MC_MR}/shared/src"
    "${MC_MW}/shared/src")

target_compile_definitions(microconsole_demo PRIVATE
    MR_STRESS_MAX_SPRITES=1024
    MR_STRESS_DEFAULT_SPRITES=1024
    MR_STRESS_FAST_METRICS=1
    MR_STRESS_ENABLE_TRIANGLES=1)

target_link_libraries(microconsole_demo PRIVATE
    microrender::gfx
    microwave::snd
    microwave::seq
    raylib)

if(MSVC)
    target_compile_options(microconsole_demo PRIVATE /O2 /W4)
else()
    target_compile_options(microconsole_demo PRIVATE -O2 -Wall -Wextra -Wpedantic)
endif()
