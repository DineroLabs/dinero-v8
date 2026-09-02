# Build and stage the Windows headless server installer.
#
# Output:
#   packaging/windows/dist/Dinero-Server-<VERSION>-windows-x86_64-Setup.exe
#
# This is the operator lane: no Qt GUI, no desktop solo-miner bundle. It
# installs dinerod as a real Windows Service and ships command-line node tools.
# The server build is intentionally GPU-disabled for the daemon so fresh Windows
# Server hosts do not need NVIDIA/CUDA DLLs just to start a full node.
#
# Usage:
#   .\packaging\windows\build-server-installer.ps1 -Version 8.0.0-rc27
#   .\packaging\windows\build-server-installer.ps1 -Version 8.0.0-rc27 -SkipBuild

[CmdletBinding()]
param(
    [string]$Version        = '8.0.0-dev',
    [string]$BuildDir       = '',
    [string]$DaemonBuildDir = '',
    [string]$CMake          = 'cmake',
    [string]$VcpkgRoot      = "$env:USERPROFILE\vcpkg",
    [string]$VcpkgBin       = "$env:USERPROFILE\vcpkg\installed\x64-windows\bin",
    [string]$Makensis           = 'C:\Program Files (x86)\NSIS\makensis.exe',
    [string]$VcRedistPath       = '',
    [string]$SnapshotPath       = '',
    [string]$SnapshotManifestPath = '',
    [string]$SnapshotAssetName  = '',
    [string]$SnapshotFileName   = 'dinero-assumeutxo-99677-v4.dat',
    [string]$SnapshotReleaseTag = 'v8.1.2',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$DistDir     = Join-Path $ScriptDir 'dist'
$Stage       = Join-Path $DistDir 'server-installer-stage'

# Release assets and their trusted runtime filename are normally identical.
# Older releases are not: v8.1.1 published
# dinero-assumeutxo-73035-v4.dat with a manifest that deliberately names the
# installed file mainnet-snapshot.dat. Keep those two identities separate so a
# compatibility smoke does not either fetch the wrong URL or produce a package
# the daemon's manifest preflight will reject.
if (-not $SnapshotAssetName) {
    $SnapshotAssetName = $SnapshotFileName
}

if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot 'build-msvc-server'
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $ProjectRoot $BuildDir
}

if (-not $DaemonBuildDir) {
    $DaemonBuildDir = Join-Path $BuildDir 'Release'
} elseif (-not [System.IO.Path]::IsPathRooted($DaemonBuildDir)) {
    $DaemonBuildDir = Join-Path $ProjectRoot $DaemonBuildDir
}

$BuildRoot = Split-Path $DaemonBuildDir -Parent

Write-Host '----------------------------------------------------------'
Write-Host "Building Dinero Server installer -- v$Version"
Write-Host '----------------------------------------------------------'
Write-Host "Build dir:  $BuildDir"
Write-Host "Binary dir: $DaemonBuildDir"

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE"
    }
}

function Find-Dumpbin {
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $programFilesX86 = ${env:ProgramFiles(x86)}
    if ($programFilesX86) {
        $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null).Trim()
            if ($installPath) {
                $candidate = Get-ChildItem -Path (Join-Path $installPath 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') -ErrorAction SilentlyContinue |
                    Sort-Object FullName -Descending |
                    Select-Object -First 1
                if ($candidate) {
                    return $candidate.FullName
                }
            }
        }
    }

    return $null
}

function Assert-NoCudaLoadTimeImports {
    param([Parameter(Mandatory = $true)][string]$BinaryPath)

    $dumpbin = Find-Dumpbin
    if (-not $dumpbin) {
        Write-Host "WARNING: dumpbin.exe not found; cannot verify CUDA load-time imports for $BinaryPath" -ForegroundColor Yellow
        return
    }

    $deps = (& $dumpbin /dependents $BinaryPath 2>&1) -join "`n"
    if ($deps -match '(?i)\bnvcuda\.dll\b' -or $deps -match '(?i)\bnvrtc[^\s]*\.dll\b') {
        Write-Host "ERROR: $BinaryPath has CUDA/NVRTC load-time imports." -ForegroundColor Red
        Write-Host "Rebuild the server lane with DINERO_WINDOWS_SERVER_BUILD=ON / ENABLE_GPU_MINING=OFF." -ForegroundColor Red
        Write-Host $deps
        exit 1
    }

    Write-Host 'Verified: dinerod.exe has no CUDA/NVRTC load-time imports.'
}

function Resolve-VcRedist {
    if ($VcRedistPath) {
        if (-not (Test-Path $VcRedistPath)) {
            Write-Host "ERROR: -VcRedistPath not found: $VcRedistPath" -ForegroundColor Red
            exit 1
        }
        return (Resolve-Path $VcRedistPath).Path
    }

    if (-not (Test-Path $DistDir)) {
        New-Item $DistDir -ItemType Directory | Out-Null
    }

    # Pinned SHA256 for the VC++ 2015-2022 x64 redistributable.
    # aka.ms/vs/17/release/vc_redist.x64.exe is a MOVING permalink: whatever bytes
    # arrive are cached here, staged into the installer payload, and run silently and
    # elevated (/install /quiet /norestart) on every end-user machine. Pin the hash so
    # a rotated/compromised download fails the build loudly instead of shipping, and so
    # the bundled runtime cannot drift silently when Microsoft updates the permalink.
    #
    # Verified 2026-08-17 by downloading the exact permalink (25,635,768 bytes); the
    # same SHA256 is embedded in Microsoft's CDN download URL path, corroborating it.
    # To refresh: download the new redist, confirm its hash from an official Microsoft
    # source, and bump the pin in BOTH installer scripts (build-server-installer.ps1 and
    # build-installer.ps1 share the dist\ cache and must stay in sync).
    $VCRedistSha256 = 'cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b'

    $cached = Join-Path $DistDir 'vc_redist.x64.exe'
    if (-not (Test-Path $cached)) {
        Write-Host 'Downloading Microsoft VC++ 2015-2022 x64 Redistributable...'
        Invoke-WebRequest 'https://aka.ms/vs/17/release/vc_redist.x64.exe' -OutFile $cached
    }

    # Verify after download AND on cache hit — a poisoned cache is as dangerous as a
    # poisoned download. Delete + fail loudly on mismatch so a stale pin is a deliberate,
    # reviewed bump rather than a silent drift.
    $actualHash = (Get-FileHash -Algorithm SHA256 $cached).Hash.ToLowerInvariant()
    if ($actualHash -ne $VCRedistSha256) {
        Remove-Item $cached -Force -ErrorAction SilentlyContinue
        throw ("vc_redist.x64.exe SHA256 mismatch: got '$actualHash', expected '$VCRedistSha256'. " +
               "Microsoft may have updated the redistributable at aka.ms/vs/17/release. " +
               "Verify the new hash from an official Microsoft source and bump the pin in BOTH " +
               "build-server-installer.ps1 and build-installer.ps1 (they share the dist\ cache).")
    }
    Write-Host "  Verified vc_redist.x64.exe SHA256: $actualHash"
    return $cached
}

function Resolve-Snapshot {
    # Resolve the AssumeUTXO snapshot file the installer will bundle. Returns
    # an absolute path to a verified-by-the-daemon snapshot. The daemon checks
    # the sha256 against its compiled-in trust anchor at load time, so a
    # tampered file is rejected — the installer is just transport here.
    #
    # Precedence:
    #   1. -SnapshotPath (explicit local file; for offline / mirrored builds)
    #   2. Cached copy at $DistDir\$SnapshotAssetName
    #   3. Download $SnapshotAssetName from the v8 release tagged by -SnapshotReleaseTag.
    #
    # Default is the height-99677 v4 anchor published with v8.1.11. The daemon
    # verifies the exact file sha256 and base hash against its compiled registry.
    # Existing installs keep their prior dinero.conf and datadir snapshot, so an
    # in-progress older lifecycle is not rewritten by an installer upgrade. When
    # a future cut publishes a new snapshot height, bump
    # -SnapshotAssetName + -SnapshotFileName + -SnapshotReleaseTag together.

    if ($SnapshotPath) {
        if (-not (Test-Path $SnapshotPath)) {
            Write-Host "ERROR: -SnapshotPath not found: $SnapshotPath" -ForegroundColor Red
            exit 1
        }
        return (Resolve-Path $SnapshotPath).Path
    }

    if (-not (Test-Path $DistDir)) {
        New-Item $DistDir -ItemType Directory | Out-Null
    }

    $cached = Join-Path $DistDir $SnapshotAssetName
    if (-not (Test-Path $cached)) {
        $url = "https://github.com/DineroLabs/dinero-v8/releases/download/$SnapshotReleaseTag/$SnapshotAssetName"
        Write-Host "Downloading AssumeUTXO snapshot from $url..."
        Invoke-WebRequest $url -OutFile $cached
    }
    return $cached
}

function Resolve-SnapshotManifest([string]$SnapshotSourcePath) {
    if ($SnapshotManifestPath) {
        if (-not (Test-Path $SnapshotManifestPath)) {
            throw "-SnapshotManifestPath not found: $SnapshotManifestPath"
        }
        return (Resolve-Path $SnapshotManifestPath).Path
    }

    # Release history contains both <snapshot>.dat.manifest.json and
    # <snapshot>.manifest.json. The installer always places the selected one
    # adjacent to the data under the runtime path <snapshot>.manifest.json.
    $localCandidates = @(
        "$SnapshotSourcePath.manifest.json",
        ([IO.Path]::Combine([IO.Path]::GetDirectoryName($SnapshotSourcePath),
            ([IO.Path]::GetFileName($SnapshotSourcePath) -replace '\.dat$', '.manifest.json')))
    ) | Select-Object -Unique
    foreach ($candidate in $localCandidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }

    $assetCandidates = @(
        "$SnapshotAssetName.manifest.json",
        ($SnapshotAssetName -replace '\.dat$', '.manifest.json')
    ) | Select-Object -Unique
    foreach ($asset in $assetCandidates) {
        $cached = Join-Path $DistDir $asset
        if (Test-Path $cached) { return $cached }
        $url = "https://github.com/DineroLabs/dinero-v8/releases/download/$SnapshotReleaseTag/$asset"
        try {
            Write-Host "Downloading AssumeUTXO manifest from $url..."
            Invoke-WebRequest $url -OutFile $cached -ErrorAction Stop
            return $cached
        } catch {
            if (Test-Path $cached) { Remove-Item $cached -Force }
        }
    }
    throw "No adjacent manifest found for AssumeUTXO snapshot asset $SnapshotAssetName"
}

function Assert-SnapshotManifest([string]$DataPath, [string]$ManifestPath) {
    $manifest = (Get-Content -Raw $ManifestPath | ConvertFrom-Json).snapshot
    if ($manifest.snapshot_file -ne $SnapshotFileName) {
        throw "Snapshot manifest filename '$($manifest.snapshot_file)' does not match '$SnapshotFileName'"
    }
    $actualHash = (Get-FileHash -Algorithm SHA256 $DataPath).Hash.ToLowerInvariant()
    if ([string]$manifest.sha256 -ne $actualHash) {
        throw "Snapshot sha256 '$actualHash' does not match manifest '$($manifest.sha256)'"
    }
    $actualBytes = (Get-Item $DataPath).Length
    if ([int64]$manifest.bytes -ne $actualBytes) {
        throw "Snapshot length '$actualBytes' does not match manifest '$($manifest.bytes)'"
    }
    Write-Host "Verified AssumeUTXO manifest: $actualBytes bytes, sha256 $actualHash"
}

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

if (-not $SkipBuild) {
    Write-Host 'Configuring server-safe MSVC build...'
    $configureArgs = @(
        '-S', $ProjectRoot,
        '-B', $BuildDir,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        '-DDINERO_WINDOWS_SERVER_BUILD=ON',
        '-DDINERO_BUILD_SEEDER=ON',
        '-DDINERO_BUILD_MINER=OFF',
        '-DDINERO_ENABLE_QUIC=ON'
    )
    $vcpkgToolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (Test-Path $vcpkgToolchain) {
        $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    }
    $dependsPrefix = Join-Path $ProjectRoot 'depends\windows-AMD64'
    if (Test-Path $dependsPrefix) {
        $configureArgs += "-DCMAKE_PREFIX_PATH=$dependsPrefix"
    }
    Invoke-NativeCommand $CMake $configureArgs

    Write-Host 'Building server targets...'
    Invoke-NativeCommand $CMake @(
        '--build', $BuildDir,
        '--config', 'Release',
        '--target', 'dinerod', 'dinero-cli', 'dinero-miner', 'dinero-stratum-worker', 'dinero-gpu-miner', 'dinero-wallet-cli', 'dinero-seeder',
        '--parallel', '1'
    )
}

$daemonBinaries = 'dinerod','dinero-cli','dinero-miner','dinero-stratum-worker','dinero-gpu-miner','dinero-wallet-cli','dinero-seeder'
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

Assert-NoCudaLoadTimeImports (Resolve-DaemonBinaryPath 'dinerod')

if (Test-Path $Stage) {
    Write-Host 'Cleaning prior stage...'
    Remove-Item $Stage -Recurse -Force
}
New-Item $Stage -ItemType Directory | Out-Null

Write-Host 'Copying daemon binaries...'
foreach ($b in $daemonBinaries) {
    Copy-Item (Resolve-DaemonBinaryPath $b) (Join-Path $Stage "$b.exe")
    Write-Host "  $b.exe"
}
Copy-Item (Join-Path $ProjectRoot 'LICENSE') (Join-Path $Stage 'LICENSE')

Write-Host 'Copying runtime DLLs from vcpkg if required...'
$vcpkgDlls = @()  # server lane is fully static (vendored libcurl+OpenSSL 3.5.7 + zlib-off)
foreach ($d in $vcpkgDlls) {
    $src = Join-Path $VcpkgBin $d
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $Stage $d)
        Write-Host "  $d"
    } else {
        Write-Host "  WARNING: $d not found at $src" -ForegroundColor Yellow
    }
}

$vcRedist = Resolve-VcRedist
Copy-Item $vcRedist (Join-Path $Stage 'vc_redist.x64.exe')
Write-Host '  vc_redist.x64.exe'

$snapshot = Resolve-Snapshot
Copy-Item $snapshot (Join-Path $Stage $SnapshotFileName)
Write-Host "  $SnapshotFileName"
$snapshotManifest = Resolve-SnapshotManifest $snapshot
Assert-SnapshotManifest $snapshot $snapshotManifest
Copy-Item $snapshotManifest (Join-Path $Stage "$SnapshotFileName.manifest.json")
Write-Host "  $SnapshotFileName.manifest.json"

$totalSize = (Get-ChildItem $Stage -Recurse | Measure-Object Length -Sum).Sum
$fileCount = (Get-ChildItem $Stage -Recurse -File).Count
Write-Host ("Stage: $fileCount files, {0:N2} MB" -f ($totalSize / 1MB))

Write-Host 'Running makensis (LZMA solid compression)...'
$numericVersion = if ($Version -match '^(\d+)\.(\d+)\.(\d+)') {
    "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
} else {
    '0.0.0.0'
}
Push-Location $ScriptDir
try {
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Makensis "/DVERSION=$Version" "/DVERSION_NUMERIC=$numericVersion" "/DSNAPSHOT_FILE=$SnapshotFileName" 'dinero-server-installer.nsi' | Out-Null
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
