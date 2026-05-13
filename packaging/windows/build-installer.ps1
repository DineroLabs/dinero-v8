# Stage all the daemon binaries + dinero-qt + Qt6 runtime via
# windeployqt, then run makensis to produce
# dist/Dinero-<VERSION>-windows-x86_64-Setup.exe.
#
# Phase 3 of the v8.0.0 monorepo consolidation: defaults now point at
# the in-tree qt/ subdir build output (build-msvc-native\qt\Release)
# instead of the pre-consolidation sibling repo (..\dinero-qt). Operators
# building from a pre-v8 layout can still override via -QtBuildDir /
# -QtRepoDir to point at the old sibling-repo paths.
#
# Prerequisites (one-time per machine):
#   - Visual Studio 2022 Build Tools (provides cl.exe + nmake)
#   - Qt 6.9.x MSVC2022 64-bit at C:\Qt\6.9.x\msvc2022_64
#       (override with -QtBin if installed elsewhere)
#   - NSIS at "C:\Program Files (x86)\NSIS\makensis.exe"
#       (winget install NSIS.NSIS)
#   - The Dinero monorepo stack already built under
#       build-msvc-native\ via:
#       cmake -S . -B build-msvc-native -G "Visual Studio 17 2022" -A x64 ^
#             -DCMAKE_BUILD_TYPE=Release ^
#             -DDINERO_BUILD_QT=ON -DDINERO_BUILD_MINER=ON ^
#             -DMINER_ENABLE_CUDA=ON -DMINER_ENABLE_OPENCL=ON ^
#             -DENABLE_GPU_MINING=ON -DENABLE_GRPC=OFF ^
#             -DENABLE_HARDWARE_WALLETS=OFF ^
#             -DCMAKE_PREFIX_PATH="C:\Qt\6.9.1\msvc2022_64"
#       cmake --build build-msvc-native --config Release ^
#             --target dinerod dinero-cli dinero-solo-miner-cli dinero-qt
#
# Usage:
#   .\packaging\windows\build-installer.ps1 -Version 8.0.0
#   .\packaging\windows\build-installer.ps1 -Version 8.0.0-rc1
#   .\packaging\windows\build-installer.ps1 -QtBin "C:\Qt\6.9.2\msvc2022_64\bin"

[CmdletBinding()]
param(
    [string]$Version       = '8.0.0-dev',
    [string]$DaemonBuildDir = '',
    [string]$QtBuildDir    = '',
    [string]$QtRepoDir     = '',
    [string]$QtBin         = 'C:\Qt\6.9.1\msvc2022_64\bin',
    [string]$VcpkgBin      = "$env:USERPROFILE\vcpkg\installed\x64-windows\bin",
    [string]$Makensis      = 'C:\Program Files (x86)\NSIS\makensis.exe'
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path

if (-not $DaemonBuildDir) {
    $DaemonBuildDir = Join-Path $ProjectRoot 'build-msvc-native\Release'
}
if (-not $QtBuildDir) {
    # v8 monorepo: dinero-qt is built in-tree under qt/. The Visual Studio
    # generator's multi-config output puts it at <build>\qt\Release\.
    $QtBuildDir = Join-Path $ProjectRoot 'build-msvc-native\qt\Release'
}
if (-not $QtRepoDir) {
    # v8 monorepo: qt/ is a subdir of the dinero repo, not a sibling.
    $QtRepoDir = Join-Path $ProjectRoot 'qt'
}

$Stage = Join-Path $ScriptDir 'dist\installer-stage'

Write-Host '----------------------------------------------------------'
Write-Host "Building Dinero installer -- v$Version"
Write-Host '----------------------------------------------------------'

# Sanity checks
$qtExe = Join-Path $QtBuildDir 'dinero-qt.exe'
if (-not (Test-Path $qtExe)) {
    Write-Host "ERROR: dinero-qt.exe not found at $qtExe" -ForegroundColor Red
    Write-Host "Build it first: cmake --build $(Split-Path $QtBuildDir) --config Release --target dinero-qt" -ForegroundColor Yellow
    exit 1
}
$daemonBinaries = 'dinerod','dinero-cli','dinero-miner','dinero-stratum-worker','dinero-gpu-miner','dinero-wallet-cli'
foreach ($b in $daemonBinaries) {
    if (-not (Test-Path (Join-Path $DaemonBuildDir "$b.exe"))) {
        Write-Host "ERROR: $b.exe not found in $DaemonBuildDir" -ForegroundColor Red
        exit 1
    }
}
$windeployqt = Join-Path $QtBin 'windeployqt.exe'
if (-not (Test-Path $windeployqt)) {
    Write-Host "ERROR: windeployqt.exe not found at $windeployqt (override with -QtBin)" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $Makensis)) {
    Write-Host "ERROR: makensis.exe not found at $Makensis (override with -Makensis)" -ForegroundColor Red
    exit 1
}

# Stage
if (Test-Path $Stage) {
    Write-Host "Cleaning prior stage..."
    Remove-Item $Stage -Recurse -Force
}
New-Item $Stage -ItemType Directory | Out-Null

Write-Host "Copying dinero-qt + daemon binaries..."
Copy-Item $qtExe (Join-Path $Stage 'dinero-qt.exe')
foreach ($b in $daemonBinaries) {
    Copy-Item (Join-Path $DaemonBuildDir "$b.exe") (Join-Path $Stage "$b.exe")
}
Copy-Item (Join-Path $ProjectRoot 'LICENSE') (Join-Path $Stage 'LICENSE')

# Bundle the dinero-qt window icon. mainwindow.cpp calls
# resolveBundledAssetPath("Dinero-Coin.png") which checks first for an
# adjacent file before any Qt resource fallback, so the icon needs to
# live next to dinero-qt.exe.
$iconSrc = Join-Path $QtRepoDir 'Dinero-Coin.png'
if (Test-Path $iconSrc) {
    Copy-Item $iconSrc (Join-Path $Stage 'Dinero-Coin.png')
    Write-Host '  Dinero-Coin.png (window icon)'
} else {
    Write-Host "  WARNING: $iconSrc not found; window icon will be missing." -ForegroundColor Yellow
}

# vcpkg's x64-windows triplet links our daemon stack against shared
# OpenSSL + libcurl rather than static. windeployqt only bundles Qt
# DLLs, so the runtime DLL chain (libcurl + libcrypto/libssl + zlib)
# must be copied manually. dumpbin shows these are the direct +
# transitive deps; everything else is Windows system or VC++ runtime.
Write-Host "Copying runtime DLLs from vcpkg..."
$vcpkgDlls = 'libcurl.dll','libcrypto-3-x64.dll','libssl-3-x64.dll','z.dll'
foreach ($d in $vcpkgDlls) {
    $src = Join-Path $VcpkgBin $d
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $Stage $d)
        Write-Host "  $d"
    } else {
        Write-Host "  WARNING: $d not found at $src" -ForegroundColor Yellow
    }
}

Write-Host "Running windeployqt..."
$oldEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $windeployqt --release --no-translations (Join-Path $Stage 'dinero-qt.exe')
} finally {
    $ErrorActionPreference = $oldEAP
}

$totalSize = (Get-ChildItem $Stage -Recurse | Measure-Object Length -Sum).Sum
$fileCount = (Get-ChildItem $Stage -Recurse -File).Count
Write-Host ("Stage: $fileCount files, {0:N2} MB" -f ($totalSize / 1MB))

Write-Host "Running makensis (LZMA solid compression, takes ~1 min)..."
Push-Location $ScriptDir
try {
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Makensis "/DVERSION=$Version" 'dinero-installer.nsi' | Out-Null
    } finally {
        $ErrorActionPreference = $oldEAP
    }
} finally {
    Pop-Location
}

$installerPath = Join-Path $ScriptDir "dist\Dinero-$Version-windows-x86_64-Setup.exe"
if (-not (Test-Path $installerPath)) {
    Write-Host "ERROR: makensis ran but $installerPath not produced." -ForegroundColor Red
    exit 1
}
$installerSize = (Get-Item $installerPath).Length
$installerHash = (Get-FileHash $installerPath -Algorithm SHA256).Hash.ToLower()

Write-Host ''
Write-Host '----------------------------------------------------------'
Write-Host 'Installer ready' -ForegroundColor Green
Write-Host '----------------------------------------------------------'
Write-Host "  Path:   $installerPath"
Write-Host ("  Size:   $installerSize bytes ({0:N2} MB)" -f ($installerSize / 1MB))
Write-Host "  SHA256: $installerHash"
