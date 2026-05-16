# PhoneToolbox Windows Environment Setup
# Run this script from PowerShell as Administrator to install dependencies

Write-Host "=== PhoneToolbox Windows Setup ===" -ForegroundColor Cyan

# 1. Check prerequisites
$hasGit = Get-Command git -ErrorAction SilentlyContinue
$hasCMake = Get-Command cmake -ErrorAction SilentlyContinue
$hasPython = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $hasPython) { $hasPython = Get-Command python -ErrorAction SilentlyContinue }
$hasVcpkg = $env:VCPKG_ROOT -ne $null

Write-Host "[1/4] Checking prerequisites..." -ForegroundColor Yellow
if (-not $hasGit) { Write-Warning "git not found. Install from https://git-scm.com/" }
if (-not $hasCMake) { Write-Warning "CMake not found. Install from https://cmake.org/" }
if (-not $hasPython) { Write-Warning "Python not found. Install from https://python.org/" }
if (-not $hasVcpkg) {
    Write-Host "  VCPKG_ROOT not set. Installing vcpkg..."
    git clone https://github.com/Microsoft/vcpkg.git "$env:USERPROFILE\vcpkg" --depth 1
    & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
    [Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$env:USERPROFILE\vcpkg", "User")
    $env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
    # Add vcpkg to PATH
    $path = [Environment]::GetEnvironmentVariable("PATH", "User")
    [Environment]::SetEnvironmentVariable("PATH", "$path;$env:USERPROFILE\vcpkg", "User")
    Write-Host "  vcpkg installed at $env:USERPROFILE\vcpkg" -ForegroundColor Green
}

# 2. Install Qt6 via vcpkg
Write-Host "[2/4] Installing Qt6 + libusb (this may take a while)..." -ForegroundColor Yellow
& "$env:VCPKG_ROOT\vcpkg" install qt6 qt6-widgets qt6-network libusb --triplet x64-windows

# 3. Build mtk_bridge.exe
Write-Host "[3/4] Building MTK bridge..." -ForegroundColor Yellow
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Split-Path -Parent $scriptDir

if ($hasPython) {
    $pipCmd = if (Get-Command pip3 -ErrorAction SilentlyContinue) { "pip3" } else { "pip" }
    & $pipCmd install pyinstaller pyusb pycryptodome pycryptodomex pyserial colorama
    # Install mtkclient
    if (Test-Path "$repoDir\mtkclient") {
        & $pipCmd install -e "$repoDir\mtkclient"
    }
    # Build
    & "$scriptDir\build_bridge.ps1"
} else {
    Write-Warning "Python not found. Build mtk_bridge manually: pyinstaller --onefile tools/mtk_bridge.py"
}

# 4. Configure and build
Write-Host "[4/4] Configuring CMake..." -ForegroundColor Yellow
$buildDir = "$repoDir\build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Set-Location $buildDir
cmake .. -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build . --config Release

Write-Host "=== Done ===" -ForegroundColor Cyan
Write-Host "Build: $repoDir\build\Release\PhoneToolbox.exe"
