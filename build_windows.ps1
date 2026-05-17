# PhoneToolbox Windows 构建脚本
# 用法:
#   .\build_windows.ps1              # 动态构建 (需要 DLL)
#   .\build_windows.ps1 -Static      # 静态构建 (单 EXE)

param(
    [switch]$Static = $false,
    [switch]$Clean = $false,
    [string]$VcpkgDir = ""
)

$ErrorActionPreference = "Stop"

# ── 1. 检测 vcpkg ──
if (-not $VcpkgDir) {
    $candidates = @(
        "$env:VCPKG_ROOT",
        "$env:LOCALAPPDATA\vcpkg",
        "C:\dev\vcpkg",
        "C:\tools\vcpkg",
        "$HOME\vcpkg"
    )
    foreach ($d in $candidates) {
        if ($d -and (Test-Path "$d\vcpkg.exe")) {
            $VcpkgDir = $d
            break
        }
    }
}

if (-not $VcpkgDir) {
    Write-Host "[!] vcpkg not found. Install it first:" -ForegroundColor Red
    Write-Host "    git clone https://github.com/Microsoft/vcpkg.git"
    Write-Host "    cd vcpkg && bootstrap-vcpkg.bat"
    exit 1
}

Write-Host "[+] Using vcpkg: $VcpkgDir" -ForegroundColor Green

# ── 2. 设置 triplet ──
if ($Static) {
    $Triplet = "x64-windows-static"
    $BuildDir = "build-static"
    Write-Host "[+] Mode: STATIC (single EXE)" -ForegroundColor Cyan
} else {
    $Triplet = "x64-windows"
    $BuildDir = "build"
    Write-Host "[+] Mode: DYNAMIC (needs DLLs)" -ForegroundColor Cyan
}

# ── 3. 安装依赖 ──
$packages = @(
    "qt6-base[$Triplet]",
    "qt6-tools[$Triplet]",
    "libusb[$Triplet]"
)

Write-Host "[+] Installing dependencies (this may take a while)..." -ForegroundColor Yellow
Push-Location $VcpkgDir
try {
    foreach ($pkg in $packages) {
        $name = $pkg -replace '\[.*\]'
        Write-Host "    Installing $name for $Triplet..."
        & .\vcpkg install $pkg
        if ($LASTEXITCODE -ne 0) {
            # Qt6 takes very long to build from source; suggest pre-built binaries
            if ($name -like "qt6*") {
                Write-Host "[!] Qt6 build from source is slow. Consider using binary caching:" -ForegroundColor Yellow
                Write-Host "    set VCPKG_BINARY_SOURCES=clear;nuget,https://nuget.pkg.github.com/OWNER/index.json,readwrite"
            }
            throw "Failed to install $pkg"
        }
    }
} finally {
    Pop-Location
}

# ── 4. 清理（可选）──
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "[+] Cleaning $BuildDir ..."
    Remove-Item -Recurse -Force $BuildDir
}

# ── 5. CMake Configure ──
Write-Host "[+] Configuring..." -ForegroundColor Yellow
$cmakeArgs = @(
    "-B", $BuildDir
    "-G", "Ninja"
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgDir\scripts\buildsystems\vcpkg.cmake"
    "-DVCPKG_TARGET_TRIPLET=$Triplet"
)

if ($Static) {
    $cmakeArgs += "-DSTATIC_BUILD=ON"
}

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }

# ── 6. Build ──
Write-Host "[+] Building..." -ForegroundColor Yellow
cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# ── 7. Deploy ──
$exePath = "$BuildDir\Release\PhoneToolbox.exe"
if (-not (Test-Path $exePath)) {
    # Ninja multi-config might put it in build dir root
    $exePath = "$BuildDir\PhoneToolbox.exe"
}

if (Test-Path $exePath) {
    Write-Host "[+] Build OK: $exePath" -ForegroundColor Green
    $size = (Get-Item $exePath).Length / 1MB
    Write-Host "    Size: $([math]::Round($size, 1)) MB"

    if (-not $Static) {
        # 动态构建：部署 DLL
        Write-Host "[+] Deploying DLLs with windeployqt..." -ForegroundColor Yellow
        $qt6Dir = (Get-Item (Get-Command cmake).Source).Directory.Parent.FullName
        & windeployqt --release --no-compiler-runtime $exePath
        Write-Host "[+] Deployed. Distribute the entire $BuildDir\Release\ folder." -ForegroundColor Green
    } else {
        Write-Host "[+] Static EXE ready. Just distribute PhoneToolbox.exe" -ForegroundColor Green
    }
} else {
    Write-Host "[!] EXE not found at expected path: $exePath" -ForegroundColor Red
    Write-Host "    Check $BuildDir for output files"
}
