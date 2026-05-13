## What's new in rc4 vs rc3

This release wraps the native-MSVC daemon stack from rc3 plus the Qt6 GUI into a **one-click NSIS installer** for Windows x64. End users no longer need to download a ZIP, extract it, and figure out which `.exe` to run — `Dinero-2.2.6-rc4-windows-x86_64-Setup.exe` installs everything to `C:\Program Files\Dinero\` with a Start Menu shortcut and a normal Add/Remove Programs entry.

The same daemon-stack ZIP from rc3 is still attached for power users who want the raw binaries.

### Dinero-2.2.6-rc4-windows-x86_64-Setup.exe

- **One-click installer**: standard NSIS wizard (Welcome / License / Directory / Install / Finish). Default path `C:\Program Files\Dinero\`; user can change it. Optional desktop shortcut. "Launch Dinero" checkbox on the Finish page.
- **Bundled payload** (16 files + 7 Qt plugin subdirs, ~77 MB uncompressed):
  - **`dinero-qt.exe`** — Qt6 GUI wallet (Bitcoin Core-style: balance, send, receive, transactions, debug console, network monitor)
  - **6 daemon binaries**: `dinerod`, `dinero-cli`, `dinero-miner`, `dinero-stratum-worker`, `dinero-gpu-miner`, `dinero-wallet-cli`
  - **Qt6 runtime** (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Network`, `Qt6Svg`) + plugin DLLs (`platforms\qwindows.dll`, `tls\qschannelbackend.dll`, `styles\qmodernwindowsstyle.dll`, image-format plugins, etc.)
  - `LICENSE`
- **Compression**: LZMA solid (single-stream LZMA across all files). 77 MB raw → 22 MB compressed (29% ratio).
- **Install context**: All Users (HKLM, system-wide Start Menu, `C:\Program Files`). Requires UAC elevation.
- **Uninstaller**:
  - Removes everything under `C:\Program Files\Dinero\` and the Start Menu group.
  - Removes registry entries (own key + Add/Remove Programs).
  - **Preserves `%APPDATA%\Dinero\`** so wallets survive reinstalls and upgrades.
- **Silent mode**: `Dinero-Setup.exe /S` for automated deployments; `Uninstall.exe /S` for silent uninstall (`QuietUninstallString` populated in the Add/Remove entry too).

### dinero-v2.2.6-rc4-windows-x86_64-msvc.zip

Same daemon stack as rc3's ZIP, unchanged in layout (`bin/` + LICENSE + README.txt + SHA256SUMS.txt). Power users who want just the daemon binaries can use this instead of the full installer.

### Build provenance

Identical to rc3 for the daemon stack — same source commit, same toolchain:

- **Toolchain**: Microsoft Visual Studio 2022 Build Tools, MSVC 14.44.35207
- **Architecture**: x86_64
- **Qt**: 6.9.1 MSVC2022 64-bit (from the Qt installer at `C:\Qt\6.9.1\msvc2022_64`)
- **OpenSSL**: vendored static (`no-shared`), built by `scripts/build-openssl-vendored.ps1`
- **NSIS**: 3.x with LZMA solid mode (`SetCompressor /SOLID lzma`, dict 64 MB)
- **Runtime DLL strategy**: `/MT` static linkage for the daemon stack; Qt6 DLLs bundled via `windeployqt`

### Verification on the MSVC build

- **Smoke-tested install/uninstall cycle**: silent install (`/S`) → install dir present with 16 files + 7 plugin dirs → All Users Start Menu shortcut present → HKLM 64-bit Add/Remove entry `Dinero v2.2.6-rc4` populated → uninstaller removes install dir, Start Menu, and registry entries cleanly → **`%APPDATA%\Dinero\` datadir survives** as required.
- **Consensus output**: identical to rc3 — 13/14 golden-vector tests match byte-for-byte, regtest end-to-end smoke (genesis hash + mined block hash + chainwork) is platform-independent.
- **ctest sweep on the daemon stack**: 270 tests pass; 79 POSIX-only `.sh` integration scripts cleanly excluded on Windows.

### Compatibility matrix

| Binary | Use the release |
|---|---|
| GUI wallet + daemon + miners + wallet-cli (one-click) | **this rc4** installer |
| Daemon stack as raw ZIP (no Qt GUI) | **this rc4** zip or [rc3](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc3) |
| `dinero-solo-miner` (with CUDA backend) | [rc2](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc2) |
| Stratum V2 binaries, Linux aarch64 | [rc1](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc1) |

### Verifying the download

```powershell
Get-FileHash Dinero-2.2.6-rc4-windows-x86_64-Setup.exe -Algorithm SHA256
# Expected: 9809b3836a71e4680761c43df0665c2089be30a133c52c84b3ed40d1b6f2ad91
```

### Known limitations on Windows

- **`dinero-gpu-miner.exe`** ships but its OpenCL runtime path is not validated on Windows. Treat as experimental until a Windows GPU hashrate baseline is published.
- **Code signing** — the installer + binaries are not yet code-signed. Windows SmartScreen may warn ("Windows protected your PC"); click "More info" → "Run anyway" or verify SHA256 manually against the value above.
- **Per-user installs not supported** — the installer requires UAC elevation and installs system-wide. A `/CurrentUser` mode and a portable-extract option are tracked for GA.

### Quick start

After install:
- Start Menu → **Dinero** → launches the Qt GUI
- All daemon binaries (`dinerod.exe`, `dinero-cli.exe`, etc.) live in `C:\Program Files\Dinero\` and are added to `%PATH%` for the install context. Open a PowerShell and run `dinero-cli getblockchaininfo` against a running `dinerod`.

### Reproducing this build from source

```powershell
git clone https://github.com/DineroLabs/Dinero-Coin
git clone https://github.com/DineroLabs/dinero-qt   # sibling checkout
git clone https://github.com/DineroLabs/dinero-solo-miner

# One-time per machine:
#   1) Strawberry Perl (https://strawberryperl.com)
#   2) Visual Studio 2022 Build Tools (C++ x86/x64 workload)
#   3) Qt 6.9.x MSVC2022 64-bit (online installer)
#   4) winget install NSIS.NSIS

cd Dinero-Coin
.\scripts\build-openssl-vendored.ps1
cmake -S . -B build-msvc-native -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc-native --config Release `
    --target dinerod dinero-cli dinero-miner `
             dinero-stratum-worker dinero-gpu-miner dinero-wallet-cli

cd ..\dinero-qt
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake" `
    -DCMAKE_PREFIX_PATH="C:\Qt\6.9.1\msvc2022_64"
cmake --build build-msvc --config Release --target dinero-qt

cd ..\Dinero-Coin
.\packaging\windows\build-installer.ps1   # stages + windeployqt + makensis
```
