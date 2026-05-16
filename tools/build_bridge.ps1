# Build mtk_bridge.exe on Windows with PyInstaller
# Usage: .\tools\build_bridge.ps1

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Split-Path -Parent $scriptDir

$pipCmd = if (Get-Command pip3 -ErrorAction SilentlyContinue) { "pip3" } else { "pip" }

Write-Host "=== Building mtk_bridge for Windows ===" -ForegroundColor Cyan

# Ensure dependencies
& $pipCmd install -q pyinstaller pyusb pycryptodome pycryptodomex pyserial colorama

# Install mtkclient if present
if (Test-Path "$repoDir\mtkclient") {
    & $pipCmd install -q -e "$repoDir\mtkclient"
}

$outDir = "$repoDir\third_party\mtk_bridge\windows\x64"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$buildDir = "$repoDir\build\mtk_bridge"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

& pyinstaller --onefile `
    --distpath "$buildDir\dist" `
    --workpath "$buildDir\work" `
    --specpath "$buildDir" `
    --name mtk_bridge.exe `
    --exclude-module PySide6 `
    --exclude-module PySide6_Essentials `
    --exclude-module PySide6_Addons `
    --exclude-module shiboken6 `
    --exclude-module fusepy `
    --exclude-module matplotlib `
    --exclude-module PIL `
    --exclude-module cv2 `
    --exclude-module tkinter `
    --exclude-module unittest `
    --exclude-module keystone `
    --exclude-module capstone `
    --exclude-module unicorn `
    "$repoDir\tools\mtk_bridge.py"

Copy-Item "$buildDir\dist\mtk_bridge.exe" -Destination "$outDir\mtk_bridge.exe" -Force

Write-Host "=== Done: $(Get-Item "$outDir\mtk_bridge.exe" | Select-Object -ExpandProperty Length) bytes ===" -ForegroundColor Green
