@echo off
setlocal
cd /d "%~dp0"

rem Remove build\ and muos.iso. Toolchains (toolchain\, tools\nasm\,
rem tools\qemu\) and the Python test tools are NOT touched.

if /i "%~1"=="help" goto usage
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage

echo ========================================
echo   MuOS Clean
echo   removing build\ and muos.iso ...
echo ========================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Clean
if errorlevel 1 (
    echo.
    echo [FAILED] clean failed.
    pause
    exit /b 1
)
echo Done. Run run.bat to rebuild.
exit /b 0

:usage
echo Usage: clean.bat
echo   Deletes build\ and muos.iso. Keeps toolchain\, tools\ and sources.
exit /b 0
