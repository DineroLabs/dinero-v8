# Dinero v8.0.0-rc2

First v8 release candidate cut from `DineroLabs/dinero-v8` after the rc1 follow-up fixes. rc2 keeps the rc1 functional payload and tightens the build/release plumbing.

## What's new in rc2 vs rc1

### Fixes shipping in rc2

- **`include/build/build_identity.h` is now in the snapshot.** rc1 (`3491c340`) was missing this pure-C++ header; the build failed on `methods_economics_context.cpp` with `error C1083: Cannot open include file: 'build/build_identity.h'`. Permanent fix lands in commit `5651c42a`. Also narrows the `build*/` `.gitignore` patterns to be root-anchored so they stop sweeping up `include/build/`.
- **Correct Win32 file metadata on every binary.** rc1's `dinero-qt.exe` reported `2.2.5-rc8` under right-click Properties → Details (the hand-rolled `.rc` was never bumped past the pre-consolidation cut), and the daemon binaries (`dinerod`, `dinero-cli`, `dinero-miner`, `dinero-stratum-worker`, `dinero-gpu-miner`, `dinero-wallet-cli`) had no `VS_VERSION_INFO` resource at all — their Properties dialog was empty.
  - rc2 templatizes Win32 version info via a new `cmake/dinero_version_info.rc.in` + a `dinero_attach_win32_versioninfo()` CMake helper. Numeric `FILEVERSION`/`PRODUCTVERSION` come from `PROJECT_VERSION_{MAJOR,MINOR,PATCH}`; the string fields come from a new `DINERO_RELEASE_TAG` cache var (override per cut with `-DDINERO_RELEASE_TAG=8.0.0-rc2`).
  - Wired into all 6 daemon targets + `dinero-solo-miner-cli` + `dinero-qt`. The helper respects `OUTPUT_NAME` so `dinero-solo-miner.exe` shows `dinero-solo-miner` as its InternalName (not the cmake target name `dinero-solo-miner-cli`).
- **NSIS installer metadata is current.** `VIProductVersion` / `VIFileVersion` bumped from `2.2.6.0` to `8.0.0.0`; `APP_URL` now points at `https://github.com/DineroLabs/dinero-v8` rather than the older `dinero-releases`.
- **Linux build now configures on Ubuntu 22.04.** The top-level `CMakeLists.txt` previously did `cmake_policy(SET CMP0144 NEW)` unguarded; that policy was added in CMake 3.27 so Ubuntu 22.04's CMake 3.22 hard-failed at configure. Now guarded behind `if(POLICY CMP0144)`.

No consensus, RPC, or wallet behavior changes vs rc1 — rc2 is a packaging/metadata pass.

## Artifacts

### Dinero-8.0.0-rc2-windows-x86_64-Setup.exe

- **Installer size**: _TBD_ (filled at upload).
- **Bundled payload** (~7 binaries + Qt6 runtime + 4 vcpkg DLLs + icon + LICENSE):
  - **`dinero-qt.exe`** — Qt6 GUI wallet
  - **6 daemon binaries**: `dinerod`, `dinero-cli`, `dinero-miner`, `dinero-stratum-worker`, `dinero-gpu-miner`, `dinero-wallet-cli`
  - **Qt6 runtime**: `Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Network`, `Qt6Svg` + plugin DLLs (platforms, imageformats, styles, tls, networkinformation, iconengines, generic, `opengl32sw`, `D3Dcompiler_47`)
  - **OpenSSL + curl runtime**: `libcurl.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `z.dll`
  - **Branding**: `Dinero-Coin.png` (window icon)
  - `LICENSE`
- **Compression**: LZMA solid (`SetCompressor /SOLID lzma`, dict 64 MB).
- **Install context**: All Users (HKLM 64-bit hive, system-wide Start Menu, `C:\Program Files\Dinero\`). Requires UAC elevation.
- **Uninstaller**: removes `C:\Program Files\Dinero\` + Start Menu group + registry entries; **preserves `%APPDATA%\Dinero\`** so wallets survive reinstalls/upgrades.
- **Silent mode**: `Dinero-Setup.exe /S` for automated deployments; `Uninstall.exe /S` for silent uninstall.

### dinero-v8.0.0-rc2-windows-x86_64-msvc.zip

Raw daemon-stack archive for power users — 6 daemon binaries + `LICENSE` + `README.txt` + `SHA256SUMS.txt`. The zip doesn't include `libcurl.dll` / `libcrypto.dll` / icon, so if you use the zip directly you need to place these alongside the executables yourself (or install via the .exe and copy them out of `C:\Program Files\Dinero\`).

### dinero-v8.0.0-rc2-linux-x86_64.tar.gz

Linux daemon stack — same 6 binaries, daemon-only (no Qt GUI; no GPU miner). Vendored OpenSSL 3.3.2 static-linked into each binary. Tarball layout:

```
bin/
  dinerod
  dinero-cli
  dinero-miner
  dinero-stratum-worker
  dinero-wallet-cli
  dinero-solo-miner
LICENSE
README.txt
SHA256SUMS.txt
```

## Build provenance

Same source tree as rc1 plus the rc2 follow-up commits (`5651c42a`, `ec539db1`, and the CMP0144 guard).

- **Windows**: MSVC 14.44 (Visual Studio 17 2022, x64), Qt 6.9.1 MSVC2022 64-bit, vcpkg dynamic OpenSSL for the daemon stack, vendored static OpenSSL 3.3.2 unavailable on this triplet so `libcurl.dll` + `libcrypto-3-x64.dll` + `libssl-3-x64.dll` + `z.dll` are bundled.
- **Linux**: Ubuntu 22.04, gcc 11.x, vendored OpenSSL 3.3.2 (static), system libcurl4-openssl-dev.

## Embedded file metadata

rc2's `VS_VERSION_INFO` is now consistent across every shipped binary:

| Field            | Value (every binary)            |
|------------------|---------------------------------|
| Company name     | DineroLabs                      |
| Product name     | Dinero                          |
| Product version  | 8.0.0-rc2                       |
| File version     | 8.0.0-rc2                       |
| Numeric version  | 8.0.0.0                         |
| Copyright        | Copyright (C) 2026 DineroLabs   |

Per-binary `FileDescription` (visible in Task Manager / Properties → Details):

| Binary                  | FileDescription                                  |
|-------------------------|--------------------------------------------------|
| `dinerod.exe`           | Dinero full node daemon                          |
| `dinero-cli.exe`        | Dinero RPC CLI client                            |
| `dinero-miner.exe`      | Dinero CPU miner                                 |
| `dinero-stratum-worker.exe` | Dinero Stratum worker client                |
| `dinero-gpu-miner.exe`  | Dinero GPU miner (CUDA/OpenCL)                   |
| `dinero-wallet-cli.exe` | Dinero reference wallet CLI                      |
| `dinero-solo-miner.exe` | Dinero solo miner (mines against a local dinerod) |
| `dinero-qt.exe`         | Dinero Qt Wallet                                 |

Note: the **UAC dialog's "Verified publisher"** line is still "Unknown publisher" because the installer is not yet Authenticode-signed. Once we ship a signed installer the UAC line will read **"Verified publisher: DineroLabs"** — until then the embedded `VS_VERSION_INFO` above is the canonical proof.

## Verifying the download

```powershell
# Windows
Get-FileHash Dinero-8.0.0-rc2-windows-x86_64-Setup.exe -Algorithm SHA256
# Expected: TBD
```

```bash
# Linux
sha256sum dinero-v8.0.0-rc2-linux-x86_64.tar.gz
# Expected: TBD
```

## Known limitations

- **`dinero-gpu-miner`** ships on Windows but its OpenCL runtime path is not yet validated on Windows MSVC; treat as experimental. Not shipped on Linux for rc2 (CUDA not available in the build environment).
- **VC++ Redistributable** — Qt6 was built against the dynamic CRT, so `MSVCP140.dll` + `VCRUNTIME140.dll` + `VCRUNTIME140_1.dll` are runtime requirements on Windows. If launch fails with `MSVCP140.dll not found`, install Microsoft's [Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- **Code signing** — not yet code-signed. Windows SmartScreen may warn; verify SHA256 above and click "More info" → "Run anyway".
