@echo off
setlocal EnableExtensions

if "%MC_ROOT%"=="" set "MC_ROOT=%~dp0.."
cd /d "%MC_ROOT%"

set "FD_SHA=5580b5e61fc0ad3ba3a0338c264e7e4a0427c3df"
set "FD_DIR=%MC_ROOT%\third_party\fastdoom"

where git >nul 2>nul || (
    echo ERROR: git was not found on PATH.
    exit /b 1
)

if exist "%FD_DIR%" if not exist "%FD_DIR%\.git" (
    echo ERROR: "%FD_DIR%" exists but is not a Git checkout.
    echo Move/remove it, then retry.
    exit /b 1
)

if not exist "%FD_DIR%\.git" (
    echo === FastDoom dependency ===
    echo cloning viti95/FastDoom...
    git clone https://github.com/viti95/FastDoom.git "%FD_DIR%" || exit /b 1
)

for /f "delims=" %%H in ('git -C "%FD_DIR%" rev-parse HEAD 2^>nul') do set "FD_HEAD=%%H"

git -C "%FD_DIR%" diff --quiet
if errorlevel 1 goto dirty
git -C "%FD_DIR%" diff --cached --quiet
if errorlevel 1 goto dirty

if /i "%FD_HEAD%"=="%FD_SHA%" goto ready

echo fetching pinned FastDoom revision...
git -C "%FD_DIR%" fetch origin || exit /b 1
git -C "%FD_DIR%" checkout --detach "%FD_SHA%" || exit /b 1

:ready
echo FastDoom ready:
git -C "%FD_DIR%" log -1 --oneline
exit /b 0

:dirty
echo ERROR: third_party\fastdoom has local modifications.
echo The MicroConsole FastDoom build intentionally uses a clean pinned checkout.
echo Commit/stash/copy those changes before retrying.
exit /b 1
