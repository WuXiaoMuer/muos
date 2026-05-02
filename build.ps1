# build.ps1 - MuOS Windows local build script
# Usage: Right-click -> Run with PowerShell, or run .\build.ps1 in PowerShell

param(
    [switch]$Run,
    [switch]$Iso,
    [switch]$Clean,
    [switch]$Grub   # 使用 GRUB/multiboot2 方案编译（需 grub-mkrescue）
)

$ErrorActionPreference = "Stop"

# ======================== Tool path search ========================

function Find-Tool {
    param([string]$Name, [string[]]$ExtraPaths)
    # 1. Search in PATH
    $inPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($inPath) { return $inPath.Source }

    # 2. Search common install paths
    $commonPaths = @(
        "C:\Program Files\NASM\$Name.exe",
        "C:\Program Files (x86)\NASM\$Name.exe",
        "C:\nasm\$Name.exe",
        "C:\qemu\$Name.exe",
        "C:\Program Files\qemu\$Name.exe",
        "C:\Program Files (x86)\qemu\$Name.exe",
        "C:\i686-elf-tools\bin\$Name.exe",
        "C:\Tools\i686-elf-tools\bin\$Name.exe",
        "$env:USERPROFILE\i686-elf-tools\bin\$Name.exe",
        "$env:USERPROFILE\tools\i686-elf-tools\bin\$Name.exe",
        "$PSScriptRoot\tools\i686-elf-tools\bin\$Name.exe",
        "$PSScriptRoot\toolchain\bin\$Name.exe",
        "$PSScriptRoot\toolchain\i686-elf\bin\$Name.exe",
        "$PSScriptRoot\tools\nasm\$Name.exe",
        "$PSScriptRoot\tools\qemu\$Name.exe"
    ) + $ExtraPaths

    foreach ($p in $commonPaths) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

$NASM      = Find-Tool "nasm"
$GCC       = Find-Tool "i686-elf-gcc"
$LD        = Find-Tool "i686-elf-ld"
$QEMU      = Find-Tool "qemu-system-i386"
$GrubMkRescue = Find-Tool "grub-mkrescue"

# ======================== Clean ========================

if ($Clean) {
    Write-Host ">>> Cleaning build artifacts..." -ForegroundColor Cyan
    $toRemove = @("boot.o", "kernel.o", "kernel.elf", "muos.iso", "iso")
    foreach ($f in $toRemove) {
        if (Test-Path $f) {
            Remove-Item -Recurse -Force $f
            Write-Host "  Deleted: $f"
        }
    }
    Write-Host ">>> Clean complete" -ForegroundColor Green
    exit 0
}

# ======================== Check required tools ========================

$missing = @()
if (-not $NASM) { $missing += "nasm" }
if (-not $GCC)  { $missing += "i686-elf-gcc" }
if (-not $LD)   { $missing += "i686-elf-ld" }

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "[ERROR] Missing required tools:" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host "  - $m" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "Please install the missing tools and add them to PATH." -ForegroundColor White
    Write-Host ""
    Write-Host "Quick install guide:" -ForegroundColor Cyan
    Write-Host "  1. NASM          : https://www.nasm.us/" -ForegroundColor Gray
    Write-Host "  2. i686-elf-gcc  : https://github.com/lordmilko/i686-elf-tools/releases" -ForegroundColor Gray
    Write-Host "  3. QEMU          : https://www.qemu.org/download/#windows" -ForegroundColor Gray
    Write-Host ""
    exit 1
}

Write-Host ""
Write-Host "======================== MuOS Build ========================" -ForegroundColor Cyan
Write-Host "NASM      : $NASM"
Write-Host "GCC       : $GCC"
Write-Host "LD        : $LD"
if ($QEMU) { Write-Host "QEMU      : $QEMU" }
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

# ======================== Build stage ========================

# Select boot source
$BootSrc = if ($Grub) { "boot_grub.s" } else { "boot.s" }
Write-Host ">>> [1/3] Compiling $BootSrc ..." -ForegroundColor Cyan
& $NASM -f elf32 $BootSrc -o boot.o
if ($LASTEXITCODE -ne 0) { throw "NASM compilation failed" }
Write-Host "      OK -> boot.o" -ForegroundColor Green

# Compile C kernel
Write-Host ">>> [2/3] Compiling kernel.c ..." -ForegroundColor Cyan
& $GCC -m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-stack-protector -c kernel.c -o kernel.o
if ($LASTEXITCODE -ne 0) { throw "GCC compilation failed" }
Write-Host "      OK -> kernel.o" -ForegroundColor Green

# Link (ELF with multiboot1 for QEMU -kernel)
Write-Host ">>> [3/3] Linking kernel.elf ..." -ForegroundColor Cyan
& $LD -m elf_i386 -T linker.ld boot.o kernel.o -o kernel.elf -nostdlib
if ($LASTEXITCODE -ne 0) { throw "Linking failed" }
Write-Host "      OK -> kernel.elf" -ForegroundColor Green

Write-Host ""
Write-Host "[Build SUCCESS] kernel.elf generated" -ForegroundColor Green
Write-Host ""

# ======================== Run / Make ISO ========================

if ($Iso) {
    if (-not $GrubMkRescue) {
        Write-Host "[WARNING] grub-mkrescue not found, cannot build ISO." -ForegroundColor Yellow
        Write-Host "        On Windows, building ISO usually requires MSYS2." -ForegroundColor Gray
        Write-Host "        Use -Run to launch ELF directly instead." -ForegroundColor Gray
        exit 1
    }

    Write-Host ">>> Building ISO image ..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path "iso\boot\grub" | Out-Null
    Copy-Item kernel.elf iso\boot\ -Force
    Copy-Item grub.cfg iso\boot\grub\ -Force
    & $GrubMkRescue -o muos.iso iso
    if ($LASTEXITCODE -ne 0) { throw "ISO build failed" }
    Write-Host "      OK -> muos.iso" -ForegroundColor Green
    Write-Host ""
}

if ($Run) {
    if (-not $QEMU) {
        Write-Host "[ERROR] qemu-system-i386 not found, cannot run." -ForegroundColor Red
        Write-Host "        Please install QEMU for Windows and add it to PATH." -ForegroundColor Gray
        exit 1
    }

    Write-Host ">>> Starting QEMU ..." -ForegroundColor Cyan
    Write-Host "      Press Ctrl + Alt + G to release mouse, Ctrl + Alt + 2 for QEMU Monitor" -ForegroundColor Gray
    Write-Host ""

    if ($Iso -and (Test-Path "muos.iso")) {
        & $QEMU -cdrom muos.iso
    } else {
        # multiboot1 ELF 直接加载（默认），无需 ISO/GRUB
        & $QEMU -kernel kernel.elf
    }
}
