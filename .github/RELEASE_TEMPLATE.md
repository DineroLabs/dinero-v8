<!--
Dinero v8 release notes template — platform-grouped.
GitHub shows the uploaded assets as ONE flat list (no native grouping); these
notes are how the release page reads cleanly. Copy this, replace <VERSION>
(e.g. 8.0.8) and the 1-line highlights, delete any platform not in this release,
and publish with:  gh release edit v<VERSION> --notes-file <thisfile-filled>
-->

**Dinero v<VERSION>** — <one-line summary>. <Fresh installs fast-sync from a bundled height-<H> AssumeUTXO snapshot; existing users unaffected. Note any consensus/daemon change or "byte-identical to previous, no re-deploy".>

**Highlights:** <PR ####> — <what changed>. <PR ####> — <what changed>.

---

## 🍎 macOS — Apple Silicon
**Wallet** — signed + notarized GUI; embedded daemon, CLI, seeder, miners + bundled snapshot:
- `Dinero-v<VERSION>-macOS-arm64.dmg` — drag-to-Applications installer
- `Dinero-v<VERSION>-macOS-arm64-qt.zip` — same app, zipped

**Operator** — headless node: `dinero-operator-v<VERSION>-macOS-arm64.tar.gz`

Checksums: `SHA256SUMS-macos-<VERSION>`

## 🪟 Windows — x86_64
**Wallet** — bundles the snapshot + SV2 miners: `Dinero-<VERSION>-windows-x86_64-Setup.exe`

**Server / operator** — headless, GPU-free:
- `Dinero-Server-<VERSION>-windows-x86_64-Setup.exe`
- `dinero-v<VERSION>-windows-x86_64-msvc.zip`

Checksums: `SHA256SUMS-windows-<VERSION>`

## 🐧 Linux — x86_64
**Wallet** (desktop) — `dinero-qt` GUI + daemon/CLI/seeder/miners + bundled snapshot:
- `Dinero-<VERSION>-linux-x86_64-full.tar.gz` — full desktop bundle; run `./bin/dinero-qt`
- `dinero-qt-<VERSION>-linux-x86_64.tar.gz` — Qt wallet only

**Node / operator** (headless):
- `dinero-core_<VERSION>-1_amd64.deb` — Debian package
- Tarballs: `dinero-core` (daemon), `dinero-cli`, `dinero-seeder`, `dinero-solo-miner`, `dinero-gpu-miner`, `dinero-miner`, `dinero-stratum-worker` (`-<VERSION>-linux-x86_64.tar.gz`)
- One-command install: `curl -fsSL https://dinerolabs.org/install.sh | sudo bash`

Checksums: `SHA256SUMS-linux-<VERSION>`

## ⚡ AssumeUTXO snapshot
- `utxo-snapshot-<H>.dat` — height-<H> v4 snapshot (UTXO + Utreexo forest + shielded state). Bundled in every desktop installer; the daemon verifies its SHA256 against the compiled-in trust anchor.

---
Verify any download against the matching `SHA256SUMS-*` file: `sha256sum -c SHA256SUMS-<platform>-<VERSION>`
