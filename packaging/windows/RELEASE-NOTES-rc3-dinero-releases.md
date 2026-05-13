## What's new in rc3 vs rc2

This release adds the **native-MSVC daemon stack** for Windows x64, completing the native-MSVC port across the full Dinero-Coin tree. rc2's caveat — *"Porting each [vendored dep] to MSVC is genuinely multi-week work"* — is now resolved: `dinerod`, `dinero-cli`, and four miner / wallet tools build cleanly under MSVC 14.44 against the project's own vendored OpenSSL static libraries, with no MinGW runtime DLLs.

All previous-rc assets remain valid. For a complete Windows install:

| Binary | Use the release |
|---|---|
| `dinerod`, `dinero-cli`, `dinero-miner`, `dinero-stratum-worker`, `dinero-gpu-miner`, `dinero-wallet-cli` | **this rc3** |
| `dinero-solo-miner` (with CUDA backend) | [rc2](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc2) |
| `dinero-qt`, Stratum V2 binaries, Linux aarch64 | [rc1](https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.6-rc1) |

### dinero-v2.2.6-rc3-windows-x86_64-msvc.zip

- **Toolchain**: native MSVC (Visual Studio 2022 Build Tools, MSVC 14.44.35207). Matches rc2's toolchain choice.
- **Vendored OpenSSL**: built statically from `third_party/openssl-3.3.2/` via the new `scripts/build-openssl-vendored.ps1` (`VC-WIN64A no-shared no-tests no-apps enable-ec enable-ecdh enable-ecdsa`). No system or vcpkg OpenSSL dependency. rc2's [Bug #1 in NATIVE-MSVC-PORT-BUGS.md](https://github.com/DineroLabs/Dinero-Coin/blob/dinero-main/NATIVE-MSVC-PORT-BUGS.md) is now resolved.
- **Runtime DLL strategy**: static. No MinGW DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) and no MSVC runtime DLLs need to ship — `/MT` static linkage throughout.
- **Source**: [Dinero-Coin@feeea5c0a](https://github.com/DineroLabs/Dinero-Coin/commit/feeea5c0a) on branch `dinero-main`.

### Included binaries (`bin/`)

| Binary | Purpose | Size |
|---|---|---|
| `dinerod.exe` | Full node daemon | ~16 MB |
| `dinero-cli.exe` | CLI RPC client | ~190 KB |
| `dinero-miner.exe` | CPU miner | ~180 KB |
| `dinero-stratum-worker.exe` | Stratum worker client | ~180 KB |
| `dinero-gpu-miner.exe` | GPU miner (OpenCL; **runtime unvalidated on Windows**) | ~180 KB |
| `dinero-wallet-cli.exe` | Reference wallet CLI | ~5 MB |

### Verification on the MSVC build

- **Regtest end-to-end smoke**: `dinerod -regtest` startup → cookie auth → `getblockchaininfo` → `generatetoaddress 1 <addr>` → height advanced 0→1 → `stop` clean shutdown. Genesis hash `0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f` matches the frozen value across every platform.
- **Bit-identical consensus output**: 13/14 golden-vector tests pass byte-for-byte against the frozen test corpus (`GenesisInvariants`, `BlockValidationInvariants`, `DAAGoldenVectors`, `MerkleInvariants`, `WitnessMerkleIsolation`, `FilterCommitmentActivation`, `ShieldedV030Vectors`, `UtreexoEphemeral`, `UtreexoSafetyGate`, `TestnetInvariantsVerification`, `ConsensusValidationTripwires`, `test_header_hash_vectors`, `MerkleGolden`). The remaining failure (`SupplyInvariantTest.PropertySupplyFormulaCorrectness_AllEpochs`) is a stale test-side formula that fails identically on every platform — the test omits the genesis coinbase from its predicted-supply formula.
- **ctest sweep**: 270 tests pass on the MSVC build. 79 POSIX-only `.sh` integration scripts are cleanly excluded on Windows (they still run on Linux / Mac / MinGW). Remaining failures match the same set on Linux.

### Notable platform-independent fix shipped here

The MSVC port surfaced a real production bug in the legacy wallet.conf parser:

- **`fix(wallet)`**: `MigrateLegacySidecarToStateDb` now strips trailing `\r` from CRLF-line-ended wallet.conf files. Previously the migration aborted with *"wallet.conf is corrupted (seed_hex is invalid or wrong length)"* against any wallet.conf produced on Windows or edited in Notepad / Notepad++ / VS Code with default line endings. Affects every platform, not just Windows.

### Verifying the download

```powershell
# PowerShell:
Get-FileHash dinero-v2.2.6-rc3-windows-x86_64-msvc.zip -Algorithm SHA256
# Expected: 0c0aebf56e406a3d3cd476855b0f087068279a4b9def007a59d5e266cc388a47
```

```bash
# POSIX:
sha256sum dinero-v2.2.6-rc3-windows-x86_64-msvc.zip
# Inside the archive, full per-file manifest:
sha256sum -c SHA256SUMS.txt
```

### Known limitations on Windows

- **`dinero-gpu-miner.exe`** ships but its OpenCL runtime path is not validated on Windows yet. Treat as experimental until a Windows GPU hashrate baseline is published.
- **No NSIS installer** in this rc — distribution is the raw ZIP. An installer wrapping the same `bin/` payload is planned for GA.
- **Code signing** — binaries are not yet code-signed. Windows SmartScreen may warn on first run; verify SHA256 manually against the values above.

### Quick start

```powershell
Expand-Archive dinero-v2.2.6-rc3-windows-x86_64-msvc.zip
cd dinero-v2.2.6-rc3-windows-x86_64-msvc\bin
.\dinerod.exe                       # mainnet, default datadir %APPDATA%\dinero
.\dinerod.exe -regtest               # local regression test network
.\dinero-cli.exe getblockchaininfo   # query a running daemon
```

### Reproducing this build from source

```powershell
# One-time per machine: install Strawberry Perl (https://strawberryperl.com)
# and Visual Studio 2022 Build Tools (C++ x86/x64 workload).
git clone https://github.com/DineroLabs/Dinero-Coin
cd Dinero-Coin

# 1) Build vendored OpenSSL (~7 min; auto-locates VS Build Tools).
.\scripts\build-openssl-vendored.ps1

# 2) Configure and build the daemon stack.
cmake -S . -B build-msvc-native -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc-native --config Release `
    --target dinerod dinero-cli dinero-miner `
             dinero-stratum-worker dinero-gpu-miner dinero-wallet-cli

# 3) Package.
.\packaging\windows\package-daemon-stack.ps1
```
