# Release-notes template — Downloads organized by platform
#
# GitHub release assets are a flat, alphabetically-sorted list; past releases
# (26+ assets) were unreadable. Every release's BODY therefore carries a
# "Downloads by platform" section: macOS (Apple Silicon | Intel columns),
# Windows, Linux, Docker, snapshots, checksums — in that order, tables with
# direct asset links. Replace {VERSION}; delete rows whose asset does not
# exist in the release; add new variants (e.g. a Ventura/macOS-13 Qt build)
# as new rows in their platform table. Keep the changelog at the top short —
# the per-platform tables are the body of the page.
#
# v8.1.4 is the reference implementation of this layout.

## Dinero v{VERSION}

**Desktop reliability release.** The daemon is byte-identical to v8.1.3 — node operators do not need to upgrade. Consensus rules (block-100,000 CTV/CCV activation) and the height-84,131 AssumeUTXO fast-sync anchor are unchanged. **Upgrade before block 99,000** if you run v8.1.0 or older.

**Fixed:** dinero-qt no longer gives up when a fully-synced node starts slowly — it now waits with exponential backoff and connects as soon as the daemon is ready ("daemon didn't start after 3 minutes", #558).
**New:** first release with Authenticode-signed Windows binaries (Microsoft-ID-verified publisher). macOS builds remain Developer ID signed and Apple-notarized.

---

# Downloads by platform

## 🍎 macOS

| Download | Apple Silicon (M1+) | Intel |
|---|---|---|
| **Desktop wallet** (dinero-qt) · requires macOS 14+ | [DMG](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/Dinero-v{VERSION}-macOS-arm64.dmg) · [app zip](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/Dinero-v{VERSION}-macOS-arm64-qt.zip) | [DMG](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/Dinero-v{VERSION}-macOS-x86_64.dmg) · [app zip](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/Dinero-v{VERSION}-macOS-x86_64-qt.zip) |
| **Headless daemon + CLI** (operator) · macOS 13+ | [tar.gz](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-operator-v{VERSION}-macOS-arm64.tar.gz) | [tar.gz](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-operator-v{VERSION}-macOS-x86_64.tar.gz) |

> **Intel Mac stuck on macOS 13 (Ventura)?** The Qt wallet needs macOS 14, but the native **DineroDPI** app runs on Ventura: [DineroDPI 1.3.4 — Intel, macOS 13+](https://github.com/DineroLabs/dinero-v8/releases/tag/dinerodpi-v1.3.4-intel). Apple Silicon users can use [DineroDPI 1.3.3](https://github.com/DineroLabs/dinero-v8/releases/tag/dinerodpi-v1.3.3) (macOS 14+).

## 🪟 Windows (x64)

All Windows binaries are Authenticode-signed (publisher: Microsoft-ID-verified).

| Download | |
|---|---|
| **Desktop wallet** (dinero-qt GUI installer) | [Dinero-{VERSION}-Setup.exe](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/Dinero-{VERSION}-windows-x86_64-Setup.exe) |
| **Node installer** (dinerod as a Windows Service) | [Dinero-Server-{VERSION}-Setup.exe](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/Dinero-Server-{VERSION}-windows-x86_64-Setup.exe) |
| **Portable operator zip** (binaries, no installer) | [dinero-v{VERSION}-windows-x86_64-msvc.zip](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-v{VERSION}-windows-x86_64-msvc.zip) |

## 🐧 Linux (x86_64)

| Download | |
|---|---|
| **Desktop wallet** — portable AppImage | [AppImage](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-v{VERSION}-linux-x86_64.AppImage) |
| **Desktop wallet** — Debian/Ubuntu package | [.deb](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-qt-desktop_{VERSION}-1_amd64.deb) |
| **Desktop wallet** — portable Qt tarball | [tar.gz](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-v{VERSION}-linux-x86_64-qt.tar.gz) |
| **Server bundle** (dinerod + CLI + tools) | [tar.gz](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-linux-x86_64-{VERSION}.tar.gz) |
| **One-command node install** (Ubuntu 24.04+) | `curl -fsSL https://dinerolabs.org/install.sh \| sudo bash` |

## 🐳 Docker

```
docker run -d --name dinero -v dinero-data:/data -p 20999:20999 ghcr.io/dinerolabs/dinero-v8:{VERSION}
```

## 📦 Fast-sync snapshots (bundled in installers; standalone for manual setups)

| Height | Files |
|---|---|
| 84,131 (preferred) | [.dat](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-assumeutxo-84131-v4.dat) · [manifest](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-assumeutxo-84131-v4.manifest.json) · [publisher manifest](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-assumeutxo-84131-v4.publisher.manifest.json) · [signature](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-assumeutxo-84131-v4.publisher.manifest.sig) |
| 73,035 (fallback) | [.dat](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-assumeutxo-73035-v4.dat) · [manifest](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/dinero-assumeutxo-73035-v4.manifest.json) |

## 🔐 Checksums

macOS: [combined](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-macos-{VERSION}) · [arm64](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-macos-arm64-{VERSION}) · [arm64 desktop](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-macos-arm64-desktop-{VERSION}) · [x86_64](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-macos-x86_64-{VERSION}) · [x86_64 desktop](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-macos-x86_64-desktop-{VERSION})
Windows: [SHA256SUMS](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-windows-x86_64-{VERSION}) · Linux: [server](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-linux-x86_64-{VERSION}) · [desktop](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-linux-x86_64-desktop-{VERSION}) · Snapshots: [SHA256SUMS](https://github.com/DineroLabs/dinero-v8/releases/download/v{VERSION}/SHA256SUMS-assumeutxo-84131)
