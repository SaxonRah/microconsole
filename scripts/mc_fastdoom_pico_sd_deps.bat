@echo off
setlocal EnableExtensions

if "%MC_ROOT%"=="" set "MC_ROOT=%~dp0.."
cd /d "%MC_ROOT%"

set "FATFS_SHA=995777133afe98a2917bf45ffc7b488d10d62650"
set "FATFS_DIR=%MC_ROOT%\third_party\pico-fatfs-sd"

where git >nul 2>nul || (
    echo ERROR: git was not found on PATH.
    exit /b 1
)

if exist "%FATFS_DIR%" if not exist "%FATFS_DIR%\.git" (
    echo ERROR: "%FATFS_DIR%" exists but is not a Git checkout.
    echo Move/remove it, then retry.
    exit /b 1
)

if not exist "%FATFS_DIR%\.git" (
    echo === Pico FatFS SD dependency ===
    echo cloning inindev/pico-fatfs-sd...
    git clone https://github.com/inindev/pico-fatfs-sd.git "%FATFS_DIR%" || exit /b 1
)

for /f "delims=" %%H in ('git -C "%FATFS_DIR%" rev-parse HEAD 2^>nul') do set "FATFS_HEAD=%%H"

git -C "%FATFS_DIR%" diff --quiet
if errorlevel 1 goto dirty
git -C "%FATFS_DIR%" diff --cached --quiet
if errorlevel 1 goto dirty

if /i "%FATFS_HEAD%"=="%FATFS_SHA%" goto ready

echo fetching pinned pico-fatfs-sd revision...
git -C "%FATFS_DIR%" fetch origin || exit /b 1
git -C "%FATFS_DIR%" checkout --detach "%FATFS_SHA%" || exit /b 1

:ready
echo Pico FatFS SD ready:
git -C "%FATFS_DIR%" log -1 --oneline
exit /b 0

:dirty
echo ERROR: third_party\pico-fatfs-sd has local modifications.
echo Commit/stash/copy those changes before retrying.
exit /b 1
