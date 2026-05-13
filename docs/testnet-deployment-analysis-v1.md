# Dinero — Testnet Deployment Analysis (v1)

**Date:** 2025‑08‑20
**Owner:** Core Engineering
**Scope:** Readiness of Dinero for public **testnet** launch; build provenance, network parameters, security posture, validation plan, and rollout.

---

## 1) Executive Summary

* ✅ **Universal static builds** across Windows/macOS/Linux (OpenSSL 3.3.1, RocksDB 9.1.0 + Snappy/LZ4/Zstd, JsonCpp all vendored)
* ✅ **Modern crypto**: EVP‑based ECDSA/secp256k1 + 1.1.x compatibility path, compressed keys preferred.
* ✅ **Network hygiene**: HRP fixed per network (`tdin` testnet, `rdin` regtest). Default ports: testnet **20998/21000**, regtest **20996/21001**.
* ✅ **Clean linkage**: `dinerod` shows no dynamic dependency on system libs (100% static vendored).
* 🚧 **Follow‑ups** called out in §10.

---

## 2) Build Provenance & Reproducibility

**Toolchains**

* macOS: AppleClang 17.x (arm64/x86\_64)
* Linux: GCC 11+/Clang 15+ (x86\_64, aarch64)

**Flags & knobs**

* **Unix**: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF`
* **Windows**: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G "NMake Makefiles"`
* **Vendored deps**: OpenSSL 3.3.1 static, RocksDB 9.1.0 static, JsonCpp from source.
* **Windows CRT**: Static `/MT` for maximum portability.
* **LTO**: optional `-DENABLE_LTO=ON` (verify with `cmake --build . --target check-lto`).

**Determinism checks**

* Strip symbol timestamps via `SOURCE_DATE_EPOCH` in CI.
* Rebuild twice and compare shasums: `shasum -a256 dinerod*` (macOS), `sha256sum` (Linux).
* Confirm no accidental rpaths: `otool -l dinerod | grep -A2 LC_RPATH` (macOS), `readelf -d dinerod | grep RPATH` (Linux).

**Universal static linkage verification**

* macOS: `otool -L build/bin/dinerod` → expect **no** external deps (OpenSSL/RocksDB vendored).
* Linux: `ldd build/bin/dinerod` → **not** listing OpenSSL/RocksDB .so; only glibc/libstdc++.
* Windows: `dumpbin /dependents dinerod.exe` → **no** OpenSSL/RocksDB DLLs.

---

## 3) Cryptography Posture

* **EVP everywhere**: keygen, sign, verify via `EVP_PKEY` + `EVP_PKEY_fromdata`/`EVP_DigestSign*`/`EVP_DigestVerify*`.
* **Curve**: `secp256k1` (NID).
* **Digests**: SHA‑256, RIPEMD‑160 through `EVP_MD_fetch` → fallback to `EVP_sha256()`/`EVP_ripemd160()` when provider not used.
* **Compressed pubkeys** default; bech32 encoding aligns with active network HRP.
* **Compat path** compiled for OpenSSL < 3.0 (guarded by `OPENSSL_VERSION_NUMBER`).

**Runtime sanity**

* On boot, log `OpenSSL_version(OPENSSL_VERSION)` and provider info (if available) to confirm 3.x path.

---

## 4) Network Parameters (Testnet)

* **HRP**: `tdin` (bech32).
* **Ports**: RPC **20998**, P2P **21000**.
* **Cookie auth**: `${datadir}/testnet/.cookie`.
* **Founder control store**: `${datadir}/testnet/blockchain_data_founder_control`.

> **Observation**: In startup logs, the **premine dev fund address** printed as `rdin…` on testnet. Ensure **testnet uses `tdin…`** for founder control or a network‑specific mapping. See §7.1.

---

## 5) RPC & Wallet

* RPC binds `127.0.0.1` by default; cookie auth enforced.
* Mining: auto‑generates network‑correct bech32 address.
* Wallet DB lives under `${datadir}/testnet/blockchain_data/wallet` (static JsonCpp).

**Quick RPC smoke**

```bash
./dinerod -testnet -server=1 -datadir=/tmp/dinero-tn -daemon
./dinero-cli -testnet getblockchaininfo
./dinero-cli -testnet getnewaddress
./dinero-cli -testnet generatetoaddress 1 $(./dinero-cli -testnet getnewaddress)
./dinero-cli -testnet getwalletinfo
```

---

## 6) P2P & Bootstrapping

* SimpleP2P runs on 21000; add public testnet seed nodes when available.
* Node discovery: ensure DNS seeds support testnet label, or ship bootstrap list.

**Basic connectivity checks**

* `getnetworkinfo` reports `connections` > 0 when seeds are live.
* Firewalls: open 21000/TCP on seed nodes.

---

## 7) Validation & Test Plan

### 7.1 Founder Control (network‑specific)

**Goal:** founder control enforces premine only to **testnet** address (`tdin…`).

**Checks**

1. Inspect `${datadir}/testnet/blockchain_data_founder_control/*.json` → address **must** start with `tdin`.
2. Startup log must show `Premine dev fund address: tdin…` (not `rdin…`).
3. Attempt invalid premine redirection → should be rejected with clear log + error code.

**Remediation if needed**

* Map addresses by network (enum → HRP/address).
* Or accept `-founderaddr=<addr>` per network at startup; validate HRP vs. active network.

### 7.2 Core Smoke (automatable)

* **Genesis & premine**: heights 0/1 exist; hashes persisted; UTXO created.
* **Address round‑trip**: `getnewaddress` → decode → encode → equals.
* **Mempool**: send small tx; appears in mempool; mined within `generatetoaddress`.
* **Reorg handling**: mine two branches on isolated nodes; re‑connect; best‑chain chosen by work.
* **Persistence**: restart daemon → chainstate/UTXO intact.

### 7.3 Crypto Path

* Force EVP 3.x path at runtime; verify `DigestSign`/`Verify` success for test vectors.
* Negative tests: signature tamper ⇒ verify fails.

### 7.4 Port/HRP Guards

* Asserts that `-testnet` ⇒ HRP `tdin`, ports 20998/21000 bound; `-regtest` ⇒ `rdin` + 20996/21001.

---

## 8) Release Artifacts & Provenance

* **Targets**: `dinerod`, `dinero-cli`, `dinero-tx`.
* **Platforms**: 
  - **Windows**: x64/x86 (MSVC, static CRT `/MT`)
  - **macOS**: arm64/x86\_64 (universal binary optional)
  - **Linux**: x86\_64/aarch64 (glibc 2.17+)
* **Static**: link OpenSSL 3.3.1/RocksDB/JsonCpp statically across all platforms.
* **Deliverables**: platform-specific archives + `SHA256SUMS` + `SHA256SUMS.asc` (GPG).
* **SBOM**: generate CycloneDX (`-DENABLE_SBOM=ON`) and attach to release.

**Signing**

* macOS app notarization not required for CLI; consider Apple code signing for fewer warnings.
* GPG‑sign `SHA256SUMS` with release key.

---

## 9) Monitoring & Ops (Testnet)

* **Logs**: structured INFO/WARN/ERROR; rotate at 50MB default.
* **Metrics (future)**: export Prometheus counters for mempool size, peer count, tip height, orphan rate.
* **SLOs**: seed uptime ≥ 99.5%, new block relay < 2s (p50) on testnet.

---

## 10) Risks & Mitigations

* **OpenSSL 3.x migration**: keep CI matrix building against 1.1.1 (compat) and 3.3.x; unit tests cover both paths.
* **Founder HRP drift**: add startup assert (active HRP must match founder address HRP).
* **Static RocksDB portability**: build with `PORTABLE=1`; run on older glibc via containerized build.
* **Seed node availability**: deploy multiple seeds across regions; health‑checks.

---

## 11) Rollout Plan

1. **Week 0 (internal)**: run §7 test plan on 2–3 dev machines; fix founder HRP if present.
2. **Week 1 (closed testnet)**: publish binaries to early testers; provide bootstrap peers; start faucet.
3. **Week 2 (public testnet)**: announce on social media; documentation live; explorer operational.
4. **Week 4+ (stabilization)**: gather feedback; iterate on UX; prepare mainnet parameters.

---

## 12) Success Metrics

* **Technical**: 0 critical bugs in first 2 weeks; >95% uptime on seed nodes.
* **Adoption**: >10 external nodes joining testnet; >100 transactions processed.
* **Community**: Positive feedback on build quality and documentation clarity.

---

## Appendix A: Build Commands

### Unix (macOS/Linux)
```bash
# Clean build from scratch
rm -rf build && mkdir build && cd build

# Configure with vendored dependencies (no Homebrew/system packages)
cmake .. -DCMAKE_BUILD_TYPE=Release -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON -DBUILD_SHARED_LIBS=OFF

# Build daemon and CLI (uses Ninja if available)
cmake --build . --parallel

# Verify no external dependencies
# macOS: Should show only system frameworks
otool -L bin/dinerod | grep -v /usr/lib | grep -v /System || echo "✅ Clean static build"
# Linux: Should show only glibc/libstdc++
ldd bin/dinerod | grep -E "(rocksdb|ssl|crypto)" && echo "❌ External deps found" || echo "✅ Clean static build"

# Quick smoke test
./bin/dinerod -testnet -server=1 -datadir=/tmp/dinero-testnet-smoke -daemon
sleep 2
./bin/dinero-cli -testnet -datadir=/tmp/dinero-testnet-smoke getblockcount
./bin/dinero-cli -testnet -datadir=/tmp/dinero-testnet-smoke stop
```

### Windows (MSVC)
```cmd
REM Prerequisites: Visual Studio, Strawberry Perl, NASM
REM Use Visual Studio Developer Command Prompt

REM Clean build
rmdir /s build 2>nul
mkdir build && cd build

REM Configure with vendored dependencies
cmake .. -DCMAKE_BUILD_TYPE=Release -G "NMake Makefiles" -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON

REM Build daemon and CLI
nmake

REM Verify no external dependencies
dumpbin /dependents bin\dinerod.exe

REM Quick smoke test
bin\dinerod.exe -testnet -server=1 -datadir=C:\temp\dinero-testnet-smoke -daemon
timeout 2
bin\dinero-cli.exe -testnet -datadir=C:\temp\dinero-testnet-smoke getblockcount
bin\dinero-cli.exe -testnet -datadir=C:\temp\dinero-testnet-smoke stop
```

## Appendix B: Network Constants

| Network | HRP    | RPC Port | P2P Port | Magic Bytes  |
|---------|--------|----------|----------|--------------|
| Mainnet | `din`  | 20998    | 20999    | `0xD1E2F3A5` |
| Testnet | `tdin` | 20998    | 21000    | `0xD1E2F3A4` |
| Regtest | `rdin` | 20996    | 21001    | `0xDAB5BFFA` |

---

*This analysis reflects the state as of August 20, 2025. Update as implementation evolves.*
