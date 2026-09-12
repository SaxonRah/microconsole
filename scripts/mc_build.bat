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
if /i "%WHAT%"=="dos-examples" goto dos_examples
if /i "%WHAT%"=="fastdoom-raylib" goto fastdoom_raylib
if /i "%WHAT%"=="fastdoom-pico" goto fastdoom_pico
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
if not exist build-dos\obj mkdir build-dos\obj
set "PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%WATCOM%\binw;%PATH%"
set "INCLUDE=%WATCOM%\h;%WATCOM%\h\nt"
where wcc >nul 2>nul || (echo ERROR: wcc.exe not found under %WATCOM%.& exit /b 1)
where wcl >nul 2>nul || (echo ERROR: wcl.exe not found under %WATCOM%.& exit /b 1)

echo === 16-bit DOS combined demo ===
rem Keep the renderer objects byte-for-byte comparable with MicroRender's
rem standalone build_watcom_stress.bat. Do not add MicroConsole-only renderer
rem defines here: all three DOS binaries below share these exact objects.
set "MC_CFLAGS=-q -bt=dos -ml -2 -ox -s -w4 -dGFX_FIXED_NO_INT64 -dGFX_COLOR_INDEX8=0 -dGFX_ENABLE_TRIANGLES=1 -dMR_STRESS_MAX_SPRITES=1024 -dMR_DOS_TILE_H=16 -dMR_DOS_VSYNC=0 -dMR_DOS_PRESENT_MODE=1"
set "MC_INCLUDES=-i=third_party\microrender\shared\src -i=third_party\microrender\microrender_dos\dos -i=third_party\microwave\shared\src"
set "MC_OBJ=build-dos\obj"

echo [dos] compiling shared MicroRender objects...
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\gfx.obj third_party\microrender\shared\src\gfx.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\gfx_font5x7.obj third_party\microrender\shared\src\gfx_font5x7.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\gfx_triangle.obj third_party\microrender\shared\src\gfx_triangle.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\mr_stress_test.obj third_party\microrender\shared\src\mr_stress_test.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\mr_strbuf.obj third_party\microrender\shared\src\mr_strbuf.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\dos_vga.obj third_party\microrender\microrender_dos\dos\dos_vga.c || exit /b 1

echo [dos] compiling three frontends for A/B isolation...
rem Exact upstream frontend, but linked from the same renderer objects as the
rem combined program. This should reproduce mstress.exe performance.
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\mcref_main.obj third_party\microrender\microrender_dos\dos\dos_stress_app.c || exit /b 1
rem MicroConsole loop with every audio reference removed at preprocessing time.
wcc %MC_CFLAGS% %MC_INCLUDES% -dMC_DOS_AUDIO=0 -fo=%MC_OBJ%\mcgfx_main.obj src\dos\main.c || exit /b 1
rem Real combined frontend.
wcc %MC_CFLAGS% %MC_INCLUDES% -dMC_DOS_AUDIO=1 -fo=%MC_OBJ%\mcdemo_main.obj src\dos\main.c || exit /b 1

echo [dos] compiling MicroWave/Sound Blaster objects...
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\mc_sb.obj src\dos\mc_sb.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\snd.obj third_party\microwave\shared\src\snd.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\snd_synth.obj third_party\microwave\shared\src\snd_synth.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\snd_seq.obj third_party\microwave\shared\src\snd_seq.c || exit /b 1
wcc %MC_CFLAGS% %MC_INCLUDES% -fo=%MC_OBJ%\mw_music_demo.obj third_party\microwave\shared\src\mw_music_demo.c || exit /b 1

set "MC_RENDER_OBJS=%MC_OBJ%\gfx.obj %MC_OBJ%\gfx_font5x7.obj %MC_OBJ%\gfx_triangle.obj %MC_OBJ%\mr_strbuf.obj %MC_OBJ%\dos_vga.obj %MC_OBJ%\mr_stress_test.obj"
set "MC_AUDIO_OBJS=%MC_OBJ%\mc_sb.obj %MC_OBJ%\snd.obj %MC_OBJ%\snd_synth.obj %MC_OBJ%\snd_seq.obj %MC_OBJ%\mw_music_demo.obj"

echo [dos] linking MCREF.EXE ^(exact upstream frontend^) ...
wcl -q -bt=dos -ml -fe=build-dos\MCREF.EXE %MC_OBJ%\mcref_main.obj %MC_RENDER_OBJS% || exit /b 1

echo [dos] linking MCGFX.EXE ^(MicroConsole loop, no audio code^) ...
wcl -q -bt=dos -ml -fe=build-dos\MCGFX.EXE %MC_OBJ%\mcgfx_main.obj %MC_RENDER_OBJS% || exit /b 1

echo [dos] linking MCDEMO.EXE ^(MicroRender + MicroWave^) ...
wcl -q -bt=dos -ml -fe=build-dos\MCDEMO.EXE %MC_OBJ%\mcdemo_main.obj %MC_RENDER_OBJS% %MC_AUDIO_OBJS% || exit /b 1

echo built build-dos\MCREF.EXE
echo built build-dos\MCGFX.EXE
echo built build-dos\MCDEMO.EXE
exit /b 0

:dos_examples
call "%MC_ROOT%\scripts\mc_examples_dos.bat"
exit /b %ERRORLEVEL%

:fastdoom_raylib
where cmake >nul 2>nul || (echo ERROR: cmake not found on PATH.& exit /b 1)
where python >nul 2>nul || (echo ERROR: python not found on PATH.& exit /b 1)

if not exist "third_party\microrender\third_party\raylib\CMakeLists.txt" (
    echo ERROR: MicroRender's Raylib submodule is not initialized.
    echo Run: .\mc.bat deps
    exit /b 1
)

call "%MC_ROOT%\scripts\mc_fastdoom_deps.bat"
if errorlevel 1 exit /b 1

python "%MC_ROOT%\scripts\mc_fastdoom_prepare.py" ^
    --fastdoom "%MC_ROOT%\third_party\fastdoom" ^
    --out "%MC_ROOT%\build-fastdoom-src"
if errorlevel 1 exit /b 1

echo === FastDoom standalone Raylib ^(Win32 portable C renderer^) ===
set "MC_FD_CMAKE_ARGS="
:fastdoom_args
if "%~1"=="" goto fastdoom_args_done
set "MC_FD_CMAKE_ARGS=!MC_FD_CMAKE_ARGS! "%~1""
shift /1
goto fastdoom_args
:fastdoom_args_done

cmake -S "%MC_ROOT%\games\fastdoom" ^
      -B "%MC_ROOT%\build-fastdoom-raylib" ^
      -G "Visual Studio 17 2022" ^
      -A Win32 ^
      -DFASTDOOM_ROOT:PATH="%MC_ROOT%\third_party\fastdoom" ^
      -DFASTDOOM_PATCHED_DIR:PATH="%MC_ROOT%\build-fastdoom-src" ^
      !MC_FD_CMAKE_ARGS!
if errorlevel 1 exit /b 1

cmake --build "%MC_ROOT%\build-fastdoom-raylib" --config Release --parallel
if errorlevel 1 exit /b 1

if not exist "%MC_ROOT%\build-fastdoom-raylib\Release\microconsole_fastdoom.exe" (
    echo ERROR: FastDoom build finished but microconsole_fastdoom.exe was not found.
    exit /b 1
)

echo built build-fastdoom-raylib\Release\microconsole_fastdoom.exe
exit /b 0


:fastdoom_pico
where python >nul 2>nul || (echo ERROR: python not found on PATH.& exit /b 1)

set "DEVICE=%~1"
if "%DEVICE%"=="" set "DEVICE=max98357a"
if not "%~1"=="" shift /1

set "PANEL=%~1"
if "%PANEL%"=="" set "PANEL=ili9341"
if not "%~1"=="" shift /1

if /i "%PANEL%"=="ili9341" (
    set "MC_FD_PANEL_CMAKE=ILI9341"
) else if /i "%PANEL%"=="st7796s" (
    set "MC_FD_PANEL_CMAKE=ST7796S"
) else (
    echo ERROR: FastDoom Pico panel must be ili9341 or st7796s.
    exit /b 1
)

set "MC_FD_PICO_ARGS="
:fastdoom_pico_args
if "%~1"=="" goto fastdoom_pico_args_done
set "MC_FD_PICO_ARGS=!MC_FD_PICO_ARGS! "%~1""
shift /1
goto fastdoom_pico_args
:fastdoom_pico_args_done

call "%MC_ROOT%\scripts\mc_fastdoom_deps.bat"
if errorlevel 1 exit /b 1

call "%MC_ROOT%\scripts\mc_fastdoom_pico_sd_deps.bat"
if errorlevel 1 exit /b 1

python "%MC_ROOT%\scripts\mc_fastdoom_prepare.py" ^
    --fastdoom "%MC_ROOT%\third_party\fastdoom" ^
    --out "%MC_ROOT%\build-fastdoom-src"
if errorlevel 1 exit /b 1

python "%MC_ROOT%\scripts\mc_fastdoom_pico_prepare.py" ^
    --src "%MC_ROOT%\build-fastdoom-src\FASTDOOM"
if errorlevel 1 exit /b 1

echo === FastDoom Pico 2 Cortex-M33 bring-up: %DEVICE% / %MC_FD_PANEL_CMAKE% ===
call "%~f0" pico "%DEVICE%" "-DMC_FASTDOOM_LCD_PANEL=%MC_FD_PANEL_CMAKE%" !MC_FD_PICO_ARGS!
if errorlevel 1 exit /b 1

if not exist "%MC_ROOT%\pico\build-%DEVICE%\microconsole_fastdoom_pico.uf2" (
    echo ERROR: Pico build completed but microconsole_fastdoom_pico.uf2 was not found.
    exit /b 1
)

echo built pico\build-%DEVICE%\microconsole_fastdoom_pico.uf2
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

rem MicroConsole standardizes every Pico target on SDK 2.3.0.
rem Do this before calling the dependency helper so an old shell-level
rem PICO_SDK_PATH cannot mix SDK trees inside a CMake cache.
set "MC_PICO_REQUIRED_SDK=2.3.0"
set "PICO_SDK_PATH=%USERPROFILE%\.pico-sdk\sdk\%MC_PICO_REQUIRED_SDK%"
if not exist "%PICO_SDK_PATH%\pico_sdk_init.cmake" (
    echo ERROR: MicroConsole Pico requires Pico SDK %MC_PICO_REQUIRED_SDK%:
    echo   %PICO_SDK_PATH%
    exit /b 1
)

call "%MC_ROOT%\third_party\microwave\microwave\pico_env_auto.bat" || exit /b 1

rem Pico SDK 2.3+ may build pioasm/picotool as native Windows host tools.
rem Keep those tools completely isolated from a global Open Watcom environment
rem and force a compiler that matches the installed Visual Studio STL.
set "INCLUDE="
set "LIB="
set "LIBPATH="
set "CPATH="
set "C_INCLUDE_PATH="
set "CPLUS_INCLUDE_PATH="
set "CC="
set "CXX="

set "MC_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MC_VSINSTALL="

if exist "%MC_VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%MC_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if not defined MC_VSINSTALL set "MC_VSINSTALL=%%I"
    )
)

if not defined MC_VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" (
    set "MC_VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
)
if not defined MC_VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" (
    set "MC_VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
)
if not defined MC_VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat" (
    set "MC_VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
)
if not defined MC_VSINSTALL if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
    set "MC_VSINSTALL=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
)

if not defined MC_VSINSTALL (
    echo ERROR: Visual Studio C++ host tools were not found.
    echo Pico SDK 2.3+ needs a native compiler for pioasm/picotool.
    exit /b 1
)

call "%MC_VSINSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

set "CC=cl.exe"
set "CXX=cl.exe"

where cl.exe >nul 2>nul || (
    echo ERROR: cl.exe was not available after VsDevCmd.
    exit /b 1
)

for %%D in ("%NINJA_EXE%") do set "PATH=%%~dpD;%PICO_TOOLCHAIN_PATH%\bin;%PATH%"

echo === Pico 2 combined demo: %DEVICE% ===
echo Host tools: MSVC cl.exe; RP2350 target: arm-none-eabi-gcc
pushd pico
cmake --preset "%DEVICE%" -DCMAKE_MAKE_PROGRAM:FILEPATH="%NINJA_EXE%" !MC_PICO_ARGS! || (popd & exit /b 1)
cmake --build --preset "%DEVICE%" --parallel || (popd & exit /b 1)
popd
echo built pico\build-%DEVICE%\microconsole_demo.uf2
exit /b 0

:all
call "%~f0" raylib || exit /b 1
if not "%WATCOM%"=="" call "%~f0" dos
if not errorlevel 1 if exist "%USERPROFILE%\.pico-sdk\sdk\2.3.0\pico_sdk_init.cmake" call "%~f0" pico max98357a
exit /b 0

:no_deps
echo ERROR: submodules are not initialized.
echo Run: .\mc.bat deps
exit /b 1
