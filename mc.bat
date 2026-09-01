@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "MC_ROOT=%CD%"

rem Pinned engine revisions. These are also the repository's gitlink revisions.
set "MC_MR_SHA=61d8d875cbdf605edaa2b6ca2f2e5739b80ee61d"
set "MC_MW_SHA=2492cc4026a5e3cdebbac6dea91dbc9eabe88d91"

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

rem A normal clone already has these two mode-160000 entries.  A source tree
rem copied into another Git repository can retain .gitmodules while losing the
rem gitlinks, which makes `git submodule update path` fail with "pathspec did
rem not match any file(s) known to git".  Repair that exact condition here.
git rev-parse --is-inside-work-tree >nul 2>nul || (
    echo ERROR: MicroConsole is not inside a Git working tree.
    echo Re-extract the repository archive including its .git directory, or run
    echo `git init` here before `mc.bat deps` if this is intentionally a new repo.
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

rem Force the working trees to the versions the integration was written and
rem tested against.  Normally submodule update already did this; these checks
rem also make a manually-populated dependency directory deterministic.
git -C third_party/microrender checkout --detach "%MC_MR_SHA%" || exit /b 1
git -C third_party/microwave checkout --detach "%MC_MW_SHA%" || exit /b 1

rem Only MicroRender's Raylib checkout is needed by this repo. Both projects
rem pin the same Raylib revision, so recursively initializing both would waste
rem hundreds of MB on a duplicate checkout.
git -C third_party/microrender submodule update --init third_party/raylib || exit /b 1

echo dependencies ready.
exit /b 0

:ensure_gitlink
set "GL_PATH=%~1"
set "GL_SHA=%~2"
set "GL_MODE="
for /f "tokens=1" %%M in ('git ls-files --stage -- "%GL_PATH%" 2^>nul') do set "GL_MODE=%%M"
if "!GL_MODE!"=="160000" exit /b 0

echo repairing missing gitlink: %GL_PATH%

rem If this is merely an empty placeholder directory from an archive, remove
rem it so Git can clone the submodule there. Never delete a populated checkout.
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
for %%D in (build-raylib build-dos) do if exist "%%D" rmdir /s /q "%%D"
for /d %%D in (pico\build-*) do if exist "%%D" rmdir /s /q "%%D"
echo clean.
exit /b 0

:help
echo MicroConsole Demo - MicroRender + MicroWave together
echo.
echo   .\mc.bat deps
echo   .\mc.bat build raylib
echo   .\mc.bat run raylib
echo   .\mc.bat build dos
echo   .\mc.bat run dos
echo   .\mc.bat build pico max98357a ^| pcm5102a ^| ns4168
echo   .\mc.bat run pico [device] [swd^|picotool^|manual]
echo   .\mc.bat clean
echo.
exit /b 0
