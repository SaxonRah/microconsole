@echo off
setlocal EnableExtensions EnableDelayedExpansion
if "%MC_ROOT%"=="" set "MC_ROOT=%~dp0.."
cd /d "%MC_ROOT%"
set "WHAT=%~1"
shift /1

if /i "%WHAT%"=="raylib" goto raylib
if /i "%WHAT%"=="dos" goto dos
if /i "%WHAT%"=="dosgfx" goto dosgfx
if /i "%WHAT%"=="dosref" goto dosref
if /i "%WHAT%"=="dos-examples" goto dos_examples
if /i "%WHAT%"=="pico" goto pico
echo ERROR: run target must be raylib, dos, dosgfx, dosref, dos-examples, or pico.
exit /b 1

:raylib
call "%MC_ROOT%\scripts\mc_build.bat" raylib || exit /b 1
set "EXE=%MC_ROOT%\build-raylib\microconsole_demo.exe"
if not exist "!EXE!" set "EXE=%MC_ROOT%\build-raylib\Release\microconsole_demo.exe"
if not exist "!EXE!" (
  echo ERROR: Raylib build completed but microconsole_demo.exe was not found.
  exit /b 1
)
set "MC_FORWARD_ARGS="
:raylib_run_args
if "%~1"=="" goto raylib_launch
set "MC_FORWARD_ARGS=!MC_FORWARD_ARGS! "%~1""
shift /1
goto raylib_run_args
:raylib_launch
"!EXE!" !MC_FORWARD_ARGS!
exit /b !ERRORLEVEL!

:dos
set "MC_DOS_EXE=MCDEMO.EXE"
set "MC_DOS_DIR=build-dos"
set "MC_DOS_BUILD=dos"
goto dos_common

:dosgfx
set "MC_DOS_EXE=MCGFX.EXE"
set "MC_DOS_DIR=build-dos"
set "MC_DOS_BUILD=dos"
goto dos_common

:dosref
set "MC_DOS_EXE=MCREF.EXE"
set "MC_DOS_DIR=build-dos"
set "MC_DOS_BUILD=dos"
goto dos_common

:dos_examples
set "MC_DOS_EXE=MCEXDEMO.EXE"
set "MC_DOS_DIR=build-dos-examples"
set "MC_DOS_BUILD=dos-examples"
goto dos_common

:dos_common
if /i "!MC_DOS_BUILD!"=="dos-examples" (
  call "%MC_ROOT%\scripts\mc_examples_dos.bat"
) else (
  call "%MC_ROOT%\scripts\mc_build.bat" dos
)
if errorlevel 1 exit /b 1

if not exist "%MC_ROOT%\!MC_DOS_DIR!\!MC_DOS_EXE!" (
  echo ERROR: DOS build completed but !MC_DOS_EXE! was not found in !MC_DOS_DIR!.
  exit /b 1
)
set "DOSBOX="
if not "%DOSBOX_EXE%"=="" if exist "%DOSBOX_EXE%" set "DOSBOX=%DOSBOX_EXE%"
if not defined DOSBOX for %%E in (dosbox-x.exe dosbox.exe DOSBox.exe) do if not defined DOSBOX for /f "delims=" %%P in ('where %%E 2^>nul') do if not defined DOSBOX set "DOSBOX=%%P"
if not defined DOSBOX if exist "%ProgramFiles%\DOSBox-0.74-3\DOSBox.exe" set "DOSBOX=%ProgramFiles%\DOSBox-0.74-3\DOSBox.exe"
if not defined DOSBOX if exist "%ProgramFiles(x86)%\DOSBox-0.74-3\DOSBox.exe" set "DOSBOX=%ProgramFiles(x86)%\DOSBox-0.74-3\DOSBox.exe"
if not defined DOSBOX (echo ERROR: DOSBox not found.& exit /b 1)

if "%MC_DOSBOX_CYCLES%"=="" set "MC_DOSBOX_CYCLES=max"
set "MC_DOS_ARGS="
:dos_run_args
if "%~1"=="" goto dos_launch
set "MC_DOS_ARGS=!MC_DOS_ARGS! %~1"
shift /1
goto dos_run_args

:dos_launch
set "CONF=%TEMP%\microconsole_%RANDOM%.conf"
> "%CONF%" echo [dosbox]
>>"%CONF%" echo machine=vgaonly
>>"%CONF%" echo [sdl]
>>"%CONF%" echo autolock=false
>>"%CONF%" echo [cpu]
>>"%CONF%" echo core=dynamic
>>"%CONF%" echo cycles=%MC_DOSBOX_CYCLES%
>>"%CONF%" echo [render]
>>"%CONF%" echo frameskip=0
>>"%CONF%" echo aspect=false
>>"%CONF%" echo [autoexec]
>>"%CONF%" echo mount c "%MC_ROOT%\!MC_DOS_DIR!"
>>"%CONF%" echo c:
>>"%CONF%" echo set BLASTER=A220 I7 D1
>>"%CONF%" echo echo MicroConsole: !MC_DOS_EXE! !MC_DOS_ARGS!  [cycles=%MC_DOSBOX_CYCLES%]
>>"%CONF%" echo !MC_DOS_EXE! !MC_DOS_ARGS!
>>"%CONF%" echo echo.
if not "%MC_DOSBOX_NOPAUSE%"=="1" (
  >>"%CONF%" echo echo Finished. Press any key to close DOSBox.
  >>"%CONF%" echo pause
)
>>"%CONF%" echo exit

echo Launching !MC_DOS_EXE! !MC_DOS_ARGS! ^(cycles=%MC_DOSBOX_CYCLES%, machine=vgaonly, core=dynamic^)
start "" /wait "%DOSBOX%" -conf "%CONF%"
set "RC=%ERRORLEVEL%"
del "%CONF%" >nul 2>nul
exit /b %RC%

:pico
set "DEVICE=%~1"
if "%DEVICE%"=="" set "DEVICE=max98357a"

set "METHOD=%~2"
if "%METHOD%"=="" set "METHOD=swd"

rem Deliberately use a plain positional number for the startup volume.
rem This avoids nested batch/CMake parsing of -DNAME=VALUE.
set "PICO_VOLUME=%~3"
if "%PICO_VOLUME%"=="" set "PICO_VOLUME=100"

for /f "delims=0123456789" %%A in ("%PICO_VOLUME%") do (
  if not "%%A"=="" (
    echo ERROR: Pico startup volume must be an integer from 0 to 100.
    exit /b 1
  )
)
if %PICO_VOLUME% LSS 0 (
  echo ERROR: Pico startup volume must be 0..100.
  exit /b 1
)
if %PICO_VOLUME% GTR 100 (
  echo ERROR: Pico startup volume must be 0..100.
  exit /b 1
)

echo Pico startup volume: %PICO_VOLUME%%%
set "MC_AUDIO_VOLUME_OVERRIDE=%PICO_VOLUME%"
call "%MC_ROOT%\scripts\mc_build.bat" pico "%DEVICE%"
set "MC_AUDIO_VOLUME_OVERRIDE="
if errorlevel 1 exit /b 1

python "%MC_ROOT%\scripts\mc_pico.py" flash "%DEVICE%" "%METHOD%"
exit /b %ERRORLEVEL%
