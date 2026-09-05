@echo off
cd /d "%~dp0"
echo ========================================
echo   MuOS Quick Start
echo ========================================
powershell -ExecutionPolicy Bypass -File build.ps1 -Run
