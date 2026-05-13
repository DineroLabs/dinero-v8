# Dinero v2.2.6-rc2-msvc1 — Windows x86_64 (MSVC)

First native MSVC build of the Dinero daemon stack for Windows x86_64.
The existing Windows pipeline used MinGW-w64; this release ships
binaries built directly with Microsoft Visual Studio 2022, linked
against the project's own vendored OpenSSL static libraries instead
of a system or vcpkg copy.

## Included binaries (`bin/`)

| Binary | Purpose |
|---|---|
| `dinerod.exe` | Full node daemon |
| `dinero-cli.exe` | CLI RPC client |
| `dinero-miner.exe` | CPU miner |
| `dinero-stratum-worker.exe` | Stratum worker client |
| `dinero-gpu-miner.exe` | GPU miner (OpenCL; **runtime unvalidated on Windows**) |
| `dinero-wallet-cli.exe` | Reference wallet CLI |

## Shipped separately (not in this archive)

These ship from other repos / subprojects, matching how the Linux releases handle them:

- **`dinero-qt`** — Qt6 GUI wallet, separate subproject build (Linux releases ship it bundled; the Windows Qt6 deployment story is tracked separately).
- **`dinero-solo-miner`** — [DineroLabs/dinero-solo-miner](https://github.com/DineroLabs/dinero-solo-miner) (a Windows MSVC build already exists in that repo).
- **`dinero-sv2-miner`** / **`dinero-sv2-gpu-miner`** — [DineroLabs/dinero-sv2](https://github.com/DineroLabs/dinero-sv2) (Linux/WSL2 validated at 643 MH/s; native Windows MSVC not yet validated).
- **`dinero-stratum`** (server) — separate binary, not part of Dinero-Coin's CMake.

## Build provenance

- **Toolchain**: Microsoft Visual Studio Build Tools 2022, MSVC 14.44 (`cl.exe` v19.44.35207)
- **Architecture**: x86_64
- **OpenSSL**: vendored, statically linked (`no-shared no-tests no-apps enable-ec enable-ecdh enable-ecdsa`), built with the new `scripts/build-openssl-vendored.ps1`
- **Runtime DLL strategy**: static — no DLL deployment required at install
- **Tested on**: Windows 11 (build 26200)

## Verification

This release passed the project's existing consensus correctness gates on the MSVC build:

- **Regtest end-to-end smoke**: daemon up → cookie auth → `getblockchaininfo` → `generatetoaddress 1 <addr>` → block accepted at height 1 → `stop` clean shutdown. Genesis hash `0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f`, post-mine block `0a3fb2d807a992bffde82f62dc3b7c2af03a9acdfab533de6cc7f97fb519d74f` — both deterministic and platform-independent.
- **Bit-identical consensus output**: 13/14 golden-vector tests (GenesisInvariants, BlockValidationInvariants, DAAGoldenVectors, MerkleInvariants, WitnessMerkleIsolation, FilterCommitmentActivation, ShieldedV030Vectors, UtreexoEphemeral, UtreexoSafetyGate, TestnetInvariantsVerification, ConsensusValidationTripwires, test_header_hash_vectors, MerkleGolden) match byte-for-byte against the frozen test corpus. No hash, merkle root, chainwork, or nullifier diverges.
- **ctest sweep**: 270 of the buildable tests pass; 79 POSIX-only `.sh` integration tests are cleanly excluded on Windows (they still run on Linux/Mac/MinGW); 7 failures + 2 exceptions + 1 timeout are all pre-existing deferrals tracked in the bug log, none consensus-blocking.

The one non-passing consensus-formal-verification subtest (`SupplyInvariantTest.PropertySupplyFormulaCorrectness_AllEpochs`) fails identically on every platform — the test's predicted supply formula is off by 50 DIN (omits the genesis coinbase). It is a stale test-side formula, not a consensus output divergence.

## Notable fixes shipped in this release

The Windows MSVC port surfaced a real production bug not previously visible on Linux/macOS:

- **`fix(wallet)`**: `MigrateLegacySidecarToStateDb` now strips trailing `\r` from CRLF-line-ended `wallet.conf` files. The parser previously crashed with "wallet.conf is corrupted (seed_hex is invalid or wrong length)" against any wallet.conf produced on Windows or edited in Notepad / Notepad++ / VS Code with default line endings. **Platform-independent fix** — also benefits Linux users editing their wallet.conf on Windows.

Other portability fixes (less impactful but ship in this archive):

- `fix(miner)`: dropped `std::string_view` keys in JsonCpp `Value::operator[]` calls in `dinero-miner` and `dinero-gpu-miner` (Linux libstdc++ tolerated an implicit conversion MSVC correctly refuses).
- `build(msvc)`: vendored OpenSSL now builds on Windows via PowerShell (`scripts/build-openssl-vendored.ps1`); 30+ previously-unbuilt test targets now build; POSIX-only test suites cleanly gated; `.sh` regtest scripts cleanly excluded from the Windows ctest gate.

## Quick start

```powershell
# Extract the archive
Expand-Archive dinero-v2.2.6-rc2-msvc1-windows-x86_64-msvc.zip

# Verify against the GitHub release's SHA256 (or check SHA256SUMS.txt inside)
Get-FileHash dinero-v2.2.6-rc2-msvc1-windows-x86_64-msvc.zip -Algorithm SHA256

# Run the daemon (mainnet, default datadir %APPDATA%\dinero)
cd dinero-v2.2.6-rc2-msvc1-windows-x86_64-msvc\bin
.\dinerod.exe

# Or regtest for local testing
.\dinerod.exe -regtest

# In a separate shell
.\dinero-cli.exe getblockchaininfo
```

## Verifying the archive

Per-file hashes are in `SHA256SUMS.txt` at the archive root:

```powershell
# Quick spot-check on PowerShell:
Get-FileHash bin\dinerod.exe -Algorithm SHA256

# Or full verification on POSIX:
sha256sum -c SHA256SUMS.txt
```

The archive itself has its own SHA256 listed below — verify it matches before extracting:

```
SHA256 (dinero-v2.2.6-rc2-msvc1-windows-x86_64-msvc.zip) =
  0c0aebf56e406a3d3cd476855b0f087068279a4b9def007a59d5e266cc388a47
```

## Known limitations on Windows

- **`dinero-gpu-miner.exe`** ships but its OpenCL runtime path is not validated on Windows yet. The binary loads but kernel selection / device enumeration may behave differently from Linux; treat as experimental until a Windows GPU hashrate baseline is published.
- **No NSIS installer** in this rc — distribution is the raw ZIP. An installer wrapping the same `bin/` payload is planned for the GA release.
- **Code signing** — binaries are not yet code-signed. Windows SmartScreen may warn on first run; either accept the warning or verify SHA256 manually against the release page.

## Source

- **Commit**: [`5fec759d2`](https://github.com/DineroLabs/Dinero-Coin/commit/5fec759d2)
- **Branch**: `dinero-main`
- **Repository**: https://github.com/DineroLabs/Dinero-Coin

## Reproducing this build

```powershell
# One-time per machine: install Strawberry Perl (https://strawberryperl.com)
# and Visual Studio 2022 Build Tools (with the C++ x86/x64 workload).

git clone https://github.com/DineroLabs/Dinero-Coin
cd Dinero-Coin

# 1) Build the vendored OpenSSL (~7 min). Auto-locates VS Build Tools.
.\scripts\build-openssl-vendored.ps1

# 2) Configure and build the daemon stack.
cmake -S . -B build-msvc-native -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc-native --config Release `
    --target dinerod dinero-cli dinero-miner `
             dinero-stratum-worker dinero-gpu-miner dinero-wallet-cli

# 3) Package.
.\packaging\windows\package-daemon-stack.ps1
```
