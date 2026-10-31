@echo off
setlocal
cd /d "%~dp0"

rem Build + smoke-test + package a release into release\.
rem   release.bat           build ISO, verify, zip
rem   release.bat nobuild   package the current artifacts
rem   release.bat noverify  skip the QEMU smoke test
rem   release.bat help

set "SW="
if /i "%~1"=="help" goto usage
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage
if /i "%~1"=="nobuild" set "SW=%SW% -NoBuild"
if /i "%~2"=="nobuild" set "SW=%SW% -NoBuild"
if /i "%~1"=="noverify" set "SW=%SW% -NoVerify"
if /i "%~2"=="noverify" set "SW=%SW% -NoVerify"

echo ========================================
echo   MuOS Release Package
echo ========================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release.ps1" %SW%
if errorlevel 1 (
    echo.
    echo [FAILED] release not created.
    pause
    exit /b 1
)
exit /b 0

:usage
echo Usage: release.bat [nobuild] [noverify]
echo   Builds muos.iso, runs tools\verify_release.py, then writes
echo   release\muos-^<ver^>\ and release\muos-^<ver^>-win32-x86.zip.
echo   nobuild  package the existing build without rebuilding
echo   noverify skip the QEMU smoke test
exit /b 0
