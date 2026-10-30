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
        "X:\qemu\$Name.exe",
        "$PSScriptRoot\toolchain\bin\$Name.exe",
        "$PSScriptRoot\tools\nasm\$Name.exe",
        "$PSScriptRoot\tools\qemu\$Name.exe"
    ) + $ExtraPaths
    foreach ($p in $commonPaths) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

$NASM    = Find-Tool "nasm"
$GCC     = Find-Tool "i686-elf-gcc"
$OBJCOPY = Find-Tool "i686-elf-objcopy"
$QEMU    = Find-Tool "qemu-system-i386"

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
if (-not $NASM)    { $missing += "nasm" }
if (-not $GCC)     { $missing += "i686-elf-gcc" }
if (-not $OBJCOPY) { $missing += "i686-elf-objcopy" }

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
    "kernel", "vga", "serial", "gdt", "idt", "isr", "pic",
    "pit", "keyboard", "mouse", "mm", "kheap", "string", "fs", "editor",
    "win7", "task", "shell", "irq", "test", "elf", "tss", "user", "syscall",
    "rtc", "module", "proc", "pipe"
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

# ======================== Ring3 user program ========================
# The user ELF is built with the same cross compiler but linked at
# 0x08048000 against the Linux-style stack/syscall ABI, then embedded in
# the kernel image as a raw blob (symbols _binary_hello_elf_*).
$UserDir    = "$PSScriptRoot\user"
$UserCFlags = @("-m32", "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra",
                "-fno-stack-protector", "-fno-pic", "-fno-pie",
                "-fno-tree-loop-distribute-patterns", "-nostdinc", "-nostdlib")

Write-Host ">>> [$step] Compiling user runtime + hello.c ..." -ForegroundColor Cyan
& $GCC @UserCFlags -c "$UserDir\runtime.c" -o "$BuildDir\user_runtime.o"
if ($LASTEXITCODE -ne 0) { throw "GCC user runtime.c failed" }
& $GCC @UserCFlags -c "$UserDir\hello.c" -o "$BuildDir\user_hello.o"
if ($LASTEXITCODE -ne 0) { throw "GCC user hello.c failed" }
& $NASM -f elf32 "$UserDir\syscall6.s" -o "$BuildDir\user_syscall6.o"
if ($LASTEXITCODE -ne 0) { throw "NASM user syscall6.s failed" }
Write-Host "      OK -> build\user_runtime.o, build\user_hello.o, build\user_syscall6.o" -ForegroundColor Green
$step++

Write-Host ">>> [$step] Linking hello.elf (0x08048000) ..." -ForegroundColor Cyan
& $NASM -f elf32 "$UserDir\start.s" -o "$BuildDir\user_start.o"
if ($LASTEXITCODE -ne 0) { throw "NASM user start.s failed" }
& $GCC -m32 -nostdlib -no-pie -T "$UserDir\linker.ld" `
       "$BuildDir\user_start.o" "$BuildDir\user_runtime.o" "$BuildDir\user_hello.o" `
       "$BuildDir\user_syscall6.o" `
       -o "$BuildDir\hello.elf" "-Wl,--build-id=none" "-Wl,--no-warn-rwx-segments"
if ($LASTEXITCODE -ne 0) { throw "Link hello.elf failed" }
Write-Host "      OK -> build\hello.elf" -ForegroundColor Green
$step++

Write-Host ">>> [$step] Embedding hello.elf as a kernel blob ..." -ForegroundColor Cyan
Push-Location $BuildDir
& $OBJCOPY -I binary -O elf32-i386 -B i386 "hello.elf" "hello_elf.o"
$blobRc = $LASTEXITCODE
Pop-Location
if ($blobRc -ne 0) { throw "objcopy hello.elf failed" }
Write-Host "      OK -> build\hello_elf.o" -ForegroundColor Green
$step++

# ======================== Position-independent (ET_DYN) user image =======
# Same sources built with -fPIE and linked at 0. The kernel loader applies
# the bias + .rel.dyn relocations (see src/elf.c, `run pie`).
Write-Host ">>> [$step] Building hello-pie.elf (ET_DYN) ..." -ForegroundColor Cyan
$PieCFlags = @("-m32", "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra",
               "-fno-stack-protector", "-fPIE",
               "-fno-tree-loop-distribute-patterns", "-nostdinc", "-nostdlib")
& $GCC @PieCFlags -c "$UserDir\runtime.c" -o "$BuildDir\pie_runtime.o"
if ($LASTEXITCODE -ne 0) { throw "GCC pie runtime.c failed" }
& $GCC @PieCFlags -c "$UserDir\hello.c" -o "$BuildDir\pie_hello.o"
if ($LASTEXITCODE -ne 0) { throw "GCC pie hello.c failed" }
& $NASM -f elf32 "$UserDir\start.s" -o "$BuildDir\pie_start.o"
if ($LASTEXITCODE -ne 0) { throw "NASM pie start.s failed" }
& $NASM -f elf32 "$UserDir\syscall6.s" -o "$BuildDir\pie_syscall6.o"
if ($LASTEXITCODE -ne 0) { throw "NASM pie syscall6.s failed" }
& $GCC -m32 -nostdlib -pie -T "$UserDir\linker_pie.ld" `
       "$BuildDir\pie_start.o" "$BuildDir\pie_runtime.o" "$BuildDir\pie_hello.o" `
       "$BuildDir\pie_syscall6.o" `
       -o "$BuildDir\hello-pie.elf" "-Wl,--build-id=none" "-Wl,--no-warn-rwx-segments"
if ($LASTEXITCODE -ne 0) { throw "Link hello-pie.elf failed" }
Push-Location $BuildDir
& $OBJCOPY -I binary -O elf32-i386 -B i386 "hello-pie.elf" "hello_pie_elf.o"
$pieRc = $LASTEXITCODE
Pop-Location
if ($pieRc -ne 0) { throw "objcopy hello-pie.elf failed" }
Write-Host "      OK -> build\hello-pie.elf, build\hello_pie_elf.o" -ForegroundColor Green
$step++

# Link (via GCC driver so standard flags are handled consistently)
Write-Host ">>> [$step] Linking kernel.elf ..." -ForegroundColor Cyan
$AllObj = @("$BuildDir\boot.o", "$BuildDir\isr_stubs.o",
            "$BuildDir\hello_elf.o", "$BuildDir\hello_pie_elf.o") + $ObjFiles
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
