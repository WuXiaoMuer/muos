@echo off
setlocal
cd /d "%~dp0"

rem ISO (VBE framebuffer) boot - required for the `pixel` window manager.
rem Equivalent to `run.bat iso`; extra arguments are forwarded to run.ps1.

if /i "%~1"=="help" goto usage
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage

echo ========================================
echo   MuOS ISO Boot  (VBE framebuffer)
echo ========================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*
if errorlevel 1 pause
exit /b 0

:usage
echo Usage: run_iso.bat [-- extra QEMU args]
echo   Builds muos.iso (incrementally) and boots it with VBE + RTL8139.
echo   Use run.bat kernel for the faster VGA text-mode path.
exit /b 1
