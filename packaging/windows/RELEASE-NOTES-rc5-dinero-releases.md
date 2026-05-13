## What's new in rc5 vs rc4

**rc5 fixes runtime DLL bundling and the missing window icon.** rc4 (deleted) launched the binaries with `libcurl.dll`, `libcrypto-3-x64.dll`, and the Dinero window icon all missing — fresh installs would fail at launch with "DLL was not found" before any of our code ran.

Root cause: vcpkg's default `x64-windows` triplet builds shared libraries (DLLs), not static, so our daemon stack and dinero-qt link against `libcurl.dll` + the OpenSSL DLLs at runtime. `windeployqt` only bundles the Qt6 chain; everything else has to be copied manually. rc5 does that copy.

Fixes shipping in rc5:
- **`libcurl.dll`** bundled (used by dinero-qt, dinero-miner, dinero-gpu-miner, dinero-cli, dinero-wallet-cli)
- **`libcrypto-3-x64.dll`** bundled (used by dinero-qt, dinerod, and others)
- **`libssl-3-x64.dll`** bundled (TLS path for HTTPS RPC clients)
- **`z.dll`** bundled (zlib, transitive dep of libcurl)
- **`Dinero-Coin.png`** bundled next to `dinero-qt.exe` (the window icon `mainwindow.cpp` resolves at startup)

Everything else from rc4's intended payload is unchanged.

### Dinero-2.2.6-rc5-windows-x86_64-Setup.exe

- **Installer size**: 24.65 MB (vs 22.12 MB in rc4 — the +2.5 MB is the 4 vcpkg DLLs).
- **Bundled payload** (31 files + 7 Qt plugin subdirs, ~82 MB uncompressed):
  - **`dinero-qt.exe`** — Qt6 GUI wallet
  - **6 daemon binaries**: `dinerod`, `dinero-cli`, `dinero-miner`, `dinero-stratum-worker`, `dinero-gpu-miner`, `dinero-wallet-cli`
  - **Qt6 runtime**: `Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Network`, `Qt6Svg` + plugin DLLs (platforms, imageformats, styles, tls, networkinformation, iconengines, generic, qtuiotouchplugin, opengl32sw, D3Dcompiler_47)
  - **OpenSSL + curl runtime**: `libcurl.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `z.dll`
  - **Branding**: `Dinero-Coin.png` (window icon)
  - `LICENSE`
- **Compression**: LZMA solid (`SetCompressor /SOLID lzma`, dict 64 MB). 82 MB raw → 24.65 MB installer (30% ratio).
- **Install context**: All Users (HKLM 64-bit hive, system-wide Start Menu, `C:\Program Files\Dinero\`). Requires UAC elevation.
- **Uninstaller**:
  - Removes everything under `C:\Program Files\Dinero\` and the Start Menu group.
  - Removes registry entries (own key + Add/Remove Programs).
  - **Preserves `%APPDATA%\Dinero\`** so wallets survive reinstalls and upgrades.
- **Silent mode**: `Dinero-Setup.exe /S` for automated deployments; `Uninstall.exe /S` for silent uninstall.

### dinero-v2.2.6-rc5-windows-x86_64-msvc.zip

Raw daemon-stack archive for power users — same 6 binaries + LICENSE + README + SHA256SUMS as rc4's zip, version-stamped rc5. The zip doesn't include `libcurl.dll` / `libcrypto.dll` / icon, so if you use the zip directly you need to place these alongside the executables yourself (or install via the .exe and copy them out of `C:\Program Files\Dinero\`).

### Build provenance

Identical to rc4 — same source commits, same toolchain (MSVC 14.44, Qt 6.9.1 MSVC2022, vendored static OpenSSL for dinerod, vcpkg dynamic OpenSSL for dinero-qt path).

### Smoke test trail

Silent install (`/S`) → install dir present with 31 files + 7 plugin dirs + 4 vcpkg DLLs + `Dinero-Coin.png` → HKLM 64-bit Uninstall entry `Dinero v2.2.6-rc5` by `DineroLabs` present → run `dinerod.exe --version` → loads (libcrypto-3-x64.dll resolves) → run `dinero-miner.exe --version` → loads (libcurl.dll resolves) → launch `dinero-qt.exe` → stays running 5s with no DLL-not-found crash → uninstaller removes install dir + registry + Start Menu → `%APPDATA%\Dinero\` survives.

### Compatibility matrix

| Binary | Use the release |
|---|---|
| Full Windows install (GUI + daemon + miners, one-click) | **this rc5** installer |
| Daemon stack raw ZIP (no Qt GUI, BYO runtime DLLs) | **this rc5** zip or [rc3](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc3) |
| `dinero-solo-miner` (CUDA backend) | [rc2](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc2) |
| Stratum V2 binaries, Linux aarch64 | [rc1](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc1) |

### Embedded file metadata

`Dinero-2.2.6-rc5-windows-x86_64-Setup.exe` ships with `VIAddVersionKey` populated so right-click -> Properties -> Details shows the publisher:

| Field | Value |
|---|---|
| Company name | DineroLabs |
| Product name | Dinero |
| File version | 2.2.6-rc5 |
| Copyright | Copyright (C) 2026 DineroLabs |
| Description | Dinero Installer |

Note: the **UAC dialog's "Verified publisher"** line is still "Unknown publisher" because the installer is not yet Authenticode-signed. Authenticode requires a code-signing certificate from a CA (Sectigo / DigiCert / GlobalSign typically $300-500/yr, EV certs more). Once we ship a signed installer the UAC line will read **"Verified publisher: DineroLabs"** — until then the file metadata above is the canonical proof.

### Verifying the download

```powershell
Get-FileHash Dinero-2.2.6-rc5-windows-x86_64-Setup.exe -Algorithm SHA256
# Expected: a4d7a51933eede8372be294061d16cc7f6d59793fe41d8401bb27967916b1aaa
```

### Known limitations on Windows

- **`dinero-gpu-miner.exe`** ships but its OpenCL runtime path is not validated on Windows. Treat as experimental.
- **VC++ Redistributable** — the daemon stack uses `/MT` static CRT linkage, but Qt6 was built against the dynamic CRT, so `MSVCP140.dll` + `VCRUNTIME140.dll` + `VCRUNTIME140_1.dll` are runtime requirements. Almost every modern Windows machine has them via Windows Update or any prior VC++ Redistributable install; if launch fails with `MSVCP140.dll not found`, install Microsoft's [Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe). A future rc may bundle the redist as a sub-installer.
- **Code signing** — not yet code-signed. Windows SmartScreen may warn; verify SHA256 above and click "More info" → "Run anyway".
