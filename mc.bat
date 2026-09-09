@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "MC_ROOT=%CD%"

rem Pinned engine revisions.
set "MC_MR_SHA=61d8d875cbdf605edaa2b6ca2f2e5739b80ee61d"
set "MC_MW_SHA=432fe71ce2686923f351161543d4fed4f8e67e11"

set "CMD=%~1"
if "%CMD%"=="" set "CMD=help"
shift /1

if /i "%CMD%"=="deps"  goto deps
if /i "%CMD%"=="build" goto build
if /i "%CMD%"=="run"   goto run
if /i "%CMD%"=="clean" goto clean
if /i "%CMD%"=="help"  goto help
if /i "%CMD%"=="-h"    goto help
if /i "%CMD%"=="--help" goto help

echo ERROR: unknown command "%CMD%".
exit /b 1

:deps
echo === MicroConsole dependencies ===
where git >nul 2>nul || (
    echo ERROR: git was not found on PATH.
    exit /b 1
)

git rev-parse --is-inside-work-tree >nul 2>nul || (
    echo ERROR: MicroConsole is not inside a Git working tree.
    exit /b 1
)

if not exist ".gitmodules" (
    echo ERROR: .gitmodules is missing.
    exit /b 1
)

call :ensure_gitlink "third_party/microrender" "%MC_MR_SHA%"
if errorlevel 1 exit /b 1
call :ensure_gitlink "third_party/microwave" "%MC_MW_SHA%"
if errorlevel 1 exit /b 1

git submodule sync --recursive || exit /b 1
git submodule update --init third_party/microrender third_party/microwave || exit /b 1

rem Fetch current remote history so the newer MicroWave volume commit is
rem available even if the repository's gitlink still points at the old pin.
git -C third_party/microrender fetch origin || exit /b 1
git -C third_party/microwave fetch origin || exit /b 1

git -C third_party/microrender checkout --detach "%MC_MR_SHA%" || exit /b 1
git -C third_party/microwave checkout --detach "%MC_MW_SHA%" || exit /b 1

git -C third_party/microrender submodule update --init third_party/raylib || exit /b 1

echo dependencies ready.
echo MicroWave:
git -C third_party/microwave log -1 --oneline

if /i "%~1"=="fastdoom" (
    call "%MC_ROOT%\scripts\mc_fastdoom_deps.bat"
    if errorlevel 1 exit /b 1
)

exit /b 0

:ensure_gitlink
set "GL_PATH=%~1"
set "GL_SHA=%~2"
set "GL_MODE="
for /f "tokens=1" %%M in ('git ls-files --stage -- "%GL_PATH%" 2^>nul') do set "GL_MODE=%%M"
if "!GL_MODE!"=="160000" exit /b 0

echo repairing missing gitlink: %GL_PATH%
if exist "%GL_PATH%\" (
    dir /a /b "%GL_PATH%" 2>nul | findstr . >nul
    if errorlevel 1 rmdir "%GL_PATH%" >nul 2>nul
)
git update-index --add --cacheinfo 160000,%GL_SHA%,%GL_PATH%
if errorlevel 1 (
    echo ERROR: could not restore gitlink for %GL_PATH%.
    exit /b 1
)
exit /b 0

:build
call "%MC_ROOT%\scripts\mc_build.bat" %1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b %ERRORLEVEL%

:run
call "%MC_ROOT%\scripts\mc_run.bat" %1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b %ERRORLEVEL%

:clean
for %%D in (build-raylib build-dos build-dos-examples build-fastdoom-raylib build-fastdoom-src) do if exist "%%D" rmdir /s /q "%%D"
for /d %%D in (pico\build-*) do if exist "%%D" rmdir /s /q "%%D"
echo clean.
exit /b 0

:help
echo MicroConsole - MicroRender + MicroWave integration
echo.
echo Setup:
echo   .\mc.bat deps
echo   .\mc.bat deps fastdoom
echo.
echo Build/run:
echo   .\mc.bat run raylib [--volume N]
echo   .\mc.bat build dos-examples
echo   .\mc.bat run dos [/sprites N] [/frames N] [/volume N] [/noaudio]
echo   .\mc.bat run dos-examples [/example ID] [/frames N] [/volume N] [/noaudio] [/list]
echo   .\mc.bat run pico [device] [swd^|picotool^|manual] [-DMC_AUDIO_VOLUME=N]
echo.
echo FastDoom Raylib:
echo   .\mc.bat build fastdoom-raylib
echo   .\mc.bat run fastdoom-raylib -iwad "C:\path\DOOM1.WAD"
echo.
echo Pico live volume:
echo   python scripts\mc_pico.py volume 50
echo.
echo Maintenance:
echo   .\mc.bat clean
echo.
exit /b 0
