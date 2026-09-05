# setup-tools.ps1 - 自动下载并配置 NASM 和 QEMU
# 运行前无需管理员权限，工具会安装到 myos/tools/ 目录下

$ErrorActionPreference = "Stop"
$toolsDir = "$PSScriptRoot\tools"

function Download-File {
    param($Url, $OutFile)
    Write-Host "Downloading: $Url" -ForegroundColor Cyan
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
    $ProgressPreference = 'Continue'
}

function Test-Tool {
    param($Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# ========== NASM ==========
$nasmPath = Test-Tool "nasm"
if (-not $nasmPath) {
    $nasmDir = "$toolsDir\nasm"
    if (-not (Test-Path "$nasmDir\nasm.exe")) {
        Write-Host "[NASM] Not found. Installing..." -ForegroundColor Yellow
        New-Item -ItemType Directory -Force -Path $nasmDir | Out-Null

        $nasmZip = "$nasmDir\nasm.zip"
        Download-File -Url "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip" -OutFile $nasmZip

        Write-Host "[NASM] Extracting..." -ForegroundColor Cyan
        Expand-Archive -Path $nasmZip -DestinationPath $nasmDir -Force
        Remove-Item $nasmZip -Force

        # NASM zip contains a subfolder like nasm-2.16.03
        $sub = Get-ChildItem $nasmDir -Directory | Select-Object -First 1
        if ($sub) {
            Move-Item "$($sub.FullName)\*" $nasmDir -Force
            Remove-Item $sub.FullName -Force
        }
    }
    $nasmPath = "$nasmDir\nasm.exe"
    Write-Host "[NASM] Installed at: $nasmPath" -ForegroundColor Green
} else {
    Write-Host "[NASM] Found at: $nasmPath" -ForegroundColor Green
}

# ========== QEMU ==========
$qemuPath = Test-Tool "qemu-system-i386"
if (-not $qemuPath) {
    $qemuDir = "$toolsDir\qemu"
    if (-not (Test-Path "$qemuDir\qemu-system-i386.exe")) {
        Write-Host "[QEMU] Not found. Installing..." -ForegroundColor Yellow
        New-Item -ItemType Directory -Force -Path $qemuDir | Out-Null

        # QEMU 官方 Windows 64-bit 安装包（可静默安装到指定目录）
        # 先尝试下载 QEMU 便携版 / zip 版本
        $qemuZip = "$qemuDir\qemu.zip"
        try {
            # 使用 QEMU Windows 64-bit 预编译安装包（来自 qemu.weilnetz.de，旧版本会被下架）
            Download-File -Url "https://qemu.weilnetz.de/w64/qemu-w64-setup-20260811.exe" -OutFile "$qemuDir\qemu-setup.exe"
            Write-Host "[QEMU] Downloaded installer. Extracting with 7z..." -ForegroundColor Cyan
            # 如果没有 7z，尝试直接运行静默安装
            Write-Host "[QEMU] Running silent install to $qemuDir ..." -ForegroundColor Cyan
            Start-Process -FilePath "$qemuDir\qemu-setup.exe" -ArgumentList "/S","/D=$qemuDir" -Wait -NoNewWindow
            Remove-Item "$qemuDir\qemu-setup.exe" -Force -ErrorAction SilentlyContinue
        } catch {
            Write-Warning "Failed to download/install QEMU automatically. Please install manually from https://www.qemu.org/download/#windows"
        }
    }
    $qemuPath = "$qemuDir\qemu-system-i386.exe"
    if (Test-Path $qemuPath) {
        Write-Host "[QEMU] Installed at: $qemuPath" -ForegroundColor Green
    } else {
        Write-Warning "[QEMU] Not found after install attempt."
    }
} else {
    Write-Host "[QEMU] Found at: $qemuPath" -ForegroundColor Green
}

# ========== i686-elf-gcc ==========
$gccPath = Test-Tool "i686-elf-gcc"
if (-not $gccPath) {
    $customGcc = "$PSScriptRoot\toolchain\bin\i686-elf-gcc.exe"
    if (Test-Path $customGcc) {
        $gccPath = $customGcc
        Write-Host "[i686-elf-gcc] Found at: $gccPath" -ForegroundColor Green
    } else {
        Write-Warning "[i686-elf-gcc] Not found. Please download from https://github.com/lordmilko/i686-elf-tools/releases and extract to myos/toolchain/"
    }
} else {
    Write-Host "[i686-elf-gcc] Found at: $gccPath" -ForegroundColor Green
}

# ========== Update build.ps1 tool paths ==========
if ($nasmPath -or $qemuPath -or $gccPath) {
    Write-Host "" -ForegroundColor DarkYellow
    Write-Host "To use these tools, you can either:" -ForegroundColor DarkYellow
    Write-Host "  1. Add the following paths to your user PATH environment variable:" -ForegroundColor Gray
    if ($nasmPath) { Write-Host "     $(Split-Path $nasmPath -Parent)" -ForegroundColor Gray }
    if ($qemuPath) { Write-Host "     $(Split-Path $qemuPath -Parent)" -ForegroundColor Gray }
    if ($gccPath)  { Write-Host "     $(Split-Path $gccPath -Parent)" -ForegroundColor Gray }
    Write-Host "  2. Or run the build script with explicit paths (build.ps1 auto-detects tools/ subdir)" -ForegroundColor Gray
    Write-Host "" -ForegroundColor DarkYellow
}

Write-Host "Setup complete!" -ForegroundColor Green
