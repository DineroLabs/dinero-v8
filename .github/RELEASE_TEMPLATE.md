<!--
Dinero v8 release notes template — platform-grouped, with CLICKABLE direct-download links.
GitHub's uploaded-asset list is ONE flat list (no native grouping/folders); these notes
are how the release PAGE reads as macOS/Windows/Linux groups while the flat Assets list
stays at the bottom. Steps:
  1. Copy this file, replace every <V> with the version (e.g. 8.0.8) and <H> with the
     AssumeUTXO snapshot height (e.g. 52287).
  2. Fill the 1-line summary + highlights; delete any platform/asset not in this release.
  3. Publish:  gh release edit v<V> --notes-file <thisfile-filled>
Tip: the URLs are https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/<asset>.
-->

**Dinero v<V>** — <one-line summary>. <Fresh installs fast-sync from a bundled height-<H> AssumeUTXO snapshot; existing users unaffected. Note any consensus/daemon change or "byte-identical to previous, no re-deploy".>

**Highlights:** **#<PR>** — <what changed>. **#<PR>** — <what changed>.

> 📥 Direct downloads are grouped by platform below. The full flat **Assets** list (all files together) is at the bottom of this page.

---

## 🍎 macOS — Apple Silicon
**Wallet** — signed + notarized GUI (embedded daemon/CLI/seeder/miners + bundled snapshot):
- ⬇️ [**Dinero-v<V>-macOS-arm64.dmg**](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/Dinero-v<V>-macOS-arm64.dmg) — drag-to-Applications installer
- ⬇️ [Dinero-v<V>-macOS-arm64-qt.zip](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/Dinero-v<V>-macOS-arm64-qt.zip) — same app, zipped

**Operator** (headless): ⬇️ [dinero-operator-v<V>-macOS-arm64.tar.gz](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/dinero-operator-v<V>-macOS-arm64.tar.gz)

🔑 [SHA256SUMS-macos-<V>](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/SHA256SUMS-macos-<V>)

## 🪟 Windows — x86_64
**Wallet** (bundles the snapshot + SV2 miners): ⬇️ [**Dinero-<V>-windows-x86_64-Setup.exe**](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/Dinero-<V>-windows-x86_64-Setup.exe)

**Server / operator** (headless, GPU-free):
- ⬇️ [Dinero-Server-<V>-windows-x86_64-Setup.exe](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/Dinero-Server-<V>-windows-x86_64-Setup.exe)
- ⬇️ [dinero-v<V>-windows-x86_64-msvc.zip](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/dinero-v<V>-windows-x86_64-msvc.zip)

🔑 [SHA256SUMS-windows-<V>](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/SHA256SUMS-windows-<V>)

## 🐧 Linux — x86_64
**Wallet** (desktop) — `dinero-qt` GUI + daemon/CLI/seeder/miners + bundled snapshot:
- ⬇️ [**Dinero-<V>-linux-x86_64-full.tar.gz**](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/Dinero-<V>-linux-x86_64-full.tar.gz) — full desktop bundle; run `./bin/dinero-qt`
- ⬇️ [dinero-qt-<V>-linux-x86_64.tar.gz](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/dinero-qt-<V>-linux-x86_64.tar.gz) — Qt wallet only

**Node / operator** (headless):
- ⬇️ [dinero-core_<V>-1_amd64.deb](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/dinero-core_<V>-1_amd64.deb) — Debian package
- Tarballs: core · cli · seeder · solo-miner · gpu-miner · miner · stratum-worker (`dinero-<name>-<V>-linux-x86_64.tar.gz`)
- One-command install: `curl -fsSL https://dinerolabs.org/install.sh | sudo bash`

🔑 [SHA256SUMS-linux-<V>](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/SHA256SUMS-linux-<V>)

## ⚡ AssumeUTXO snapshot
⬇️ [utxo-snapshot-<H>.dat](https://github.com/DineroLabs/dinero-v8/releases/download/v<V>/utxo-snapshot-<H>.dat) — height-<H> v4 snapshot (UTXO + Utreexo forest + shielded state). Bundled in every desktop installer above; the daemon verifies its SHA256 against the compiled-in trust anchor.

---
Verify any download against the matching `SHA256SUMS-*` file: `sha256sum -c SHA256SUMS-<platform>-<V>`
