# Stage the daemon binaries + vcpkg runtime DLLs, then run makensis
# against dinero-server-installer.nsi to produce
# dist\Dinero-Server-<VERSION>-windows-x86_64-Setup.exe.
#
# This is the OPERATOR lane for Windows: headless install with dinerod
# registered as a Windows Service, no Qt6 GUI. Parallel to
# build-installer.ps1 (the user lane that ships dinero-qt).
#
# Prerequisites:
#   - The Dinero daemon stack already built under build-msvc-native\
#     via the same cmake invocation as the user installer:
#       cmake -S . -B build-msvc-native -G "Visual Studio 17 2022" -A x64 ^
#             -DCMAKE_BUILD_TYPE=Release ^
#             -DDINERO_BUILD_QT=ON -DDINERO_BUILD_MINER=ON ^
#             -DMINER_ENABLE_CUDA=ON -DMINER_ENABLE_OPENCL=ON ^
#             -DENABLE_GPU_MINING=ON -DENABLE_GRPC=OFF ^
#             -DENABLE_HARDWARE_WALLETS=OFF ^
#             -DCMAKE_PREFIX_PATH="C:\Qt\6.9.1\msvc2022_64"
#       cmake --build build-msvc-native --config Release ^
#             --target dinerod dinero-cli dinero-miner dinero-stratum-worker dinero-gpu-miner dinero-wallet-cli dinero-seeder
#   - NSIS at "C:\Program Files (x86)\NSIS\makensis.exe"
#   - vcpkg installed at ~\vcpkg (for the runtime DLLs)
#
# Usage:
#   .\packaging\windows\build-server-installer.ps1 -Version 8.0.0-rc3

[CmdletBinding()]
param(
    [string]$Version       = '8.0.0-dev',
    [string]$DaemonBuildDir = '',
    [string]$VcpkgBin      = "$env:USERPROFILE\vcpkg\installed\x64-windows\bin",
    [string]$Makensis      = 'C:\Program Files (x86)\NSIS\makensis.exe'
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path

if (-not $DaemonBuildDir) {
    $DaemonBuildDir = Join-Path $ProjectRoot 'build-msvc-native\Release'
}

$Stage = Join-Path $ScriptDir 'dist\server-installer-stage'

Write-Host '----------------------------------------------------------'
Write-Host "Building Dinero Server installer -- v$Version"
Write-Host '----------------------------------------------------------'

# Sanity checks. dinero-qt + dinero-solo-miner are intentionally NOT
# in this list — the server installer ships headless.
$daemonBinaries = 'dinerod','dinero-cli','dinero-miner','dinero-stratum-worker','dinero-gpu-miner','dinero-wallet-cli','dinero-seeder'
$BuildRoot = Split-Path $DaemonBuildDir -Parent

function Resolve-DaemonBinaryPath([string]$BinaryName) {
    $primary = Join-Path $DaemonBuildDir "$BinaryName.exe"
    if (Test-Path $primary) {
        return $primary
    }
    if ($BinaryName -eq 'dinero-seeder') {
        $seederPath = Join-Path $BuildRoot 'seeder\Release\dinero-seeder.exe'
        if (Test-Path $seederPath) {
            return $seederPath
        }
    }
    return $primary
}

foreach ($b in $daemonBinaries) {
    if (-not (Test-Path (Resolve-DaemonBinaryPath $b))) {
        Write-Host "ERROR: $b.exe not found in $DaemonBuildDir" -ForegroundColor Red
        exit 1
    }
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

Write-Host "Copying daemon binaries..."
foreach ($b in $daemonBinaries) {
    Copy-Item (Resolve-DaemonBinaryPath $b) (Join-Path $Stage "$b.exe")
    Write-Host "  $b.exe"
}
Copy-Item (Join-Path $ProjectRoot 'LICENSE') (Join-Path $Stage 'LICENSE')

# vcpkg runtime DLLs. Same chain as the user installer: dinerod and the
# miner binaries link against libcurl + OpenSSL DLLs at runtime.
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

$totalSize = (Get-ChildItem $Stage -Recurse | Measure-Object Length -Sum).Sum
$fileCount = (Get-ChildItem $Stage -Recurse -File).Count
Write-Host ("Stage: $fileCount files, {0:N2} MB" -f ($totalSize / 1MB))

Write-Host "Running makensis (LZMA solid compression)..."
Push-Location $ScriptDir
try {
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Makensis "/DVERSION=$Version" 'dinero-server-installer.nsi' | Out-Null
    } finally {
        $ErrorActionPreference = $oldEAP
    }
} finally {
    Pop-Location
}

$installerPath = Join-Path $ScriptDir "dist\Dinero-Server-$Version-windows-x86_64-Setup.exe"
if (-not (Test-Path $installerPath)) {
    Write-Host "ERROR: makensis ran but $installerPath not produced." -ForegroundColor Red
    exit 1
}
$installerSize = (Get-Item $installerPath).Length
$installerHash = (Get-FileHash $installerPath -Algorithm SHA256).Hash.ToLower()

Write-Host ''
Write-Host '----------------------------------------------------------'
Write-Host 'Server installer ready' -ForegroundColor Green
Write-Host '----------------------------------------------------------'
Write-Host "  Path:   $installerPath"
Write-Host ("  Size:   $installerSize bytes ({0:N2} MB)" -f ($installerSize / 1MB))
Write-Host "  SHA256: $installerHash"
