# Vendored Dependencies Guide

DineroCoin uses **vendored dependencies** (embedded in `📁 third_party/`) to ensure:
- ✅ **Reproducible builds** across all platforms
- ✅ **No system package conflicts**
- ✅ **Portable binaries** that work everywhere
- ✅ **Offline builds** without network access

---

## 📦 Vendored Libraries (15 total)

All these libraries are included and built automatically:

| Library            | Version | Purpose                          |
|--------------------|---------|----------------------------------|
| **jsoncpp**        | 1.9.6   | JSON parsing (RPC, config)       |
| **OpenSSL**        | 3.3.2   | TLS/crypto (Lightning, wallet)   |
| **RocksDB**        | 9.1.1   | Blockchain database              |
| **secp256k1**      | latest  | ECDSA & Schnorr signatures       |
| **Argon2**         | latest  | Wallet encryption (KDF)          |
| **BIP39**          | latest  | Mnemonic seed phrases            |
| **Boost**          | 1.85.0  | C++ utilities                    |
| **msgpack-c**      | latest  | Lightning serialization          |
| **nlohmann/json**  | latest  | JSON utilities                   |
| **LZ4**            | 1.9.4   | Fast compression                 |
| **Snappy**         | latest  | Compression (RocksDB)            |
| **Zstd**           | latest  | Compression                      |
| **SQLite**         | 3.48.0  | Local database                   |
| **hidapi**         | 0.14.0  | Hardware wallet USB              |
| **libusb**         | 1.0.27  | Hardware wallet USB              |

---

## 📁 Directory Structure

After cloning the repository, your `📁 third_party/` should look like this:

```
third_party/
├── argon2/
├── bip39/
├── boost_1_85_0/
├── hidapi/
├── jsoncpp/
├── libusb-1.0.27/
├── lz4-1.9.4/
├── msgpack-c/
├── nlohmann/
├── openssl-3.3.2/
│   ├── libcrypto.a      # Built by build-openssl-vendored.sh
│   └── libssl.a         # Built by build-openssl-vendored.sh
├── rocksdb-9.1.1/
├── secp256k1/
├── snappy/
├── sqlite-amalgamation-3480000/
└── zstd/
```

✅ **Verification:** If `📁 third_party/` has these directories, your clone is complete.

---

## 🔒 Version Pinning Policy

All vendored libraries are **pinned to known-good commits or releases**.

Updates follow this policy:

| Priority | Trigger                    | Action                                      |
|----------|----------------------------|---------------------------------------------|
| 🚨 **P0** | Security or CVE patches   | Immediate bump + emergency release          |
| ⚡ **P1** | Minor or patch releases   | After integration tests pass                |
| 🧪 **P2** | Major version changes     | Reviewed under `deps/upgrade/` branch       |

**Validation:** All version updates are validated via:
- ✅ Reproducible builds (hash check in CI)
- ✅ Backward compatibility tests
- ✅ Cross-platform verification (macOS, Linux, Windows)

**Upgrade process:**
```bash
# 1. Create upgrade branch
git checkout -b deps/upgrade/openssl-3.4.0

# 2. Update version in CMakeLists.txt
# 3. Rebuild and test
./scripts/build-openssl-vendored.sh
cmake -B build -S . && cmake --build build -j$(nproc)

# 4. Run full test suite
ctest --test-dir build --output-on-failure

# 5. Create PR with changelog
```

---

## 🔧 Setup Instructions

### Quick Start (macOS & Linux)

```bash
# 1. Build vendored OpenSSL (required first time)
./scripts/build-openssl-vendored.sh

# 2. Configure and build DineroCoin
cmake -B build -S .
cmake --build build -j$(nproc)
```

That's it! All other dependencies build automatically.

### Superbuild (Optional)

For advanced CI/CD integration, you can build **all dependencies** with a single CMake command:

```bash
cmake -S . -B build -DENABLE_SUPERBUILD=ON
```

This triggers sub-builds for jsoncpp, RocksDB, secp256k1, etc. in parallel.

**Note:** Superbuild is **optional** — the default workflow already handles dependency builds correctly.

---

## OpenSSL Vendoring Details

### Why Vendor OpenSSL?

OpenSSL is **critical** for DineroCoin's security:
- Lightning Network encryption
- Wallet encryption (AES-256-GCM)
- TLS connections
- Cryptographic operations

**Problem:** System OpenSSL versions vary wildly:
- Ubuntu 20.04: OpenSSL 1.1.1
- Ubuntu 22.04: OpenSSL 3.0.2
- Fedora 40: OpenSSL 3.2.1
- macOS: OpenSSL 3.5+ (Homebrew)

**Solution:** Vendor OpenSSL 3.3.2 for consistency.

### Building Vendored OpenSSL

The script `scripts/build-openssl-vendored.sh` handles everything:

```bash
./scripts/build-openssl-vendored.sh
```

**What it does:**
1. Detects your platform (macOS/Linux, x86_64/ARM64)
2. Configures OpenSSL for static linking
3. Builds `libcrypto.a` and `libssl.a`
4. Places libraries in `📁 third_party/openssl-3.3.2/`

**Build time:** 2-5 minutes (one-time setup)

**Output:**
```
✅ OpenSSL built successfully!

Libraries created:
  libcrypto.a: 7.3M
  libssl.a: 1.5M
```

### Using System OpenSSL (Optional)

If you prefer system OpenSSL (e.g., for distro packaging):

```bash
cmake -B build -S . -DUSE_SYSTEM_OPENSSL=ON
```

This uses your system's OpenSSL via `find_package()`.

**Not recommended for production** - binaries won't be portable.

---

## jsoncpp Vendoring

**jsoncpp** was recently modernized from 208 → 70 lines of CMake:
- ✅ Removed deprecated policy warnings
- ✅ Modern CMake (3.10-3.29)
- ✅ Clean build output

No build steps needed - builds automatically with DineroCoin.

---

## Platform-Specific Notes

### macOS

All dependencies are vendored and built automatically:
- ✅ OpenSSL (pre-built in repo)
- ✅ No Homebrew dependencies required
- ✅ Universal binaries (Intel + Apple Silicon)

### Linux

Most dependencies are vendored:
- ✅ OpenSSL (build with script)
- ✅ No apt/dnf/yum packages required
- ✅ Portable binaries

**First-time setup:**
```bash
# Build OpenSSL once
./scripts/build-openssl-vendored.sh

# Then build normally
cmake -B build -S .
cmake --build build -j$(nproc)
```

### Windows

OpenSSL vendoring is **TODO** (currently uses vcpkg).

---

## 📜 License Summary

All vendored dependencies use permissive, GPL-compatible licenses:

| Library            | License              | Notes                              |
|--------------------|----------------------|------------------------------------|
| **jsoncpp**        | MIT                  | Attribution required in NOTICE     |
| **OpenSSL**        | Apache 2.0 + SSLeay  | Compatible with commercial use     |
| **RocksDB**        | Apache 2.0           | Facebook/Meta OSS                  |
| **secp256k1**      | MIT                  | Bitcoin Core library               |
| **Argon2**         | Apache 2.0 / CC0     | Public domain variant available    |
| **BIP39**          | MIT                  | -                                  |
| **Boost**          | BSL-1.0              | Permissive, no attribution needed  |
| **msgpack-c**      | Boost / Apache 2.0   | Dual-licensed                      |
| **nlohmann/json**  | MIT                  | -                                  |
| **LZ4**            | BSD-2-Clause         | -                                  |
| **Snappy**         | BSD-3-Clause         | Google                             |
| **Zstd**           | BSD-3-Clause / GPLv2 | Dual-licensed, we use BSD variant  |
| **SQLite**         | Public Domain        | No restrictions                    |
| **hidapi**         | BSD-3-Clause / GPLv3 | We use BSD variant                 |
| **libusb**         | LGPL 2.1+            | Dynamic linking permitted          |

**Compliance:** Full license texts are in `📁 third_party/<library>/LICENSE`.

**Redistribution:** When distributing DineroCoin binaries:
- ✅ Include `NOTICE` file with attributions
- ✅ Provide source code access (GPL compliance for libusb)
- ✅ Respect trademark policies (e.g., Bitcoin logo usage)

---

## Troubleshooting

### Error: "Vendored OpenSSL libraries not found"

**Solution:**
```bash
./scripts/build-openssl-vendored.sh
```

Or use system OpenSSL temporarily:
```bash
cmake -B build -S . -DUSE_SYSTEM_OPENSSL=ON
```

### Error: CMake policy warnings

**Solution:** Your jsoncpp is outdated. Re-pull from git:
```bash
git pull origin main
```

The modernized jsoncpp (Nov 2025) eliminates all warnings.

### Error: RocksDB build fails with "too many sections"

**macOS solution:** This is a ranlib archive limit. Fixed automatically in CMakeLists.txt (line 71-73).

**Linux solution:** Ensure you have enough disk space and RAM (8GB+ recommended).

---

## CI/CD Integration

### GitHub Actions Example

For automated builds (GitHub Actions, GitLab CI, etc.):

```yaml
# .github/workflows/build.yml
name: Build DineroCoin

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive  # Fetch vendored dependencies

      - name: Build vendored OpenSSL
        run: ./scripts/build-openssl-vendored.sh

      - name: Configure CMake
        run: cmake -B build -S .

      - name: Build DineroCoin
        run: cmake --build build -j$(nproc)

      - name: Run tests
        run: ctest --test-dir build --output-on-failure
```

This ensures **reproducible builds** in CI.

### Docker Example

```dockerfile
FROM ubuntu:22.04

# Install build tools only (no library dependencies!)
RUN apt-get update && apt-get install -y \
    build-essential cmake git perl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Build vendored OpenSSL
RUN ./scripts/build-openssl-vendored.sh

# Build DineroCoin
RUN cmake -B build -S . && cmake --build build -j$(nproc)
```

**No apt-get for OpenSSL, RocksDB, etc.** — fully vendored! 🎉

---

## 🛠️ Maintenance Command Cheatsheet

| Task                          | Command                                                                 |
|-------------------------------|-------------------------------------------------------------------------|
| **Clean build**               | `rm -rf build && cmake -B build -S . && cmake --build build -j$(nproc)` |
| **Rebuild OpenSSL**           | `cd third_party/openssl-3.3.2 && make clean && cd - && ./scripts/build-openssl-vendored.sh` |
| **Update all submodules**     | `git submodule update --init --recursive`                               |
| **Run tests**                 | `ctest --test-dir build --output-on-failure`                            |
| **Check library sizes**       | `du -sh third_party/*/lib*.a`                                           |
| **Use system OpenSSL**        | `cmake -B build -S . -DUSE_SYSTEM_OPENSSL=ON`                           |
| **Build with all warnings**   | `cmake -B build -S . -DCMAKE_CXX_FLAGS="-Wall -Wextra" && cmake --build build` |
| **Clean rebuild from scratch**| `git clean -fdx && ./scripts/build-openssl-vendored.sh && cmake -B build -S . && cmake --build build -j$(nproc)` |
| **Package for distribution** | `cmake --build build --target package`                                  |

---

## Benefits Summary

| Benefit               | Description                                    |
|-----------------------|------------------------------------------------|
| **Reproducibility**   | Same libraries, same versions, everywhere      |
| **No conflicts**      | Isolated from system packages                  |
| **Portability**       | Binaries work on any Linux distro              |
| **Offline builds**    | No network required after clone                |
| **Security**          | Known, audited versions                        |
| **Simplicity**        | One script, zero dependencies                  |
| **CI/CD friendly**    | Deterministic builds in containers             |
| **Cross-platform**    | macOS, Linux, Windows (coming soon)            |

---

## Questions?

- **Want to vendor more libraries?** Follow the jsoncpp/OpenSSL pattern in CMakeLists.txt
- **Need system packages?** Use `-DUSE_SYSTEM_<LIBRARY>=ON` flags
- **Having issues?** Check `CMakeLists.txt` for options or open an issue
- **License questions?** See the License Summary table above

**Last updated:** November 2025 (OpenSSL vendoring complete, jsoncpp modernized)

---

## ⚡ Lightning Network Dependencies

DineroCoin's Lightning implementation has **specialized dependency requirements**.

**Current Lightning stack:**
- ✅ **6/9 recommended libraries** fully vendored (80% BOLT spec coverage)
- ⚠️ **3 additional libraries recommended** for production Lightning

**See detailed analysis:** [`LIGHTNING_DEPENDENCIES.md`](LIGHTNING_DEPENDENCIES.md)

**Quick summary:**

| Component | Status | Priority |
|-----------|--------|----------|
| bech32 | ✅ **Vendored** (`external/bech32/`) | - |
| msgpack-c | ✅ **Vendored** | - |
| secp256k1 (base) | ✅ **Vendored** | - |
| OpenSSL 3.3.2 | ✅ **Vendored** | - |
| RocksDB | ✅ **Vendored** | - |
| SQLite | ✅ **Vendored** | - |
| **libwally-core** | ⚠️ **Recommended** | 🚨 P0 (PSBT, BOLT #3) |
| **secp256k1-zkp** | ⚠️ **Recommended** | ⚡ P1 (MuSig2, BOLT #12) |
| **blake3** | ⚠️ **Optional** | 🧪 P2 (performance) |

**To add Lightning dependencies:**
```bash
# Priority 0: BOLT3 compliance
./scripts/vendor-libwally.sh

# Priority 1: Advanced features
./scripts/vendor-secp256k1-zkp.sh

# Priority 2: Performance
./scripts/vendor-blake3.sh
```

---

## Contributing

When adding new vendored dependencies:

1. ✅ Add to `📁 third_party/` directory
2. ✅ Update this document with version and license
3. ✅ Add to CMakeLists.txt with `EXCLUDE_FROM_ALL`
4. ✅ Create build script if complex (like OpenSSL, libwally)
5. ✅ Test on macOS, Linux, and Windows
6. ✅ Update CI/CD workflows
7. ✅ For Lightning deps, also update `LIGHTNING_DEPENDENCIES.md`

**Example PR title:** `vendor: Add libwally-core 1.3.0 for Lightning PSBT support`
