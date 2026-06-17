@echo off
cd /d "%~dp0"
echo ========================================
echo   MuOS ISO Boot
echo ========================================
echo Building and creating ISO...
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
powershell -ExecutionPolicy Bypass -File build.ps1 -Iso
echo.
echo Starting QEMU with CD-ROM...
echo Click the QEMU window to type.
echo ========================================
"C:\Program Files\qemu\qemu-system-i386.exe" -cdrom "%~dp0muos.iso" -m 128M
