# Cross-Platform CI Pipeline for v3.0.0-alpha1

**Date:** 2026-01-11
**Purpose:** Multi-platform daemon builds for v3.0.0-alpha1 pre-release
**Scope:** macOS (arm64/x86_64), Linux (x86_64), Windows (x86_64)

---

## Executive Summary

This document defines the CI pipeline for building and verifying DineroCoin v3.0.0-alpha1 binaries across all supported platforms. The pipeline ensures:

- ✅ **Reproducible builds** (hermetic where possible)
- ✅ **Cross-platform compatibility** (macOS, Linux, Windows)
- ✅ **Automated testing** (unit, integration, smoke tests)
- ✅ **Release-grade artifacts** (static linking, no Homebrew deps)
- ✅ **Binary verification** (checksums, signatures, dependency checks)

---

## Pipeline Architecture

### Workflow Triggers

```yaml
on:
  push:
    tags:
      - 'v3.0.0-alpha*'
      - 'v3.0.0-beta*'
      - 'v3.0.0-rc*'
      - 'v3.*'
  workflow_dispatch:
    inputs:
      version:
        description: 'Version to build (e.g., v3.0.0-alpha1)'
        required: true
        type: string
```

**Trigger Conditions:**
1. **Tag push:** Automatic on version tags
2. **Manual dispatch:** For testing or re-builds

---

## Build Matrix

### Platform Support

| Platform | Runner | Architectures | Static Linking | Tests Enabled |
|----------|--------|---------------|----------------|---------------|
| **macOS** | `macos-14` (M1) | arm64, x86_64 | ✅ Yes | ✅ Yes |
| **Linux** | `ubuntu-22.04` | x86_64 | ✅ Yes | ✅ Yes |
| **Windows** | `windows-2022` | x86_64 | ⚠️ Partial | ✅ Yes |

### Build Configuration

```yaml
strategy:
  fail-fast: false
  matrix:
    include:
      # macOS (Apple Silicon)
      - os: macos-14
        platform: macos
        arch: arm64
        cmake_flags: >-
          -DDINERO_RELEASE=ON
          -DCMAKE_BUILD_TYPE=Release
          -DCMAKE_OSX_ARCHITECTURES=arm64
          -DENABLE_TESTS=ON
          -DENABLE_GPU_MINING=OFF

      # macOS (Intel)
      - os: macos-13
        platform: macos
        arch: x86_64
        cmake_flags: >-
          -DDINERO_RELEASE=ON
          -DCMAKE_BUILD_TYPE=Release
          -DCMAKE_OSX_ARCHITECTURES=x86_64
          -DENABLE_TESTS=ON
          -DENABLE_GPU_MINING=OFF

      # Linux (x86_64)
      - os: ubuntu-22.04
        platform: linux
        arch: x86_64
        cmake_flags: >-
          -DDINERO_RELEASE=ON
          -DCMAKE_BUILD_TYPE=Release
          -DENABLE_TESTS=ON
          -DENABLE_GPU_MINING=ON

      # Windows (x86_64)
      - os: windows-2022
        platform: windows
        arch: x86_64
        cmake_flags: >-
          -DDINERO_RELEASE=ON
          -DCMAKE_BUILD_TYPE=Release
          -DENABLE_TESTS=ON
          -DENABLE_GPU_MINING=OFF
```

---

## Pipeline Stages

### Stage 1: Environment Setup

**Purpose:** Install build dependencies

#### macOS
```bash
brew install cmake openssl boost sqlite qt@6
# Note: gRPC/protobuf NOT linked in DINERO_RELEASE=ON mode
```

#### Linux
```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  libssl-dev \
  libboost-all-dev \
  libsqlite3-dev \
  libcurl4-openssl-dev \
  qt6-base-dev \
  qt6-tools-dev \
  ninja-build
```

#### Windows
```powershell
choco install cmake ninja visualstudio2022buildtools -y
```

---

### Stage 2: Checkout & Submodules

```yaml
- name: Checkout code
  uses: actions/checkout@v4
  with:
    submodules: recursive
    fetch-depth: 0  # Full history for git describe
```

**Critical:** Submodules must be initialized (RocksDB, secp256k1-zkp, etc.)

---

### Stage 3: CMake Configuration

```bash
# Set SOURCE_DATE_EPOCH for reproducible builds
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)

cmake -S . -B build \
  -DDINERO_RELEASE=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTS=ON \
  ${{ matrix.cmake_flags }}
```

**Verification:**
- ✅ DINERO_RELEASE=ON is set (enforces static linking)
- ✅ ENABLE_TESTS=ON (tests will run)
- ✅ SOURCE_DATE_EPOCH set (reproducible timestamps)

---

### Stage 4: Build Binaries

```bash
cmake --build build --config Release --parallel $(nproc)
```

**Build Targets:**
- `dinerod` - Daemon (required)
- `dinero-cli` - RPC client (required)
- `dinero-qt` - GUI wallet (optional, macOS/Linux)
- `lightningd` - Lightning daemon (optional)
- Test executables (all tests)

**Expected Outputs:**
```
build/dinerod
build/dinero-cli
build/dinero-qt (macOS/Linux only)
build/lightningd (optional)
build/tests/...
```

---

### Stage 5: Run Tests

```yaml
- name: Run unit tests
  run: ctest --test-dir build -R "Ring" --output-on-failure

- name: Run consensus tests
  run: ctest --test-dir build -R "consensus" --output-on-failure

- name: Run mobile tests
  run: ctest --test-dir build -R "mobile" --output-on-failure

- name: Run Lightning tests
  run: ctest --test-dir build -R "lightning" --output-on-failure
```

**Test Categories:**
- **Ring tests** (formal verification, 65 tests)
- **Consensus tests** (stateless validation, Utreexo)
- **Mobile tests** (iOS pressure, burst mode)
- **Lightning tests** (channel validation, watchtower)
- **Wallet tests** (encryption, HD derivation)

**Failure Handling:**
- Any test failure **blocks the release**
- Test output saved to artifacts
- Developers notified via GitHub Actions

---

### Stage 6: Smoke Tests

**Purpose:** Basic functionality verification before packaging

```bash
# Start daemon in regtest mode
./build/dinerod -regtest -daemon

# Wait for startup
sleep 5

# Smoke test: RPC connectivity
./build/dinero-cli -regtest getblockchaininfo

# Smoke test: Generate blocks
./build/dinero-cli -regtest generatetoaddress 10 <address>

# Smoke test: Wallet operations
./build/dinero-cli -regtest getnewaddress
./build/dinero-cli -regtest getbalance

# Shutdown
./build/dinero-cli -regtest stop
```

**Success Criteria:**
- ✅ Daemon starts successfully
- ✅ RPC commands respond
- ✅ Blocks can be generated
- ✅ Wallet operations work
- ✅ Daemon shuts down cleanly

---

### Stage 7: Binary Verification

**Purpose:** Ensure release-grade quality (no Homebrew deps, static linking)

```bash
# Verify binary has no Homebrew dependencies (macOS)
./scripts/verify_release_binary.sh build/dinerod

# Verify no gRPC/protobuf/abseil linkage (Linux)
ldd build/dinerod | grep -E "grpc|protobuf|absl" && exit 1 || exit 0

# Verify binary signature (Windows)
signtool verify /pa build/dinerod.exe
```

**macOS Verification:**
```bash
otool -L build/dinerod | tee deps.txt

# Expected: Only system libraries
# - /usr/lib/libSystem.B.dylib
# - /usr/lib/libc++.1.dylib
# - /usr/lib/libz.1.dylib

# NOT expected:
# - /opt/homebrew/... (any Homebrew path)
# - libgrpc, libprotobuf, libabsl
```

**Linux Verification:**
```bash
ldd build/dinerod | tee deps.txt

# Expected: Only system libraries
# - linux-vdso.so.1
# - libc.so.6
# - libpthread.so.0
# - libdl.so.2
# - libm.so.6
# - ld-linux-x86-64.so.2

# NOT expected:
# - libgrpc, libprotobuf, libabsl
```

---

### Stage 8: Package Artifacts

**Purpose:** Create distributable archives with checksums

```bash
# Use existing build script
RELEASE_VERSION=${{ github.ref_name }} ./scripts/build-release-node.sh
```

**Artifacts Created:**
```
dist/
├── DineroCoin-v3.0.0-alpha1-macos-arm64.tar.gz
├── DineroCoin-v3.0.0-alpha1-macos-arm64.tar.gz.sha256
├── DineroCoin-v3.0.0-alpha1-linux-x86_64.tar.gz
├── DineroCoin-v3.0.0-alpha1-linux-x86_64.tar.gz.sha256
├── DineroCoin-v3.0.0-alpha1-windows-x86_64.zip
├── DineroCoin-v3.0.0-alpha1-windows-x86_64.zip.sha256
├── SHA256SUMS
├── SHA256SUMS.asc (GPG signature)
└── BUILD_ATTESTATION.json
```

**Archive Contents:**
```
DineroCoin-v3.0.0-alpha1-<platform>-<arch>/
├── bin/
│   ├── dinerod
│   ├── dinero-cli
│   └── lightningd (optional)
├── dinero-qt.app/ (macOS GUI)
├── README.md
├── INSTALL.md
├── CHANGELOG.md
├── EXECUTIVE_SUMMARY.md
├── EXCHANGE_DUE_DILIGENCE.md
└── LICENSE
```

---

### Stage 9: Upload Artifacts

```yaml
- name: Upload build artifacts
  uses: actions/upload-artifact@v4
  with:
    name: dinero-${{ matrix.platform }}-${{ matrix.arch }}-${{ github.ref_name }}
    path: |
      dist/*.tar.gz
      dist/*.zip
      dist/*.sha256
      dist/BUILD_ATTESTATION.json
      dist/SHA256SUMS*
    retention-days: 90
```

---

### Stage 10: Create GitHub Release

**Purpose:** Publish pre-release to GitHub

```yaml
- name: Create GitHub Release
  uses: softprops/action-gh-release@v1
  with:
    files: |
      dist/*.tar.gz
      dist/*.zip
      dist/*.sha256
      dist/SHA256SUMS*
      dist/BUILD_ATTESTATION.json
    draft: false
    prerelease: true  # Mark as pre-release for alpha
    body: |
      # DineroCoin ${{ github.ref_name }}

      **⚠️ PRE-RELEASE: Alpha Version**

      This is an alpha release of DineroCoin v3.0.
      - ✅ Protocol complete (Stateless, Lightning, Mobile)
      - ⚠️ No external security audit
      - ⚠️ Recommended for testnet use only

      ## What's New
      - Stateless validation (Utreexo)
      - Proof network (cache, routing, gossip)
      - Lightning integration (read-only client)
      - Mobile profile (compiler-enforced)

      ## Downloads
      - **macOS (arm64):** DineroCoin-${{ github.ref_name }}-macos-arm64.tar.gz
      - **macOS (x86_64):** DineroCoin-${{ github.ref_name }}-macos-x86_64.tar.gz
      - **Linux (x86_64):** DineroCoin-${{ github.ref_name }}-linux-x86_64.tar.gz
      - **Windows (x86_64):** DineroCoin-${{ github.ref_name }}-windows-x86_64.zip

      ## Verification
      ```bash
      shasum -a 256 -c DineroCoin-*.tar.gz.sha256
      ```

      ## Security Notice
      This release has NOT been externally audited. Use at your own risk.

      ## Support
      - Issues: https://github.com/Trucker2827/Dinero-Coin/issues
      - Security: security@dinero-coin.com
  env:
    GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
```

---

## Platform-Specific Considerations

### macOS

**Challenges:**
- Apple Silicon (arm64) vs Intel (x86_64) builds
- Code signing requirements (not enforced for alpha)
- Homebrew dependencies must be avoided

**Solutions:**
- Use `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64`
- Static link all non-system libraries
- Verify with `otool -L`

**Future:** Universal binaries (arm64 + x86_64) for beta/rc

### Linux

**Challenges:**
- glibc version compatibility
- Distribution-specific paths

**Solutions:**
- Use Ubuntu 22.04 (widely compatible glibc)
- Static link where possible
- Verify with `ldd`

**Future:** AppImage for broader compatibility

### Windows

**Challenges:**
- MSVC build system
- DLL dependencies
- Path separators

**Solutions:**
- Use Ninja generator (`-G Ninja`)
- Bundle runtime DLLs in release
- PowerShell build script (`build-release-node.ps1`)

**Future:** Installer (MSI or NSIS)

---

## CI Performance Optimization

### Parallel Builds

```yaml
cmake --build build --parallel $(nproc)
```

**Expected Times:**
- macOS (M1): 5-8 minutes
- Linux (x86_64): 8-12 minutes
- Windows (x86_64): 12-18 minutes

### Caching

```yaml
- name: Cache dependencies
  uses: actions/cache@v3
  with:
    path: |
      ~/.cache/vcpkg
      ~/.cache/cargo
      build/_deps
    key: ${{ runner.os }}-${{ hashFiles('**/CMakeLists.txt') }}
```

**Speedup:** 30-50% faster on cache hit

---

## Smoke Test Specification

### Test 1: Daemon Startup
```bash
./dinerod -regtest -daemon
sleep 5
ps aux | grep dinerod
```
**Expected:** Process running

### Test 2: RPC Connectivity
```bash
./dinero-cli -regtest getblockchaininfo
```
**Expected:** JSON response with chain info

### Test 3: Block Generation
```bash
./dinero-cli -regtest generatetoaddress 10 $(./dinero-cli -regtest getnewaddress)
```
**Expected:** 10 block hashes returned

### Test 4: Wallet Operations
```bash
./dinero-cli -regtest getnewaddress
./dinero-cli -regtest getbalance
./dinero-cli -regtest listunspent
```
**Expected:** Address generated, balance shown, UTXOs listed

### Test 5: Daemon Shutdown
```bash
./dinero-cli -regtest stop
sleep 3
ps aux | grep dinerod
```
**Expected:** Process terminated cleanly

---

## Failure Handling

### Build Failures

**Detection:** CMake or make returns non-zero exit code

**Actions:**
1. Save build log to artifacts
2. Annotate GitHub Actions run with error
3. Notify developers via GitHub notifications
4. Block release creation

### Test Failures

**Detection:** CTest reports failed tests

**Actions:**
1. Save test output to artifacts (`--output-on-failure`)
2. Annotate failed tests in GitHub UI
3. Block release creation
4. Generate test failure report

### Verification Failures

**Detection:** Binary verification script returns non-zero

**Actions:**
1. Save dependency analysis to artifacts
2. List problematic dependencies
3. Block release creation
4. Generate verification failure report

---

## Release Checklist

### Pre-Release
- [ ] All tests pass (Ring, consensus, mobile, Lightning, wallet)
- [ ] Smoke tests pass on all platforms
- [ ] Binary verification passes (no Homebrew deps)
- [ ] Archives created with checksums
- [ ] BUILD_ATTESTATION.json generated

### Release Creation
- [ ] GitHub release created
- [ ] Marked as **pre-release** (alpha)
- [ ] Security notice included
- [ ] Download links verified
- [ ] SHA256SUMS file attached

### Post-Release
- [ ] Release announcement posted
- [ ] Documentation updated
- [ ] Testnet deployment initiated
- [ ] Community feedback collection started

---

## Monitoring & Observability

### Build Metrics

Track in GitHub Actions:
- Build duration (per platform)
- Test execution time
- Artifact size
- Cache hit rate

### Success Criteria

**Green Build:**
- ✅ All platforms build successfully
- ✅ All tests pass (100%)
- ✅ All smoke tests pass
- ✅ Binary verification passes
- ✅ Artifacts uploaded

**Red Build:**
- ❌ Any build failure
- ❌ Any test failure
- ❌ Binary verification failure
- ❌ Artifact creation failure

---

## Implementation Plan

### Phase 1: Workflow Creation (Day 1)
- [ ] Create `.github/workflows/v3-alpha-release.yml`
- [ ] Define build matrix
- [ ] Add smoke test step
- [ ] Enable test execution (`-DENABLE_TESTS=ON`)

### Phase 2: Testing (Day 1-2)
- [ ] Test workflow with `workflow_dispatch`
- [ ] Verify builds on all platforms
- [ ] Validate artifact structure
- [ ] Fix any platform-specific issues

### Phase 3: Integration (Day 2)
- [ ] Merge workflow to main
- [ ] Tag v3.0.0-alpha1
- [ ] Verify automatic trigger
- [ ] Monitor GitHub Actions run

### Phase 4: Release (Day 2-3)
- [ ] Verify all artifacts created
- [ ] Download and test binaries locally
- [ ] Publish GitHub release
- [ ] Announce to community

---

## Future Improvements

### For Beta/RC
- Universal macOS binaries (arm64 + x86_64)
- Windows code signing
- Linux AppImage
- Docker images
- Reproducible build verification

### For v3.0.0 Final
- Automated release notes generation
- Performance benchmarks in CI
- Security scanning (SAST, dependency check)
- Multi-sig release signatures
- Torrent distribution

---

## Conclusion

This CI pipeline provides a **production-ready framework** for building and distributing DineroCoin v3.0.0-alpha1 across all major platforms. It ensures:

- ✅ **Quality:** All tests pass before release
- ✅ **Security:** Binary verification prevents dependency issues
- ✅ **Reliability:** Smoke tests catch runtime issues
- ✅ **Transparency:** Full build logs and attestation
- ✅ **Reproducibility:** Hermetic builds with SOURCE_DATE_EPOCH

**Next Step:** Implement workflow and run first alpha build.

---

**Document Date:** 2026-01-11
**Author:** Claude Code
**Status:** Ready for implementation
