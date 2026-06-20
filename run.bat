@echo off
cd /d "%~dp0"
echo ========================================
echo   MuOS Quick Start
echo ========================================
echo Building...
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
powershell -ExecutionPolicy Bypass -File build.ps1
echo.
echo Starting QEMU...
echo Click the QEMU window to type.
echo ========================================
"C:\Program Files\qemu\qemu-system-i386.exe" -kernel "%~dp0build\kernel.elf" -m 256M
