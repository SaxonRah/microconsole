cmake_minimum_required(VERSION 3.20)
project(microconsole_fastdoom_raylib C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(MC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(FASTDOOM_ROOT "${MC_ROOT}/third_party/fastdoom" CACHE PATH
    "Pinned viti95/FastDoom checkout")
set(FASTDOOM_PATCHED_DIR "${MC_ROOT}/build-fastdoom-src" CACHE PATH
    "Generated FastDoom source overlay")

set(FD "${FASTDOOM_PATCHED_DIR}/FASTDOOM")
set(FD_MAIN "${FD}/d_main.c")
set(MC_MR "${MC_ROOT}/third_party/microrender")
set(MC_RAYLIB "${MC_MR}/third_party/raylib")

if(NOT EXISTS "${FD}/doomdef.h")
    message(FATAL_ERROR
        "FastDoom is missing. Run: .\\mc.bat deps fastdoom")
endif()

if(NOT EXISTS "${FD_MAIN}")
    message(FATAL_ERROR
        "Generated FastDoom portable source tree is missing. "
        "Run scripts/mc_fastdoom_prepare.py or use .\\mc.bat build fastdoom-raylib")
endif()

if(NOT EXISTS "${MC_RAYLIB}/CMakeLists.txt")
    message(FATAL_ERROR
        "MicroRender's Raylib dependency is missing. Run: .\\mc.bat deps")
endif()

if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "FastDoom Raylib bring-up must be configured as Win32/x86. "
        "Use the canonical .\\mc.bat build fastdoom-raylib command.")
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${MC_RAYLIB}" "${CMAKE_BINARY_DIR}/raylib" EXCLUDE_FROM_ALL)

set(FD_SOURCES
    "${FD_MAIN}"
    "${FD}/am_map.c"
    "${FD}/d_net.c"
    "${FD}/dutils.c"
    "${FD}/f_finale.c"
    "${FD}/f_wipe.c"
    "${FD}/g_game.c"
    "${FD}/hu_lib.c"
    "${FD}/hu_stuff.c"
    "${FD}/i_file.c"
    "${FD}/i_gamma.c"
    "${FD}/i_random.c"
    "${FD}/i_main.c"
    "${FD}/info.c"
    "${FD}/m_bench.c"
    "${FD}/m_menu.c"
    "${FD}/m_misc.c"
    "${FD}/math.c"
    "${FD}/p_ceilng.c"
    "${FD}/p_doors.c"
    "${FD}/p_enemy.c"
    "${FD}/p_floor.c"
    "${FD}/p_inter.c"
    "${FD}/p_lights.c"
    "${FD}/p_map.c"
    "${FD}/p_maputl.c"
    "${FD}/p_mobj.c"
    "${FD}/p_plats.c"
    "${FD}/p_pspr.c"
    "${FD}/p_saveg.c"
    "${FD}/p_setup.c"
    "${FD}/p_sight.c"
    "${FD}/p_spec.c"
    "${FD}/p_switch.c"
    "${FD}/p_telept.c"
    "${FD}/p_tick.c"
    "${FD}/p_user.c"
    "${FD}/r_bsp.c"
    "${FD}/r_data.c"
    "${FD}/r_draw.c"
    "${FD}/r_main.c"
    "${FD}/r_plane.c"
    "${FD}/r_segs.c"
    "${FD}/r_things.c"
    "${FD}/st_lib.c"
    "${FD}/st_stuff.c"
    "${FD}/tables.c"
    "${FD}/v_video.c"
    "${FD}/w_wad.c"
    "${FD}/wi_stuff.c"
    "${FD}/z_zone.c"
)

add_executable(microconsole_fastdoom
    ${FD_SOURCES}
    "${CMAKE_CURRENT_LIST_DIR}/src/mc_fastdoom_compat_data.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mc_fastdoom_dos_win32.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mc_fastdoom_fastmath.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mc_fastdoom_render.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mc_fastdoom_raylib.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mc_fastdoom_sound_stub.c"
)

target_include_directories(microconsole_fastdoom PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/compat"
    "${FD}"
)

target_compile_definitions(microconsole_fastdoom PRIVATE
    MC_FASTDOOM_RAYLIB=1
    USE_BACKBUFFER=1
    SCREENWIDTH=320
    SCREENHEIGHT=200
    DEBUG_ENABLED=0
    ASSERT_ENABLED=0
    BOUNDS_CHECK_ENABLED=0
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
)

target_link_libraries(microconsole_fastdoom PRIVATE raylib)

if(WIN32)
    target_link_libraries(microconsole_fastdoom PRIVATE dbghelp)
endif()

if(MSVC)
    target_compile_options(microconsole_fastdoom PRIVATE
        /FI"${CMAKE_CURRENT_LIST_DIR}/compat/mc_fastdoom_force_compat.h"
        /O2
        /Zi
        /W3
        /wd4068
        /wd4113
        /wd4244
        /wd4267
        /wd4311
        /wd4312
        /wd4996
    )
    target_link_options(microconsole_fastdoom PRIVATE /DEBUG)
else()
    target_compile_options(microconsole_fastdoom PRIVATE
        -include "${CMAKE_CURRENT_LIST_DIR}/compat/mc_fastdoom_force_compat.h"
        -O2
        -Wall
        -Wextra
        -Wno-unknown-pragmas
    )
endif()

# FastDoom's startup text is a runtime file, not a WAD lump.
add_custom_command(TARGET microconsole_fastdoom POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:microconsole_fastdoom>/TEXT"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${FASTDOOM_ROOT}/TEXT/PROG.TXT"
            "$<TARGET_FILE_DIR:microconsole_fastdoom>/TEXT/PROG.TXT"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${FASTDOOM_ROOT}/LEVELS"
            "$<TARGET_FILE_DIR:microconsole_fastdoom>/LEVELS"
    VERBATIM
)
