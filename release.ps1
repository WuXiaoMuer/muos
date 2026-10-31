# release.ps1 - build, smoke-test and package a MuOS release.
#
#   .\release.ps1                  # build ISO + verify + package
#   .\release.ps1 -NoBuild         # package the current artifacts
#   .\release.ps1 -NoVerify        # skip the QEMU smoke test
#
# Output:
#   release\muos-<ver>\            staged tree (sources + muos.iso + scripts)
#   release\muos-<ver>-win32-x86.zip
#
# The version is read from `#define MUOS_VERSION` in src\shell.c, so bumping
# that one line is enough to cut a new release.

param(
    [switch]$NoBuild,
    [switch]$NoVerify
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$BuildDir = "$Root\build"

function Find-Python {
    $cmd = Get-Command "python" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $py = Get-Command "py" -ErrorAction SilentlyContinue
    if ($py) { return $py.Source }
    throw "python not found (needed for muos.iso and verify_release.py)"
}

$ver = "v1.3"
$m = Select-String -Path "$Root\src\shell.c" -Pattern '#define\s+MUOS_VERSION\s+"([^"]+)"' |
     Select-Object -First 1
if ($m) { $ver = $m.Matches[0].Groups[1].Value }

$python = Find-Python
Write-Host ">>> MuOS release $ver" -ForegroundColor Cyan

# ---------------- build ----------------
if (-not $NoBuild) {
    Write-Host ">>> Building kernel + ISO ..." -ForegroundColor Cyan
    & "$Root\build.ps1" -Iso
}

if (-not (Test-Path "$Root\muos.iso")) {
    throw "muos.iso missing; run .\build.ps1 -Iso (or drop -NoBuild)"
}
if (-not (Test-Path "$BuildDir\kernel.elf")) {
    throw "build\kernel.elf missing; run .\build.ps1"
}

# ---------------- smoke test ----------------
if (-not $NoVerify) {
    Write-Host ">>> Smoke test (kernel + ISO) ..." -ForegroundColor Cyan
    & $python "$Root\tools\verify_release.py"
    if ($LASTEXITCODE -ne 0) { throw "verify_release.py FAILED - release aborted" }
} else {
    Write-Host ">>> Smoke test skipped (-NoVerify)" -ForegroundColor Yellow
}

# ---------------- stage ----------------
$Stage = "$Root\release\muos-$ver"
$Zip = "$Root\release\muos-$ver-win32-x86.zip"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

$files = @(
    "muos.iso", "README.md", "ROADMAP.md", "CODE_LOGIC.md", "LICENSE", "NOTICE",
    "build.ps1", "run.ps1", "run.bat", "run_iso.bat", "clean.bat",
    "release.ps1", "release.bat", "setup-tools.ps1", "linker.ld"
)
foreach ($f in $files) {
    $src = Join-Path $Root $f
    if (-not (Test-Path $src)) {
        Write-Warning "  skip missing: $f"
        continue
    }
    Copy-Item $src (Join-Path $Stage $f)
}

foreach ($d in @("src", "include", "user")) {
    Copy-Item (Join-Path $Root $d) (Join-Path $Stage $d) -Recurse
}
New-Item -ItemType Directory -Force -Path "$Stage\tools" | Out-Null
Get-ChildItem "$Root\tools" -File -Filter "*.py" | Copy-Item -Destination "$Stage\tools"

# ---------------- package ----------------
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path "$Stage\*" -DestinationPath $Zip -CompressionLevel Optimal

$sizeKB = [math]::Round((Get-Item $Zip).Length / 1KB)
Write-Host ""
Write-Host "================ Release ready ================" -ForegroundColor Green
Write-Host "  version : $ver"
Write-Host "  folder  : release\muos-$ver"
Write-Host "  archive : release\muos-$ver-win32-x86.zip  ($sizeKB KB)"
Write-Host "  boot    : release\muos-$ver\run.bat   (ISO, VBE + pixel)"
Write-Host "===============================================" -ForegroundColor Green
