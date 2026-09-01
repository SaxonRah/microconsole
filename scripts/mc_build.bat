@echo off
setlocal EnableExtensions EnableDelayedExpansion
if "%MC_ROOT%"=="" set "MC_ROOT=%~dp0.."
cd /d "%MC_ROOT%"

set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=all"
shift /1

if not exist "third_party\microrender\shared\src\gfx.c" goto no_deps
if not exist "third_party\microwave\shared\src\snd.c" goto no_deps

if /i "%WHAT%"=="raylib" goto raylib
if /i "%WHAT%"=="dos"    goto dos
if /i "%WHAT%"=="pico"   goto pico
if /i "%WHAT%"=="all"    goto all

echo ERROR: unknown build target "%WHAT%".
exit /b 1

:raylib
where cmake >nul 2>nul || (echo ERROR: cmake not found on PATH.& exit /b 1)
if not exist "third_party\microrender\third_party\raylib\CMakeLists.txt" (
    echo ERROR: MicroRender's Raylib submodule is not initialized.
    echo Run: .\mc.bat deps
    exit /b 1
)
echo === Raylib combined demo ===
set "MC_CMAKE_ARGS="
:raylib_args
if "%~1"=="" goto raylib_args_done
set "MC_CMAKE_ARGS=!MC_CMAKE_ARGS! "%~1""
shift /1
goto raylib_args
:raylib_args_done
cmake -S . -B build-raylib -DCMAKE_BUILD_TYPE=Release !MC_CMAKE_ARGS! || exit /b 1
cmake --build build-raylib --config Release || exit /b 1
echo built build-raylib\Release\microconsole_demo.exe ^(MSVC^) or build-raylib\microconsole_demo.exe
exit /b 0

:dos
if "%WATCOM%"=="" (
    echo ERROR: WATCOM is not set.
    exit /b 1
)
if not exist build-dos mkdir build-dos
rem Follow MicroWave's known-good Open Watcom 1.9 setup: put the toolchain on
rem PATH and invoke the driver by name. Invoking an absolute quoted wcl.exe
rem path makes some OW 1.9 installs pass argv[0] through as a source filename.
set "PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%WATCOM%\binw;%PATH%"
set "INCLUDE=%WATCOM%\h;%WATCOM%\h\nt"
where wcl >nul 2>nul || (echo ERROR: wcl.exe not found under %WATCOM%.& exit /b 1)
echo === 16-bit DOS combined demo ===
wcl -q -bt=dos -ml -2 -ox -s -w4 -fe=build-dos\MCDEMO.EXE ^
  -dGFX_FIXED_NO_INT64 -dGFX_COLOR_INDEX8=0 -dGFX_ENABLE_TRIANGLES=1 ^
  -dMR_STRESS_MAX_SPRITES=1024 -dMR_STRESS_DEFAULT_SPRITES=1024 ^
  -dMR_STRESS_FAST_METRICS=1 -dMR_STRESS_ENABLE_TRIANGLES=1 ^
  -i=third_party\microrender\shared\src ^
  -i=third_party\microrender\microrender_dos\dos ^
  -i=third_party\microwave\shared\src ^
  src\dos\main.c src\dos\mc_sb.c ^
  third_party\microrender\microrender_dos\dos\dos_vga.c ^
  third_party\microrender\shared\src\gfx.c ^
  third_party\microrender\shared\src\gfx_font5x7.c ^
  third_party\microrender\shared\src\gfx_triangle.c ^
  third_party\microrender\shared\src\gfx_rgb444.c ^
  third_party\microrender\shared\src\mr_stress_test.c ^
  third_party\microrender\shared\src\mr_strbuf.c ^
  third_party\microwave\shared\src\snd.c ^
  third_party\microwave\shared\src\snd_synth.c ^
  third_party\microwave\shared\src\snd_seq.c ^
  third_party\microwave\shared\src\mw_music_demo.c || exit /b 1
echo built build-dos\MCDEMO.EXE
exit /b 0

:pico
set "DEVICE=%~1"
if "%DEVICE%"=="" set "DEVICE=max98357a"
if not "%~1"=="" shift /1
set "MC_PICO_ARGS="
:pico_args
if "%~1"=="" goto pico_args_done
set "MC_PICO_ARGS=!MC_PICO_ARGS! "%~1""
shift /1
goto pico_args
:pico_args_done
call "%MC_ROOT%\third_party\microwave\microwave\pico_env_auto.bat" || exit /b 1
for %%D in ("%NINJA_EXE%") do set "PATH=%%~dpD;%PICO_TOOLCHAIN_PATH%\bin;%PATH%"
echo === Pico 2 combined demo: %DEVICE% ===
pushd pico
cmake --preset "%DEVICE%" -DCMAKE_MAKE_PROGRAM:FILEPATH="%NINJA_EXE%" !MC_PICO_ARGS! || (popd & exit /b 1)
cmake --build --preset "%DEVICE%" --parallel || (popd & exit /b 1)
popd
echo built pico\build-%DEVICE%\microconsole_demo.uf2
exit /b 0

:all
call "%~f0" raylib || exit /b 1
if not "%WATCOM%"=="" call "%~f0" dos
if not errorlevel 1 if exist "%USERPROFILE%\.pico-sdk\sdk\2.2.0\pico_sdk_init.cmake" call "%~f0" pico max98357a
exit /b 0

:no_deps
echo ERROR: submodules are not initialized.
echo Run: .\mc.bat deps
exit /b 1
