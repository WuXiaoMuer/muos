# run.ps1 - build (incrementally) and launch MuOS in QEMU.
#
#   .\run.ps1                 # ISO boot: VBE framebuffer + RTL8139 (pixel works)
#   .\run.ps1 -Kernel         # -kernel boot: faster, VGA text mode only
#   .\run.ps1 -Disk           # ISO + an ATA disk image (for MuFS/ATA tests)
#   .\run.ps1 -NoBuild        # launch the existing artifacts
#
# Extra QEMU arguments can be appended after `--` (e.g. .\run.ps1 -- -monitor stdio)

param(
    [switch]$Kernel,
    [switch]$Disk,
    [switch]$NoBuild,
    [int]$Mem = 256,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$QemuArgs
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$BuildDir = "$Root\build"

function Find-Qemu {
    $candidates = @(
        "X:\qemu\qemu-system-i386.exe",
        "C:\qemu\qemu-system-i386.exe",
        "C:\Program Files\qemu\qemu-system-i386.exe",
        "C:\Program Files (x86)\qemu\qemu-system-i386.exe"
    )
    $cmd = Get-Command "qemu-system-i386" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in $candidates) { if (Test-Path $p) { return $p } }
    throw "qemu-system-i386 not found"
}

if (-not $NoBuild) {
    if ($Kernel) { & "$Root\build.ps1" }
    else        { & "$Root\build.ps1" -Iso }
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$Qemu = Find-Qemu
$argsList = @("-m", "$Mem", "-nic", "user,model=rtl8139", "-serial", "stdio")

if ($Kernel) {
    $argsList += @("-kernel", "$BuildDir\kernel.elf")
} else {
    if (-not (Test-Path "$Root\muos.iso")) { throw "muos.iso missing; run .\build.ps1 -Iso" }
    $argsList += @("-cdrom", "$Root\muos.iso")
}

if ($Disk) {
    $img = "$BuildDir\ata_disk.img"
    if (-not (Test-Path $img)) {
        # 8MB raw image; MuFS/ATA tests reuse the same file.
        $fs = [System.IO.File]::Create($img)
        $fs.SetLength(8MB)
        $fs.Close()
    }
    $argsList += @("-drive", "file=$img,format=raw,if=ide")
}

if ($QemuArgs) {
    $QemuArgs = $QemuArgs | Where-Object { $_ -ne "--" }
    $argsList += $QemuArgs
}

Write-Host ">>> $Qemu $($argsList -join ' ')" -ForegroundColor Cyan
& $Qemu @argsList
