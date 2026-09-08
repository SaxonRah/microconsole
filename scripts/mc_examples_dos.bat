@echo off
rem Build the console-era examples for 16-bit DOS.
rem
rem Kept separate from scripts\mc_build.bat on purpose. That script's :dos
rem section compiles a specific set of MicroRender objects and then links three
rem executables from them so that renderer performance stays comparable across
rem MCREF, MCGFX and MCDEMO. Adding targets with different compiler flags into
rem the middle of that would break the property it exists to protect.
rem
rem Produces:
rem   MCEXDEMO.EXE   the examples, Mode X + Sound Blaster
rem   MCEXCAP.EXE    headless capture, for the cross-target diff
rem
rem Usage:  scripts\mc_examples_dos.bat

setlocal EnableExtensions EnableDelayedExpansion
if "%MC_ROOT%"=="" set "MC_ROOT=%~dp0.."
cd /d "%MC_ROOT%"

if "%WATCOM%"=="" (
    echo ERROR: WATCOM is not set.
    exit /b 1
)
set "PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%WATCOM%\binw;%PATH%"
set "INCLUDE=%WATCOM%\h;%WATCOM%\h\nt"
where wcc >nul 2>nul || (echo ERROR: wcc.exe not found under %WATCOM%.& exit /b 1)

if not exist build-dos-examples mkdir build-dos-examples
set "OBJ=build-dos-examples"

set "MR=third_party\microrender\shared\src"
set "MRD=third_party\microrender\microrender_dos\dos"
set "MW=third_party\microwave\shared\src"

rem -zt0 puts every data object in a far segment rather than DGROUP.
rem
rem Without it the link fails with "size of group DGROUP exceeds 64k". The nine
rem examples each carry a few KB of palettes, tile art, wavetables and shrink
rem tables; individually all are small, and together they are well past the
rem 64 KB near-data limit. The engines themselves are built without it, exactly
rem as scripts\mc_build.bat does, so their objects stay byte-comparable.
set "EXCF=-q -bt=dos -ml -2 -ox -s -w4 -zt0 -dGFX_FIXED_NO_INT64"
set "ENGCF=-q -bt=dos -ml -2 -ox -s -w4 -dGFX_FIXED_NO_INT64"
set "INC=-i=%MR% -i=%MRD% -i=%MW% -i=examples -i=src\dos"

echo [dos] engines and DOS backend...
for %%F in (gfx gfx_font5x7 gfx_triangle mr_strbuf) do (
    wcc %ENGCF% %INC% -fo=%OBJ%\%%F.obj %MR%\%%F.c || exit /b 1
)
for %%F in (snd snd_synth snd_seq mw_music_demo) do (
    wcc %ENGCF% %INC% -fo=%OBJ%\%%F.obj %MW%\%%F.c || exit /b 1
)
for %%F in (dos_vga dos_keyboard) do (
    wcc %ENGCF% %INC% -fo=%OBJ%\%%F.obj %MRD%\%%F.c || exit /b 1
)

echo [dos] example library...
for %%F in (mc_example mc_ex_gfx mc_ex_chip) do (
    wcc %EXCF% %INC% -fo=%OBJ%\%%F.obj examples\%%F.c || exit /b 1
)
for %%F in (ex_a2600_beam ex_nes_split ex_sms_lock ex_gb_window ex_pce_wavetable ex_md_linescroll ex_snes_mode7 ex_snes_colormath ex_neogeo_zoom) do (
    wcc %EXCF% %INC% -fo=%OBJ%\%%F.obj examples\%%F.c || exit /b 1
)

echo [dos] frontends...
wcc %EXCF% %INC% -fo=%OBJ%\mc_sb.obj src\dos\mc_sb.c || exit /b 1
wcc %EXCF% %INC% -fo=%OBJ%\mcexdemo.obj src\dos\mc_examples_main.c || exit /b 1
wcc %EXCF% %INC% -fo=%OBJ%\mcexcap.obj tools\mc_example_capture_dos.c || exit /b 1

set "EXOBJ=%OBJ%\mc_example.obj %OBJ%\mc_ex_gfx.obj %OBJ%\mc_ex_chip.obj"
for %%F in (ex_a2600_beam ex_nes_split ex_sms_lock ex_gb_window ex_pce_wavetable ex_md_linescroll ex_snes_mode7 ex_snes_colormath ex_neogeo_zoom) do (
    set "EXOBJ=!EXOBJ! %OBJ%\%%F.obj"
)
set "ENGOBJ=%OBJ%\gfx.obj %OBJ%\gfx_font5x7.obj %OBJ%\gfx_triangle.obj %OBJ%\mr_strbuf.obj %OBJ%\snd.obj %OBJ%\snd_synth.obj"

echo [dos] linking MCEXDEMO.EXE...
wlink system dos option quiet,map=%OBJ%\mcexdemo.map name %OBJ%\MCEXDEMO.EXE ^
  file { %OBJ%\mcexdemo.obj %OBJ%\mc_sb.obj %OBJ%\dos_vga.obj %OBJ%\dos_keyboard.obj ^
         %OBJ%\mw_music_demo.obj %OBJ%\snd_seq.obj !EXOBJ! %ENGOBJ% } || exit /b 1

echo [dos] linking MCEXCAP.EXE...
wlink system dos option quiet name %OBJ%\MCEXCAP.EXE ^
  file { %OBJ%\mcexcap.obj !EXOBJ! %ENGOBJ% } || exit /b 1

echo.
echo built %OBJ%\MCEXDEMO.EXE  ^(Mode X + Sound Blaster^)
echo built %OBJ%\MCEXCAP.EXE   ^(headless capture for the cross-target diff^)
echo.
echo   MCEXDEMO /list
echo   MCEXDEMO /example snes-mode7
echo   MCEXCAP nes-split 85 80        writes OUT.PPM and OUT.RAW
exit /b 0
