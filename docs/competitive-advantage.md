# Dinero's Competitive Advantage
_Last updated: August 20, 2025_

## Why Dinero feels "professional" out of the box
- **Zero-setup GUI** — Qt frameworks are bundled inside the app; no Homebrew/system Qt required.
- **Hermetic core** — fully vendored static deps (OpenSSL, RocksDB, Snappy/LZ4/Zstd, JsonCpp).
- **Reproducible builds** — deterministic toolchains with `SOURCE_DATE_EPOCH`; stable archives and link order.
- **Enterprise-friendly** — SBOM, checksums, signing, offline install paths, predictable resource use.

## Pragmatic comparison
> Notes: Other projects vary by distro/packager. Dinero's differentiator is *no external GUI deps at runtime* and a *hermetic, reproducible* build story.

| Feature | **Dinero** | Bitcoin Core | Monero | Ethereum (geth) |
|---|---|---|---|---|
| Official GUI | ✅ Yes (bundled) | ✅ Yes | ✅ Yes | ⚠️ Not in geth (3rd-party UIs exist) |
| GUI runtime deps | ✅ **No system deps** (bundled Qt) | Varies by OS | Varies by OS | n/a |
| Fresh OS install | ✅ Works immediately (offline-friendly) | Varies by OS/package | Varies by OS/package | ✅ CLI only |
| Cross-platform binaries | ✅ macOS universal + Linux + Windows | Yes | Yes | Yes (CLI) |
| Reproducible + signed artifacts | ✅ First-class | Varies by release | Varies by release | Varies by client |
| Compression-tuned RocksDB | ✅ Vendored + presets | n/a (LevelDB by default) | n/a | n/a |

## Adoption impact
- **Easier onboarding** — download → run. No brew/apt/yum gymnastics. Works on locked-down machines and with limited connectivity.
- **Enterprise-ready** — IT can deploy without dependency drift; reproducible artifacts + SBOM + signatures; predictable behavior.
- **Global reach** — identical binaries across platforms; documented offline install flows.

## "Backed by code": how we prove it
- **Static core checks**
  - macOS: `otool -L build/bin/dinerod | ! grep -E 'ssl|crypto|rocksdb|snappy|lz4|zstd'` → no third-party dylibs
  - Linux: `ldd build/bin/dinerod | ! grep -E 'ssl|crypto|rocksdb|snappy|lz4|zstd'`
  - Windows: `dumpbin /DEPENDENTS dinerod.exe` shows no vendor DLLs
- **Universal macOS**
  - `lipo -info build/deps/openssl/lib/libcrypto.a` → `arm64 x86_64`
- **GUI no-Homebrew audit**
  - `scripts/macos/audit-app-clean.sh` fails the build if any `/opt/homebrew` paths remain after `macdeployqt`.
- **Reproducibility**
  - CI sets `SOURCE_DATE_EPOCH`, disables ELF build-ids, uses `/Brepro` on MSVC, and byte-compares artifacts from fresh clones.
- **Compliance**
  - We publish release ZIP/DMG/EXE + `SHA256SUMS.txt` + PGP `.asc` + `sbom.json` per tag.

## File layout
- `docs/competitive-advantage.md` ← this page
- `docs/distribution/release-checklist.md` (signing, SBOM, reproducibility, audits)
- `scripts/macos/deploy-qt-app.sh` + `scripts/macos/audit-app-clean.sh`
- `cmake/Repro.cmake` (deterministic flags)
- `docs/rocksdb-compression-guide.md` (tuning presets)

## Bottom line
Dinero delivers **consumer-grade simplicity** and **enterprise-grade discipline**: a GUI that "just runs," a core with **zero external runtime deps**, and releases you can **verify offline**.
