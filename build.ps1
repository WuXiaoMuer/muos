# build.ps1 - MuOS Windows local build script (v0.2)
param(
    [switch]$Run,
    [switch]$Iso,
    [switch]$Clean,
    [int]$Jobs = 0,
    [int]$SandboxMB = 8
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
$LD      = Find-Tool "i686-elf-ld"
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
if (-not $LD)      { $missing += "i686-elf-ld" }
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

if ($SandboxMB -lt 8 -or ($SandboxMB % 4) -ne 0) {
    Write-Host "[ERROR] -SandboxMB must be >= 8 and a multiple of 4" -ForegroundColor Red
    exit 1
}
$CFlags = @("-m32", "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra",
             "-DUSER_SANDBOX_MB=$SandboxMB",
             "-fno-exceptions", "-fno-stack-protector", "-nostdinc",
             # Keep GCC from turning our byte loops in string.c into
             # calls to memcpy/memset (self-recursion hazard in a
             # freestanding kernel).
             "-fno-tree-loop-distribute-patterns",
             "-I$PSScriptRoot\include")

$step = 1
$cores = [Environment]::ProcessorCount
if ($Jobs -le 0) { $Jobs = $cores }
$headerStamp = (Get-ChildItem "$PSScriptRoot\include\*.h" | Measure-Object -Property LastWriteTime -Maximum).Maximum

function Test-ObjectFresh($obj, $src, $stamp) {
    if (-not (Test-Path $obj)) { return $false }
    $t = (Get-Item $obj).LastWriteTime
    if ($t -lt (Get-Item $src).LastWriteTime) { return $false }
    if ($stamp -and $t -lt $stamp) { return $false }
    return $true
}

function Test-OutputFresh($out, $inputs) {
    if (-not (Test-Path $out)) { return $false }
    $t = (Get-Item $out).LastWriteTime
    foreach ($i in $inputs) {
        $item = Get-Item $i -ErrorAction SilentlyContinue
        if ($item -and $item.LastWriteTime -gt $t) { return $false }
    }
    return $true
}

# NASM: boot.s
if (Test-ObjectFresh "$BuildDir\boot.o" "$SrcDir\boot.s" $headerStamp) {
    Write-Host ">>> [$step] src\boot.s up to date" -ForegroundColor DarkGray
} else {
    Write-Host ">>> [$step] Compiling src\boot.s ..." -ForegroundColor Cyan
    & $NASM -f elf32 "$SrcDir\boot.s" -o "$BuildDir\boot.o"
    if ($LASTEXITCODE -ne 0) { throw "NASM boot.s failed" }
    Write-Host "      OK -> build\boot.o" -ForegroundColor Green
}
$step++

# NASM: isr_stubs.s
if (Test-ObjectFresh "$BuildDir\isr_stubs.o" "$SrcDir\isr_stubs.s" $headerStamp) {
    Write-Host ">>> [$step] src\isr_stubs.s up to date" -ForegroundColor DarkGray
} else {
    Write-Host ">>> [$step] Compiling src\isr_stubs.s ..." -ForegroundColor Cyan
    & $NASM -f elf32 -w+other -o "$BuildDir\isr_stubs.o" "$SrcDir\isr_stubs.s"
    if ($LASTEXITCODE -ne 0) { throw "NASM isr_stubs.s failed" }
    Write-Host "      OK -> build\isr_stubs.o" -ForegroundColor Green
}
$step++

# GCC: all C files. Up-to-date objects are skipped; the rest are compiled
# in parallel batches ($Jobs processes at a time, default = CPU count).
$CFiles = @(
    "kernel", "vga", "serial", "gdt", "idt", "isr", "pic",
    "pit", "keyboard", "mouse", "mm", "kheap", "string", "fs", "editor",
    "win7", "task", "shell", "irq", "test", "elf", "tss", "user", "syscall",
    "rtc", "module", "proc", "pipe", "ata", "pci", "net", "fb", "gui",
    "part", "fat", "ext2"
)

$ObjFiles = @()
foreach ($name in $CFiles) { $ObjFiles += "$BuildDir\$name.o" }

$todo = @()
foreach ($name in $CFiles) {
    $src = "$SrcDir\$name.c"
    $obj = "$BuildDir\$name.o"
    if (-not (Test-ObjectFresh $obj $src $headerStamp)) {
        $todo += [pscustomobject]@{ Name = $name; Src = $src; Obj = $obj }
    }
}

if ($todo.Count -eq 0) {
    Write-Host ">>> [$step] C sources up to date ($($CFiles.Count) objects)" -ForegroundColor DarkGray
} else {
    Write-Host ">>> [$step] Compiling $($todo.Count)/$($CFiles.Count) C files (jobs=$Jobs) ..." -ForegroundColor Cyan
    for ($i = 0; $i -lt $todo.Count; $i += $Jobs) {
        $last = [Math]::Min($i + $Jobs - 1, $todo.Count - 1)
        $batch = $todo[$i..$last]
        $running = @()
        foreach ($t in $batch) {
            $argsList = $CFlags + @("-c", $t.Src, "-o", $t.Obj)
            $argStr = ($argsList | ForEach-Object {
                if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
            }) -join ' '

            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = $GCC
            $psi.Arguments = $argStr
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow = $true
            $psi.RedirectStandardError = $true
            $psi.RedirectStandardOutput = $true

            $proc = New-Object System.Diagnostics.Process
            $proc.StartInfo = $psi
            [void]$proc.Start()
            $errTask = $proc.StandardError.ReadToEndAsync()
            $outTask = $proc.StandardOutput.ReadToEndAsync()
            $running += [pscustomobject]@{
                Name = $t.Name; Proc = $proc; Err = $errTask; Out = $outTask
            }
        }
        foreach ($r in $running) {
            $r.Proc.WaitForExit()
            $errText = $r.Err.Result
            $outText = $r.Out.Result
            if ($r.Proc.ExitCode -ne 0) {
                if ($errText) { Write-Host $errText }
                if ($outText) { Write-Host $outText }
                throw "GCC $($r.Name).c failed"
            }
            Write-Host "      OK -> build\$($r.Name).o" -ForegroundColor Green
        }
    }
}
$step++

# ======================== Ring3 user program ========================
# The user ELF is built with the same cross compiler but linked at
# 0x08048000 against the Linux-style stack/syscall ABI, then embedded in
# the kernel image as a raw blob (symbols _binary_hello_elf_*).
$UserDir    = "$PSScriptRoot\user"
$UserCFlags = @("-m32", "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra",
                "-fno-stack-protector", "-fno-pic", "-fno-pie",
                "-fno-tree-loop-distribute-patterns", "-nostdinc", "-nostdlib")
$userStamp = (Get-ChildItem "$UserDir\*.h", "$PSScriptRoot\include\*.h" |
              Measure-Object -Property LastWriteTime -Maximum).Maximum

Write-Host ">>> [$step] Compiling user runtime + hello.c ..." -ForegroundColor Cyan
if (Test-ObjectFresh "$BuildDir\user_runtime.o" "$UserDir\runtime.c" $userStamp) {
    Write-Host "      user_runtime.o up to date" -ForegroundColor DarkGray
} else {
    & $GCC @UserCFlags -c "$UserDir\runtime.c" -o "$BuildDir\user_runtime.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC user runtime.c failed" }
}
if (Test-ObjectFresh "$BuildDir\user_hello.o" "$UserDir\hello.c" $userStamp) {
    Write-Host "      user_hello.o up to date" -ForegroundColor DarkGray
} else {
    & $GCC @UserCFlags -c "$UserDir\hello.c" -o "$BuildDir\user_hello.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC user hello.c failed" }
}
if (Test-ObjectFresh "$BuildDir\user_syscall6.o" "$UserDir\syscall6.s" $userStamp) {
    Write-Host "      user_syscall6.o up to date" -ForegroundColor DarkGray
} else {
    & $NASM -f elf32 "$UserDir\syscall6.s" -o "$BuildDir\user_syscall6.o"
    if ($LASTEXITCODE -ne 0) { throw "NASM user syscall6.s failed" }
}
Write-Host "      OK -> user objects ready" -ForegroundColor Green
$step++

Write-Host ">>> [$step] Linking hello.elf (0x08048000) ..." -ForegroundColor Cyan
if (Test-ObjectFresh "$BuildDir\user_start.o" "$UserDir\start.s" $userStamp) {
    Write-Host "      user_start.o up to date" -ForegroundColor DarkGray
} else {
    & $NASM -f elf32 "$UserDir\start.s" -o "$BuildDir\user_start.o"
    if ($LASTEXITCODE -ne 0) { throw "NASM user start.s failed" }
}
if (Test-OutputFresh "$BuildDir\hello.elf" @(
        "$BuildDir\user_start.o", "$BuildDir\user_runtime.o",
        "$BuildDir\user_hello.o", "$BuildDir\user_syscall6.o",
        "$UserDir\linker.ld")) {
    Write-Host "      hello.elf up to date" -ForegroundColor DarkGray
} else {
    & $GCC -m32 -nostdlib -no-pie -T "$UserDir\linker.ld" `
           "$BuildDir\user_start.o" "$BuildDir\user_runtime.o" "$BuildDir\user_hello.o" `
           "$BuildDir\user_syscall6.o" `
           -o "$BuildDir\hello.elf" "-Wl,--build-id=none" "-Wl,--no-warn-rwx-segments"
    if ($LASTEXITCODE -ne 0) { throw "Link hello.elf failed" }
    Write-Host "      OK -> build\hello.elf" -ForegroundColor Green
}
$step++

Write-Host ">>> [$step] Embedding hello.elf as a kernel blob ..." -ForegroundColor Cyan
if (Test-OutputFresh "$BuildDir\hello_elf.o" @("$BuildDir\hello.elf")) {
    Write-Host "      hello_elf.o up to date" -ForegroundColor DarkGray
} else {
    Push-Location $BuildDir
    & $OBJCOPY -I binary -O elf32-i386 -B i386 "hello.elf" "hello_elf.o"
    $blobRc = $LASTEXITCODE
    Pop-Location
    if ($blobRc -ne 0) { throw "objcopy hello.elf failed" }
    Write-Host "      OK -> build\hello_elf.o" -ForegroundColor Green
}
$step++

# ======================== Position-independent (ET_DYN) user image =======
# Same sources built with -fPIE and linked at 0. The kernel loader applies
# the bias + .rel.dyn relocations (see src/elf.c, `run pie`).
Write-Host ">>> [$step] Building hello-pie.elf (ET_DYN) ..." -ForegroundColor Cyan
$PieCFlags = @("-m32", "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra",
               "-fno-stack-protector", "-fPIE",
               "-fno-tree-loop-distribute-patterns", "-nostdinc", "-nostdlib")
if (-not (Test-ObjectFresh "$BuildDir\pie_runtime.o" "$UserDir\runtime.c" $userStamp)) {
    & $GCC @PieCFlags -c "$UserDir\runtime.c" -o "$BuildDir\pie_runtime.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC pie runtime.c failed" }
}
if (-not (Test-ObjectFresh "$BuildDir\pie_hello.o" "$UserDir\hello.c" $userStamp)) {
    & $GCC @PieCFlags -c "$UserDir\hello.c" -o "$BuildDir\pie_hello.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC pie hello.c failed" }
}
if (-not (Test-ObjectFresh "$BuildDir\pie_start.o" "$UserDir\start.s" $userStamp)) {
    & $NASM -f elf32 "$UserDir\start.s" -o "$BuildDir\pie_start.o"
    if ($LASTEXITCODE -ne 0) { throw "NASM pie start.s failed" }
}
if (-not (Test-ObjectFresh "$BuildDir\pie_syscall6.o" "$UserDir\syscall6.s" $userStamp)) {
    & $NASM -f elf32 "$UserDir\syscall6.s" -o "$BuildDir\pie_syscall6.o"
    if ($LASTEXITCODE -ne 0) { throw "NASM pie syscall6.s failed" }
}
$pieInputs = @("$BuildDir\pie_start.o", "$BuildDir\pie_runtime.o",
               "$BuildDir\pie_hello.o", "$BuildDir\pie_syscall6.o",
               "$UserDir\linker_pie.ld")
if (Test-OutputFresh "$BuildDir\hello-pie.elf" $pieInputs) {
    Write-Host "      hello-pie.elf up to date" -ForegroundColor DarkGray
} else {
    & $GCC -m32 -nostdlib -pie -T "$UserDir\linker_pie.ld" `
           "$BuildDir\pie_start.o" "$BuildDir\pie_runtime.o" "$BuildDir\pie_hello.o" `
           "$BuildDir\pie_syscall6.o" `
           -o "$BuildDir\hello-pie.elf" "-Wl,--build-id=none" "-Wl,--no-warn-rwx-segments"
    if ($LASTEXITCODE -ne 0) { throw "Link hello-pie.elf failed" }
    Write-Host "      OK -> build\hello-pie.elf" -ForegroundColor Green
}
if (Test-OutputFresh "$BuildDir\hello_pie_elf.o" @("$BuildDir\hello-pie.elf")) {
    Write-Host "      hello_pie_elf.o up to date" -ForegroundColor DarkGray
} else {
    Push-Location $BuildDir
    & $OBJCOPY -I binary -O elf32-i386 -B i386 "hello-pie.elf" "hello_pie_elf.o"
    $pieRc = $LASTEXITCODE
    Pop-Location
    if ($pieRc -ne 0) { throw "objcopy hello-pie.elf failed" }
    Write-Host "      OK -> build\hello_pie_elf.o" -ForegroundColor Green
}
$step++

# ======================== Dynamic linking set ========================
# ld-mu.so   : user-mode dynamic linker (ET_DYN, no dependencies)
# libmu.so   : first shared library (ET_DYN, SysV hash + dynsym)
# dyn.elf    : PIE program with PT_INTERP=/lib/ld-mu.so, DT_NEEDED libmu.so
$SoCFlags = @("-m32", "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra",
              "-fno-stack-protector", "-fPIC", "-fno-builtin",
              "-fno-tree-loop-distribute-patterns", "-nostdinc", "-nostdlib")

Write-Host ">>> [$step] Building libmu.so (shared library) ..." -ForegroundColor Cyan
if (-not (Test-ObjectFresh "$BuildDir\libmu_pic.o" "$UserDir\libmu.c" $userStamp)) {
    & $GCC @SoCFlags -c "$UserDir\libmu.c" -o "$BuildDir\libmu_pic.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC libmu.c failed" }
}
if (-not (Test-OutputFresh "$BuildDir\libmu.so" @("$BuildDir\libmu_pic.o",
        "$UserDir\linker_so.ld"))) {
    & $LD -m elf_i386 -shared -T "$UserDir\linker_so.ld" `
          "$BuildDir\libmu_pic.o" -o "$BuildDir\libmu.so" `
          --hash-style=gnu --build-id=none --no-warn-rwx-segments
    if ($LASTEXITCODE -ne 0) { throw "Link libmu.so failed" }
    Write-Host "      OK -> build\libmu.so" -ForegroundColor Green
} else {
    Write-Host "      libmu.so up to date" -ForegroundColor DarkGray
}
$step++

Write-Host ">>> [$step] Building ld-mu.so (dynamic linker) ..." -ForegroundColor Cyan
if (-not (Test-ObjectFresh "$BuildDir\ldmu_pic.o" "$UserDir\ldmu.c" $userStamp)) {
    & $GCC @SoCFlags -c "$UserDir\ldmu.c" -o "$BuildDir\ldmu_pic.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC ldmu.c failed" }
}
if (-not (Test-ObjectFresh "$BuildDir\ld_start.o" "$UserDir\ld_start.s" $userStamp)) {
    & $NASM -f elf32 "$UserDir\ld_start.s" -o "$BuildDir\ld_start.o"
    if ($LASTEXITCODE -ne 0) { throw "NASM ld_start.s failed" }
}
if (-not (Test-OutputFresh "$BuildDir\ld-mu.so" @("$BuildDir\ldmu_pic.o",
        "$BuildDir\ld_start.o", "$UserDir\linker_so.ld"))) {
    & $LD -m elf_i386 -shared -T "$UserDir\linker_so.ld" `
          "$BuildDir\ldmu_pic.o" "$BuildDir\ld_start.o" -o "$BuildDir\ld-mu.so" `
          --hash-style=sysv --build-id=none --no-warn-rwx-segments
    if ($LASTEXITCODE -ne 0) { throw "Link ld-mu.so failed" }
    Write-Host "      OK -> build\ld-mu.so" -ForegroundColor Green
} else {
    Write-Host "      ld-mu.so up to date" -ForegroundColor DarkGray
}
$step++

Write-Host ">>> [$step] Building dyn.elf (PT_INTERP + DT_NEEDED libmu.so) ..." -ForegroundColor Cyan
if (-not (Test-ObjectFresh "$BuildDir\pie_dyn.o" "$UserDir\dyn.c" $userStamp)) {
    & $GCC @PieCFlags -c "$UserDir\dyn.c" -o "$BuildDir\pie_dyn.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC dyn.c failed" }
}
if (-not (Test-OutputFresh "$BuildDir\dyn.elf" @(
        "$BuildDir\pie_start.o", "$BuildDir\pie_runtime.o",
        "$BuildDir\pie_dyn.o", "$BuildDir\pie_syscall6.o",
        "$BuildDir\libmu.so", "$UserDir\linker_dyn.ld"))) {
    & $GCC -m32 -nostdlib -pie -T "$UserDir\linker_dyn.ld" `
           "$BuildDir\pie_start.o" "$BuildDir\pie_runtime.o" "$BuildDir\pie_dyn.o" `
           "$BuildDir\pie_syscall6.o" `
           -L"$BuildDir" -lmu `
           -o "$BuildDir\dyn.elf" `
           "-Wl,--dynamic-linker=/lib/ld-mu.so" "-Wl,--hash-style=sysv" `
           "-Wl,--build-id=none" "-Wl,--no-warn-rwx-segments"
    if ($LASTEXITCODE -ne 0) { throw "Link dyn.elf failed" }
    Write-Host "      OK -> build\dyn.elf" -ForegroundColor Green
} else {
    Write-Host "      dyn.elf up to date" -ForegroundColor DarkGray
}
$step++

# TLS set: libtls.so (initial-exec + global-dynamic TLS) and tls.elf.
Write-Host ">>> [$step] Building libtls.so (TLS shared library) ..." -ForegroundColor Cyan
if (-not (Test-ObjectFresh "$BuildDir\tlslib_pic.o" "$UserDir\tlslib.c" $userStamp)) {
    & $GCC @SoCFlags -c "$UserDir\tlslib.c" -o "$BuildDir\tlslib_pic.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC tlslib.c failed" }
}
if (-not (Test-OutputFresh "$BuildDir\libtls.so" @("$BuildDir\tlslib_pic.o",
        "$UserDir\linker_so.ld"))) {
    & $LD -m elf_i386 -shared -T "$UserDir\linker_so.ld" `
          "$BuildDir\tlslib_pic.o" -o "$BuildDir\libtls.so" `
          --hash-style=gnu --build-id=none --no-warn-rwx-segments
    if ($LASTEXITCODE -ne 0) { throw "Link libtls.so failed" }
    Write-Host "      OK -> build\libtls.so" -ForegroundColor Green
} else {
    Write-Host "      libtls.so up to date" -ForegroundColor DarkGray
}
$step++

Write-Host ">>> [$step] Building tls.elf (main TLS + DT_NEEDED libtls.so) ..." -ForegroundColor Cyan
if (-not (Test-ObjectFresh "$BuildDir\pie_tls.o" "$UserDir\tls.c" $userStamp)) {
    & $GCC @PieCFlags -c "$UserDir\tls.c" -o "$BuildDir\pie_tls.o"
    if ($LASTEXITCODE -ne 0) { throw "GCC tls.c failed" }
}
if (-not (Test-OutputFresh "$BuildDir\tls.elf" @(
        "$BuildDir\pie_start.o", "$BuildDir\pie_runtime.o",
        "$BuildDir\pie_tls.o", "$BuildDir\pie_syscall6.o",
        "$BuildDir\libtls.so", "$UserDir\linker_dyn.ld"))) {
    & $GCC -m32 -nostdlib -pie -T "$UserDir\linker_dyn.ld" `
           "$BuildDir\pie_start.o" "$BuildDir\pie_runtime.o" "$BuildDir\pie_tls.o" `
           "$BuildDir\pie_syscall6.o" `
           -L"$BuildDir" -ltls `
           -o "$BuildDir\tls.elf" `
           "-Wl,--dynamic-linker=/lib/ld-mu.so" "-Wl,--hash-style=sysv" `
           "-Wl,--allow-shlib-undefined" `
           "-Wl,--build-id=none" "-Wl,--no-warn-rwx-segments"
    if ($LASTEXITCODE -ne 0) { throw "Link tls.elf failed" }
    Write-Host "      OK -> build\tls.elf" -ForegroundColor Green
} else {
    Write-Host "      tls.elf up to date" -ForegroundColor DarkGray
}
$step++

Write-Host ">>> [$step] Embedding dynamic-linking blobs ..." -ForegroundColor Cyan
Push-Location $BuildDir
if (-not (Test-OutputFresh "ld_mu_so.o" @("ld-mu.so"))) {
    & $OBJCOPY -I binary -O elf32-i386 -B i386 "ld-mu.so" "ld_mu_so.o"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "objcopy ld-mu.so failed" }
}
if (-not (Test-OutputFresh "libmu_so.o" @("libmu.so"))) {
    & $OBJCOPY -I binary -O elf32-i386 -B i386 "libmu.so" "libmu_so.o"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "objcopy libmu.so failed" }
}
if (-not (Test-OutputFresh "dyn_elf.o" @("dyn.elf"))) {
    & $OBJCOPY -I binary -O elf32-i386 -B i386 "dyn.elf" "dyn_elf.o"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "objcopy dyn.elf failed" }
}
if (-not (Test-OutputFresh "libtls_so.o" @("libtls.so"))) {
    & $OBJCOPY -I binary -O elf32-i386 -B i386 "libtls.so" "libtls_so.o"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "objcopy libtls.so failed" }
}
if (-not (Test-OutputFresh "tls_elf.o" @("tls.elf"))) {
    & $OBJCOPY -I binary -O elf32-i386 -B i386 "tls.elf" "tls_elf.o"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "objcopy tls.elf failed" }
}
Pop-Location
Write-Host "      OK -> dynamic blobs ready" -ForegroundColor Green
$step++

# Link (via GCC driver so standard flags are handled consistently)
Write-Host ">>> [$step] Linking kernel.elf ..." -ForegroundColor Cyan
$AllObj = @("$BuildDir\boot.o", "$BuildDir\isr_stubs.o",
            "$BuildDir\hello_elf.o", "$BuildDir\hello_pie_elf.o",
            "$BuildDir\ld_mu_so.o", "$BuildDir\libmu_so.o",
            "$BuildDir\dyn_elf.o", "$BuildDir\libtls_so.o",
            "$BuildDir\tls_elf.o") + $ObjFiles
$LdArgs = @("-nostdlib", "-T", "$PSScriptRoot\linker.ld",
            "-Wl,--no-warn-rwx-segments") + $AllObj + @("-o", "$BuildDir\kernel.elf")
if (Test-OutputFresh "$BuildDir\kernel.elf" ($AllObj + @("$PSScriptRoot\linker.ld"))) {
    Write-Host "      kernel.elf up to date" -ForegroundColor DarkGray
} else {
    & $GCC @LdArgs
    if ($LASTEXITCODE -ne 0) { throw "Link failed" }
    Write-Host "      OK -> build\kernel.elf" -ForegroundColor Green
}

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
    if (Test-OutputFresh "$PSScriptRoot\muos.iso" @(
            "$BuildDir\kernel.elf", "$SrcDir\boot_cd.s",
            "$PSScriptRoot\tools\make_iso.py")) {
        Write-Host "      muos.iso up to date" -ForegroundColor DarkGray
    } else {
        & $python "$PSScriptRoot\tools\make_iso.py"
        if ($LASTEXITCODE -ne 0) { throw "ISO build failed" }
        Write-Host "      OK -> muos.iso" -ForegroundColor Green
    }
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
        & $QEMU -cdrom "$PSScriptRoot\muos.iso" -m 256M -nic user,model=rtl8139
    } else {
        & $QEMU -kernel "$BuildDir\kernel.elf" -m 256M -nic user,model=rtl8139
    }
}
