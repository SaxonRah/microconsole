@echo off
setlocal EnableExtensions EnableDelayedExpansion
if "%MC_ROOT%"=="" set "MC_ROOT=%~dp0.."
cd /d "%MC_ROOT%"
set "WHAT=%~1"
shift /1

if /i "%WHAT%"=="raylib" goto raylib
if /i "%WHAT%"=="dos" goto dos
if /i "%WHAT%"=="pico" goto pico
echo ERROR: run target must be raylib, dos, or pico.
exit /b 1

:raylib
set "EXE=%MC_ROOT%\build-raylib\microconsole_demo.exe"
if not exist "!EXE!" set "EXE=%MC_ROOT%\build-raylib\Release\microconsole_demo.exe"
if not exist "!EXE!" (
  call "%MC_ROOT%\scripts\mc_build.bat" raylib || exit /b 1
  set "EXE=%MC_ROOT%\build-raylib\microconsole_demo.exe"
  if not exist "!EXE!" set "EXE=%MC_ROOT%\build-raylib\Release\microconsole_demo.exe"
)
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
if not exist build-dos\MCDEMO.EXE call "%MC_ROOT%\scripts\mc_build.bat" dos || exit /b 1
set "DOSBOX="
if not "%DOSBOX_EXE%"=="" if exist "%DOSBOX_EXE%" set "DOSBOX=%DOSBOX_EXE%"
if not defined DOSBOX for %%E in (dosbox-x.exe dosbox.exe DOSBox.exe) do if not defined DOSBOX for /f "delims=" %%P in ('where %%E 2^>nul') do if not defined DOSBOX set "DOSBOX=%%P"
if not defined DOSBOX if exist "%ProgramFiles%\DOSBox-0.74-3\DOSBox.exe" set "DOSBOX=%ProgramFiles%\DOSBox-0.74-3\DOSBox.exe"
if not defined DOSBOX if exist "%ProgramFiles(x86)%\DOSBox-0.74-3\DOSBox.exe" set "DOSBOX=%ProgramFiles(x86)%\DOSBox-0.74-3\DOSBox.exe"
if not defined DOSBOX (echo ERROR: DOSBox not found.& exit /b 1)

rem Keep the emulator environment identical to MicroRender's standalone DOS
rem benchmark. Mode X is materially slower through some SVGA compatibility
rem paths, and DOSBox's dynamic core is much faster than the normal core for
rem this renderer workload.
if "%MC_DOSBOX_CYCLES%"=="" set "MC_DOSBOX_CYCLES=max"
set "MC_DOS_ARGS="
:dos_run_args
if "%~1"=="" goto dos_launch
rem DOS arguments used by this demo contain no spaces; do not add literal
rem quote characters to the DOS command tail.
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
>>"%CONF%" echo mount c "%MC_ROOT%\build-dos"
>>"%CONF%" echo c:
>>"%CONF%" echo set BLASTER=A220 I7 D1
>>"%CONF%" echo echo MicroConsole: MCDEMO.EXE !MC_DOS_ARGS!  [cycles=%MC_DOSBOX_CYCLES%]
>>"%CONF%" echo MCDEMO.EXE !MC_DOS_ARGS!
>>"%CONF%" echo echo.
if not "%MC_DOSBOX_NOPAUSE%"=="1" (
  >>"%CONF%" echo echo Finished. Press any key to close DOSBox.
  >>"%CONF%" echo pause
)
>>"%CONF%" echo exit

echo Launching MCDEMO.EXE !MC_DOS_ARGS! ^(cycles=%MC_DOSBOX_CYCLES%, machine=vgaonly, core=dynamic^)
start "" /wait "%DOSBOX%" -conf "%CONF%"
set "RC=%ERRORLEVEL%"
del "%CONF%" >nul 2>nul
exit /b %RC%

:pico
set "DEVICE=%~1"
if "%DEVICE%"=="" set "DEVICE=max98357a"
set "METHOD=%~2"
if "%METHOD%"=="" set "METHOD=swd"
call "%MC_ROOT%\scripts\mc_build.bat" pico "%DEVICE%" || exit /b 1
python "%MC_ROOT%\scripts\mc_pico.py" flash "%DEVICE%" "%METHOD%"
exit /b %ERRORLEVEL%
