@echo off
cd /d "%~dp0"
echo ========================================
echo   MuOS ISO Boot
echo ========================================
powershell -ExecutionPolicy Bypass -File build.ps1 -Iso -Run
