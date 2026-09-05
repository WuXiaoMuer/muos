# build.ps1 - MuOS Windows local build script (v0.2)
param(
    [switch]$Run,
    [switch]$Iso,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$BuildDir = "$PSScriptRoot\build"
$SrcDir   = "$PSScriptRoot\src"

# ======================== Tool search ========================

function Find-Tool {
    param([string]$Name, [string[]]$ExtraPaths)
    $inPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($inPath) { return $inPath.Source }
    $commonPaths = @(
        "C:\Program Files\NASM\$Name.exe",
        "C:\Program Files (x86)\NASM\$Name.exe",
        "C:\qemu\$Name.exe",
        "C:\Program Files\qemu\$Name.exe",
        "C:\Program Files (x86)\qemu\$Name.exe",
        "$PSScriptRoot\toolchain\bin\$Name.exe",
        "$PSScriptRoot\tools\nasm\$Name.exe",
        "$PSScriptRoot\tools\qemu\$Name.exe"
    ) + $ExtraPaths
    foreach ($p in $commonPaths) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

$NASM = Find-Tool "nasm"
$GCC  = Find-Tool "i686-elf-gcc"
$QEMU = Find-Tool "qemu-system-i386"

# ======================== Clean ========================
if ($Clean) {
    Write-Host ">>> Cleaning build artifacts..." -ForegroundColor Cyan
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
        Write-Host "  Deleted: build/"
    }
    Remove-Item -Force "$PSScriptRoot\muos.iso" -ErrorAction SilentlyContinue
    Write-Host ">>> Clean complete" -ForegroundColor Green
    exit 0
}

# ======================== Check tools ========================
$missing = @()
if (-not $NASM) { $missing += "nasm" }
if (-not $GCC)  { $missing += "i686-elf-gcc" }

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "[ERROR] Missing tools:" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host "  - $m" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "  NASM : https://www.nasm.us/"
    Write-Host "  GCC  : https://github.com/lordmilko/i686-elf-tools/releases"
    Write-Host "  QEMU : https://www.qemu.org/download/#windows"
    Write-Host ""
    exit 1
}

Write-Host ""
Write-Host "======================== MuOS Build ========================" -ForegroundColor Cyan
Write-Host "NASM : $NASM"
Write-Host "GCC  : $GCC"
if ($QEMU) { Write-Host "QEMU : $QEMU" }
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

# ======================== Build ========================
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$CFlags = @("-m32", "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra",
             "-fno-exceptions", "-fno-stack-protector", "-nostdinc",
             # Keep GCC from turning our byte loops in string.c into
             # calls to memcpy/memset (self-recursion hazard in a
             # freestanding kernel).
             "-fno-tree-loop-distribute-patterns",
             "-I$PSScriptRoot\include")

$step = 1

# NASM: boot.s
Write-Host ">>> [$step] Compiling src\boot.s ..." -ForegroundColor Cyan
& $NASM -f elf32 "$SrcDir\boot.s" -o "$BuildDir\boot.o"
if ($LASTEXITCODE -ne 0) { throw "NASM boot.s failed" }
Write-Host "      OK -> build\boot.o" -ForegroundColor Green
$step++

# NASM: isr_stubs.s
Write-Host ">>> [$step] Compiling src\isr_stubs.s ..." -ForegroundColor Cyan
& $NASM -f elf32 -w+other -o "$BuildDir\isr_stubs.o" "$SrcDir\isr_stubs.s"
if ($LASTEXITCODE -ne 0) { throw "NASM isr_stubs.s failed" }
Write-Host "      OK -> build\isr_stubs.o" -ForegroundColor Green
$step++

# GCC: all C files
$CFiles = @(
    "kernel", "vga", "vgagfx", "serial", "gdt", "idt", "isr", "pic",
    "pit", "keyboard", "mouse", "mm", "kheap", "string", "fs", "editor",
    "win7", "task", "shell", "irq", "test"
)

$ObjFiles = @()
foreach ($name in $CFiles) {
    $src = "$SrcDir\$name.c"
    $obj = "$BuildDir\$name.o"
    Write-Host ">>> [$step] Compiling src\$name.c ..." -ForegroundColor Cyan
    & $GCC @CFlags -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "GCC $name.c failed" }
    Write-Host "      OK -> build\$name.o" -ForegroundColor Green
    $ObjFiles += $obj
    $step++
}

# Link (via GCC driver so standard flags are handled consistently)
Write-Host ">>> [$step] Linking kernel.elf ..." -ForegroundColor Cyan
$AllObj = @("$BuildDir\boot.o", "$BuildDir\isr_stubs.o") + $ObjFiles
$LdArgs = @("-nostdlib", "-T", "$PSScriptRoot\linker.ld",
            "-Wl,--no-warn-rwx-segments") + $AllObj + @("-o", "$BuildDir\kernel.elf")
& $GCC @LdArgs
if ($LASTEXITCODE -ne 0) { throw "Link failed" }
Write-Host "      OK -> build\kernel.elf" -ForegroundColor Green

Write-Host ""
Write-Host "[Build SUCCESS] build\kernel.elf generated" -ForegroundColor Green
Write-Host ""

# ======================== ISO / Run ========================
if ($Iso) {
    Write-Host ">>> [ISO] Building bootable ISO via Python ..." -ForegroundColor Cyan
    $python = Find-Tool "python"
    if (-not $python) {
        Write-Host "[ERROR] python not found in PATH." -ForegroundColor Red
        exit 1
    }
    & $python "$PSScriptRoot\make_iso.py"
    if ($LASTEXITCODE -ne 0) { throw "ISO build failed" }
    Write-Host "      OK -> muos.iso" -ForegroundColor Green
    Write-Host ""
}

if ($Run) {
    if (-not $QEMU) {
        Write-Host "[ERROR] qemu-system-i386 not found." -ForegroundColor Red
        exit 1
    }
    Write-Host ">>> Starting QEMU ..." -ForegroundColor Cyan
    Write-Host "      Ctrl+Alt+G = release mouse" -ForegroundColor Gray
    Write-Host ""
    if ($Iso -and (Test-Path "$PSScriptRoot\muos.iso")) {
        & $QEMU -cdrom "$PSScriptRoot\muos.iso" -m 256M
    } else {
        & $QEMU -kernel "$BuildDir\kernel.elf" -m 256M
    }
}
