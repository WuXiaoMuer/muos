@echo off
setlocal
cd /d "%~dp0"

rem MuOS launcher.
rem   run.bat              ISO boot: VBE framebuffer + RTL8139 (pixel works)
rem   run.bat iso          same as above
rem   run.bat kernel       -kernel boot: faster, VGA text mode only
rem   run.bat disk         ISO + build\ata_disk.img (MuFS/ATA tests)
rem   run.bat nobuild      launch the existing artifacts
rem   run.bat help         this message
rem Extra QEMU arguments are forwarded, e.g.:
rem   run.bat iso -- -monitor stdio

set "SW="
set "MODE=%~1"
set "REST=%*"
for /f "tokens=1,*" %%A in ("%REST%") do set "REST=%%B"

if "%MODE%"=="" goto launch
if /i "%MODE%"=="iso" goto launch
if /i "%MODE%"=="kernel" ( set "SW=-Kernel" & goto launch )
if /i "%MODE%"=="disk" ( set "SW=-Disk" & goto launch )
if /i "%MODE%"=="nobuild" ( set "SW=-NoBuild" & goto launch )
if /i "%MODE%"=="help" goto help
if /i "%MODE%"=="-h" goto help
if /i "%MODE%"=="--help" goto help
goto unknown

:launch
echo ========================================
echo   MuOS  (iso: VBE + pixel^| kernel: text^| disk: ATA^)
echo ========================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %SW% %REST%
if errorlevel 1 (
    echo.
    echo [FAILED] run exited with an error.
    pause
)
exit /b 0

:unknown
echo Unknown mode: %MODE%
echo.
goto usage

:help
:usage
echo Usage: run.bat [iso^|kernel^|disk^|nobuild^|help] [-- extra QEMU args]
echo   iso      ISO boot with VBE framebuffer and RTL8139 (default)
echo   kernel   QEMU -kernel boot (fast, VGA text mode)
echo   disk     ISO + build\ata_disk.img for ATA/MuFS tests
echo   nobuild  skip the build and launch current artifacts
if /i "%MODE%"=="help" exit /b 0
if /i "%MODE%"=="-h" exit /b 0
if /i "%MODE%"=="--help" exit /b 0
exit /b 1
