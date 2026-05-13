$ErrorActionPreference = "Stop"

$Version = if ($env:RELEASE_VERSION) { $env:RELEASE_VERSION } else { "v2.0.1-dinero-rings" }
$BuildDir = "build-release"
$Platform = "windows"
$Arch = "x86_64"

Write-Host "Building DineroCoin $Version for $Platform-$Arch..."

# Clean previous build
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
if (Test-Path "dist") { Remove-Item -Recurse -Force dist }

# Configure build
cmake -S . -B $BuildDir `
  -DCMAKE_BUILD_TYPE=Release `
  -DDINERO_USE_VENDORED_DEPS=ON `
  -DUSE_SYSTEM_OPENSSL=OFF `
  -DENABLE_TESTS=OFF `
  -DENABLE_BENCHMARKS=OFF

# Build binaries
cmake --build $BuildDir --config Release --parallel

# Create distribution structure
$DistDir = "dist/DineroCoin-$Version-$Platform-$Arch"
New-Item -ItemType Directory -Force -Path "$DistDir/bin" | Out-Null

# Copy binaries
if (Test-Path "$BuildDir/Release/dinerod.exe") {
    Copy-Item "$BuildDir/Release/dinerod.exe" "$DistDir/bin/"
}
if (Test-Path "$BuildDir/Release/dinero-cli.exe") {
    Copy-Item "$BuildDir/Release/dinero-cli.exe" "$DistDir/bin/"
}
if (Test-Path "$BuildDir/Release/dinero-qt.exe") {
    Copy-Item "$BuildDir/Release/dinero-qt.exe" "$DistDir/"
}

# Copy documentation
@("README.md") | ForEach-Object {
    if (Test-Path $_) {
        Copy-Item $_ "$DistDir/"
    }
}

@("EXECUTIVE_SUMMARY.md", "EXCHANGE_DUE_DILIGENCE.md", "AUDITOR_ONBOARDING_PACK.md", "SOC_AUDITOR_CHECKLIST.md") | ForEach-Object {
    if (Test-Path "docs/$_") {
        Copy-Item "docs/$_" "$DistDir/"
    }
}

# Create INSTALL.md
$Commit = if (Get-Command git -ErrorAction SilentlyContinue) {
    git rev-parse --short HEAD 2>$null
} else {
    "unknown"
}

@"
# DineroCoin $Version — Installation Guide

**Platform:** $Platform ($Arch)
**Build Date:** $((Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm") + " UTC")
**Commit:** $Commit

## Quick Start

### GUI Wallet
1. Extract this archive
2. Run ``dinero-qt.exe``

### Command Line
``````cmd
cd bin
dinerod.exe -daemon
dinero-cli.exe getblockchaininfo
``````

## Support
- Repository: https://github.com/Trucker2827/Dinero-Coin
- Security: security@dinero-coin.com
"@ | Out-File -FilePath "$DistDir/INSTALL.md" -Encoding UTF8

# Create ZIP archive
Compress-Archive -Path "dist/DineroCoin-$Version-$Platform-$Arch" -DestinationPath "dist/DineroCoin-$Version-$Platform-$Arch.zip" -Force

# Generate checksum
$Hash = (Get-FileHash "dist/DineroCoin-$Version-$Platform-$Arch.zip" -Algorithm SHA256).Hash.ToLower()
"$Hash  DineroCoin-$Version-$Platform-$Arch.zip" | Out-File -FilePath "dist/DineroCoin-$Version-$Platform-$Arch.zip.sha256" -Encoding ASCII

# Generate build attestation (PowerShell version)
$CommitFull = if (Get-Command git -ErrorAction SilentlyContinue) { git rev-parse HEAD 2>$null } else { "unknown" }
$CommitShort = if (Get-Command git -ErrorAction SilentlyContinue) { git rev-parse --short HEAD 2>$null } else { "unknown" }
$Tag = if (Get-Command git -ErrorAction SilentlyContinue) { git describe --tags --exact-match 2>$null } else { "untagged" }
if (-not $Tag) { $Tag = "untagged" }
$SourceEpoch = if (Get-Command git -ErrorAction SilentlyContinue) { git log -1 --format=%ct 2>$null } else { "0" }
if (-not $SourceEpoch) { $SourceEpoch = "0" }
$ArtifactSize = (Get-Item "dist/DineroCoin-$Version-$Platform-$Arch.zip").Length

# Get vendored dependency versions
$RocksDBCommit = if (Get-Command git -ErrorAction SilentlyContinue) {
  Push-Location third_party/rocksdb -ErrorAction SilentlyContinue
  $commit = git rev-parse --short HEAD 2>$null
  Pop-Location
  if ($commit) { $commit } else { "unknown" }
} else { "unknown" }

$GTestCommit = if (Get-Command git -ErrorAction SilentlyContinue) {
  Push-Location third_party/googletest -ErrorAction SilentlyContinue
  $commit = git rev-parse --short HEAD 2>$null
  Pop-Location
  if ($commit) { $commit } else { "unknown" }
} else { "unknown" }

$RocksDBTag = if (Get-Command git -ErrorAction SilentlyContinue) {
  Push-Location third_party/rocksdb -ErrorAction SilentlyContinue
  $tag = git describe --tags --exact-match 2>$null
  Pop-Location
  if ($tag) { $tag } else { "unknown" }
} else { "unknown" }

$GTestTag = if (Get-Command git -ErrorAction SilentlyContinue) {
  Push-Location third_party/googletest -ErrorAction SilentlyContinue
  $tag = git describe --tags --exact-match 2>$null
  Pop-Location
  if ($tag) { $tag } else { "v1.14.0" }
} else { "v1.14.0" }

@"
{
  "version": "1.0",
  "attestation_type": "dinero-build-attestation",
  "generated_at": "$((Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))",
  "release": {
    "version": "$Version",
    "git_commit": "$CommitFull",
    "git_commit_short": "$CommitShort",
    "git_tag": "$Tag"
  },
  "build": {
    "platform": "$Platform",
    "architecture": "$Arch",
    "timestamp": "$((Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))",
    "source_date_epoch": "$SourceEpoch"
  },
  "toolchain": {
    "compiler": "MSVC",
    "cmake": "$(cmake --version 2>$null | Select-Object -First 1)",
    "build_type": "Release"
  },
  "artifacts": [
    {
      "filename": "DineroCoin-$Version-$Platform-$Arch.zip",
      "sha256": "$Hash",
      "size_bytes": $ArtifactSize
    }
  ],
  "dependencies": {
    "mode": "vendored",
    "rocksdb": {
      "source": "facebook/rocksdb",
      "commit": "$RocksDBCommit",
      "tag": "$RocksDBTag"
    },
    "googletest": {
      "source": "google/googletest",
      "commit": "$GTestCommit",
      "tag": "$GTestTag"
    }
  },
  "reproducibility": {
    "instructions": "https://github.com/Trucker2827/Dinero-Coin/blob/main/.github/workflows/README.md",
    "script": "scripts/build-release-node.ps1",
    "environment": "GitHub Actions (native runner)"
  }
}
"@ | Out-File -FilePath "dist/BUILD_ATTESTATION.json" -Encoding UTF8

Write-Host "✅ Build attestation generated: dist/BUILD_ATTESTATION.json"

Write-Host "`nBuild complete: dist/DineroCoin-$Version-$Platform-$Arch.zip"
Get-Content "dist/DineroCoin-$Version-$Platform-$Arch.zip.sha256"
