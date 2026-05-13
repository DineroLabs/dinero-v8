# 🔒 DineroCoin Security & Release Engineering Guide

## Comprehensive Security Hardening and Release Process

This document provides enterprise-grade security hardening guidelines and release engineering processes for DineroCoin production deployment.

---

## 📋 Table of Contents

1. [Security Hardening](#security-hardening)
2. [Release Engineering](#release-engineering)
3. [Code Signing](#code-signing)
4. [Supply Chain Security](#supply-chain-security)
5. [Incident Response](#incident-response)

---

## 🔐 PACKAGE 5: Security Hardening Guide

### 1. Memory Safety

**Threat**: Memory leaks can expose private keys, blinding factors, and view keys.

#### Zeroization

**Implementation**:
```cpp
// secure_allocator.h
#include <sodium.h>

template<typename T>
class SecureAllocator {
public:
    void deallocate(T* p, std::size_t n) {
        sodium_memzero(p, n * sizeof(T));
        std::allocator<T>().deallocate(p, n);
    }
};

// Usage
std::vector<uint8_t, SecureAllocator<uint8_t>> private_key(32);
// Automatically zero-ed when destroyed
```

**C++17 Approach**:
```cpp
#include <openssl/crypto.h>

void SecureZeroMemory(void* ptr, size_t len) {
    OPENSSL_cleanse(ptr, len);
}

// Usage
std::vector<uint8_t> blinding_factor(32);
// When done:
SecureZeroMemory(blinding_factor.data(), 32);
blinding_factor.clear();
```

#### Locked Memory (mlock)

**Prevent swapping sensitive data to disk**:
```cpp
#include <sys/mman.h>

bool LockMemory(void* addr, size_t len) {
#ifdef _WIN32
    return VirtualLock(addr, len) != 0;
#else
    return mlock(addr, len) == 0;
#endif
}

bool UnlockMemory(void* addr, size_t len) {
#ifdef _WIN32
    return VirtualUnlock(addr, len) != 0;
#else
    return munlock(addr, len) == 0;
#endif
}

// Usage
std::vector<uint8_t> view_key(32);
LockMemory(view_key.data(), 32);
// Use view key...
UnlockMemory(view_key.data(), 32);
SecureZeroMemory(view_key.data(), 32);
```

#### Checklist

- [x] **Zeroize blinding factors after use** - ✅ IMPLEMENTED
- [x] **Zeroize shared secrets after ECDH** - ✅ IMPLEMENTED
- [ ] **Use locked memory (mlock) for keys** - ⚠️ TODO
- [ ] **Disable core dumps** - ⚠️ TODO
- [ ] **Use Address Sanitizer in CI** - ⚠️ TODO

---

### 2. Network Safety

**Threat**: DDoS attacks, spam, malicious peers

#### Rate Limiting

**RPC Rate Limiting**:
```cpp
// rpc_limiter.h
#include <unordered_map>
#include <chrono>
#include <mutex>

class RPCRateLimiter {
public:
    bool Allow(const std::string& ip, int max_requests_per_hour) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::system_clock::now();
        auto& entry = requests_[ip];

        // Clean old entries
        auto hour_ago = now - std::chrono::hours(1);
        entry.erase(
            std::remove_if(entry.begin(), entry.end(),
                [hour_ago](auto& t) { return t < hour_ago; }),
            entry.end()
        );

        // Check limit
        if (entry.size() >= max_requests_per_hour) {
            return false; // Rate limited
        }

        entry.push_back(now);
        return true;
    }

private:
    std::unordered_map<std::string, std::vector<std::chrono::system_clock::time_point>> requests_;
    std::mutex mutex_;
};
```

**Usage**:
```cpp
RPCRateLimiter limiter;

bool HandleRPCRequest(const std::string& ip, const std::string& method) {
    // Write operations: 10/hour
    if (method == "sendtoaddress" || method == "sendconfidential") {
        if (!limiter.Allow(ip, 10)) {
            return false; // Too many requests
        }
    }

    // Read operations: 100/hour
    if (!limiter.Allow(ip, 100)) {
        return false;
    }

    // Process request...
    return true;
}
```

#### RPC Authentication

**Configuration** (`dinero.conf`):
```conf
# REQUIRED: RPC authentication
rpcuser=dinero_admin
rpcpassword=CHANGE_THIS_TO_STRONG_PASSWORD

# REQUIRED: IP whitelist
rpcallowip=127.0.0.1
rpcallowip=10.0.0.0/8

# REQUIRED: RPC port (non-standard)
rpcport=18332

# OPTIONAL: RPC SSL
rpcssl=true
rpcsslcertificatechainfile=/path/to/cert.pem
rpcsslprivatekeyfile=/path/to/key.pem

# OPTIONAL: Maximum upload size (prevent large blob uploads)
rpcmaxuploadsize=1048576
```

#### Peer Management

**Max Peers**:
```cpp
// net.h
constexpr int MAX_OUTBOUND_CONNECTIONS = 8;
constexpr int MAX_INBOUND_CONNECTIONS = 117;
constexpr int MAX_TOTAL_CONNECTIONS = 125;
```

**Handshake Timeout**:
```cpp
// net_processing.cpp
constexpr int HANDSHAKE_TIMEOUT_SECONDS = 60;

void CheckHandshakeTimeout(CNode* node) {
    if (!node->fSuccessfullyConnected &&
        GetTime() - node->nTimeConnected > HANDSHAKE_TIMEOUT_SECONDS) {
        node->fDisconnect = true;
        LogPrint(BCLog::NET, "Handshake timeout peer=%d\n", node->GetId());
    }
}
```

#### Checklist

- [ ] **Rate-limit RPC calls** - ⚠️ IMPLEMENT
- [ ] **Enforce authentication for write RPC** - ⚠️ VERIFY
- [ ] **Add max upload targets** - ⚠️ TODO
- [ ] **Add max peers limit** - ✅ DONE (default: 125)
- [ ] **Add handshake timeouts** - ⚠️ TODO

---

### 3. Wallet Safety

**Threat**: Unauthorized access, brute-force attacks

#### Encryption

**Argon2id Key Derivation**:
```cpp
#include <argon2.h>

std::vector<uint8_t> DeriveKey(
    const std::string& password,
    const std::vector<uint8_t>& salt
) {
    std::vector<uint8_t> derived_key(32);

    argon2id_hash_raw(
        3,                          // t_cost (iterations)
        65536,                      // m_cost (memory in KB)
        4,                          // parallelism
        password.c_str(),
        password.size(),
        salt.data(),
        salt.size(),
        derived_key.data(),
        32
    );

    return derived_key;
}
```

**AES-256-GCM Encryption**:
```cpp
#include <openssl/evp.h>

std::vector<uint8_t> EncryptWallet(
    const std::vector<uint8_t>& plaintext,
    const std::vector<uint8_t>& key
) {
    std::vector<uint8_t> iv(12);      // 96-bit IV
    std::vector<uint8_t> tag(16);     // 128-bit tag
    std::vector<uint8_t> ciphertext(plaintext.size() + 28); // IV + tag + ciphertext

    // Generate random IV
    RAND_bytes(iv.data(), 12);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key.data(), iv.data());

    int len;
    EVP_EncryptUpdate(ctx, ciphertext.data() + 28, &len, plaintext.data(), plaintext.size());

    int ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + 28 + len, &len);
    ciphertext_len += len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data());

    // Prepend IV and tag
    memcpy(ciphertext.data(), iv.data(), 12);
    memcpy(ciphertext.data() + 12, tag.data(), 16);

    EVP_CIPHER_CTX_free(ctx);

    return ciphertext;
}
```

#### Passphrase Time Lock

**Prevent brute-force**:
```cpp
class PassphraseTimeLock {
public:
    bool CheckPassphrase(const std::string& passphrase) {
        if (IsLocked()) {
            return false; // Still locked from previous attempts
        }

        bool correct = VerifyPassphrase(passphrase);

        if (!correct) {
            failed_attempts_++;

            if (failed_attempts_ >= 3) {
                lockout_until_ = std::chrono::system_clock::now() +
                                std::chrono::minutes(5);
                failed_attempts_ = 0;
            }
        } else {
            failed_attempts_ = 0;
        }

        return correct;
    }

private:
    bool IsLocked() {
        return std::chrono::system_clock::now() < lockout_until_;
    }

    int failed_attempts_ = 0;
    std::chrono::system_clock::time_point lockout_until_;
};
```

#### Checklist

- [x] **Encrypt wallet.dat with Argon2id** - ✅ DONE
- [x] **Add key stretching** - ✅ DONE (Argon2id)
- [ ] **Add passphrase time lock** - ⚠️ TODO
- [ ] **Auto-lock after inactivity** - ✅ DONE (15 min default)
- [ ] **Require password for sends** - ⚠️ TODO

---

### 4. FFI Safety

**Threat**: Buffer overflows, NULL pointer dereferences in Rust FFI

#### Buffer Safety

**BEFORE (unsafe)**:
```cpp
// ❌ UNSAFE
int bp_generate(
    uint8_t* proof_out,
    size_t* proof_len,
    uint64_t value,
    const uint8_t* blind
) {
    // No bounds checking!
    memcpy(proof_out, generated_proof.data(), generated_proof.size());
}
```

**AFTER (safe)**:
```cpp
// ✅ SAFE
int bp_generate(
    uint8_t* proof_out,
    size_t* proof_len_out,
    size_t proof_buffer_size,  // Add buffer size parameter
    uint64_t value,
    const uint8_t* blind
) {
    if (!proof_out || !proof_len_out || !blind) {
        return -1; // NULL pointer check
    }

    if (proof_buffer_size < BULLETPROOFS_MAX_PROOF_SIZE) {
        return -2; // Buffer too small
    }

    // Generate proof...

    if (generated_proof.size() > proof_buffer_size) {
        return -3; // Proof larger than buffer
    }

    memcpy(proof_out, generated_proof.data(), generated_proof.size());
    *proof_len_out = generated_proof.size();

    return 0; // Success
}
```

#### Return Value Checking

**ALWAYS check returns**:
```cpp
// ❌ WRONG
int result = bp_generate(proof.data(), &proof_len, value, blind.data());
// Assuming success...

// ✅ CORRECT
int result = bp_generate(proof.data(), &proof_len, BULLETPROOFS_MAX_PROOF_SIZE, value, blind.data());
if (result != 0) {
    throw std::runtime_error("Bulletproof generation failed: " + std::to_string(result));
}
```

#### Checklist

- [x] **No unbounded buffers** - ✅ VERIFIED
- [x] **No NULL pointers** - ✅ VERIFIED
- [x] **Always check bp_generate return values** - ✅ VERIFIED
- [ ] **Fuzzing on FFI boundary** - ⚠️ TODO

---

### 5. Supply Chain Safety

**Threat**: Malicious dependencies, compromised builds

#### Vendor Dependencies

**Vendor Bulletproofs Rust**:
```bash
# Copy Bulletproofs into project
mkdir -p third_party/bulletproofs_ffi
cp -r /path/to/bulletproofs/* third_party/bulletproofs_ffi/

# Update Cargo.toml
# [dependencies]
# bulletproofs = { path = "../bulletproofs_ffi" }
```

**Vendor secp256k1**:
```bash
# Git submodule
git submodule add https://github.com/bitcoin-core/secp256k1.git third_party/secp256k1
git submodule update --init --recursive
```

#### Dependency Verification

```bash
# Verify checksums
sha256sum third_party/secp256k1/src/secp256k1.c
# Compare with known-good hash
```

#### Code Signing Certificates

**macOS**:
```bash
# Request Developer ID certificate from Apple
# Install in Keychain

# Sign application
codesign --deep --force --verify --verbose \
  --sign "Developer ID Application: Your Name (TEAM_ID)" \
  --options runtime \
  DineroCoin.app

# Notarize
xcrun altool --notarize-app \
  --primary-bundle-id "org.dinerocoin.wallet" \
  --username "your@email.com" \
  --password "@keychain:AC_PASSWORD" \
  --file DineroCoin.dmg
```

**Windows**:
```powershell
# Get EV Code Signing certificate
# Sign with signtool
signtool sign /f cert.pfx /p password /t http://timestamp.digicert.com DineroCoin.exe
```

#### Checklist

- [ ] **Vendor Bulletproofs Rust directory** - ⚠️ TODO
- [x] **Vendor secp256k1** - ✅ DONE (git submodule)
- [ ] **Turn on code signing** - ⚠️ TODO
- [ ] **Verify dependency checksums** - ⚠️ TODO
- [ ] **Pin dependency versions** - ⚠️ TODO

---

## 📦 PACKAGE 6: Release Engineering & Code Signing

### 1. Build Targets

| Platform | Format | Signing |
|----------|--------|---------|
| **macOS** | .dmg | Developer ID + Notarization |
| **Windows** | .exe | Authenticode |
| **Linux** | AppImage | GPG |
| **Linux** | .tar.gz | GPG |
| **Source** | .tar.gz | GPG |

---

### 2. Deterministic Builds (Gitian)

**Why?** Same source → same binary hash on all machines

#### Setup

```bash
# Install Gitian
git clone https://github.com/devrandom/gitian-builder.git
cd gitian-builder

# Create base VM
bin/make-base-vm --suite focal --arch amd64

# Build
bin/gbuild ../dinero/contrib/gitian-descriptors/gitian-linux.yml
```

#### Gitian Descriptor (`gitian-linux.yml`):
```yaml
name: "dinero-linux"
enable_cache: true
suites:
- "focal"
architectures:
- "amd64"
packages:
- "g++"
- "cmake"
- "ninja-build"
- "libssl-dev"
- "libsodium-dev"
remotes:
- "url": "https://github.com/dinerocoin/dinero.git"
  "dir": "dinero"
files: []
script: |
  WRAP_DIR=$HOME/wrapped
  HOSTS="x86_64-linux-gnu"

  cd dinero
  mkdir build && cd build
  cmake -GNinja -DCMAKE_BUILD_TYPE=Release ..
  ninja
  ninja package

  mkdir -p $OUTDIR/src
  cp dinero-*.tar.gz $OUTDIR/src/
```

---

### 3. Code Signing

#### macOS

```bash
#!/bin/bash
# sign-macos.sh

APP_NAME="DineroCoin"
BUNDLE_ID="org.dinerocoin.wallet"
DEVELOPER_ID="Developer ID Application: DineroCoin Team (TEAM_ID)"

# Build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(sysctl -n hw.ncpu)

# Sign
codesign --deep --force --verify --verbose \
  --sign "$DEVELOPER_ID" \
  --options runtime \
  --entitlements entitlements.plist \
  "$APP_NAME.app"

# Create DMG
hdiutil create -volname "$APP_NAME" \
  -srcfolder "$APP_NAME.app" \
  -ov -format UDZO \
  "$APP_NAME.dmg"

# Sign DMG
codesign --sign "$DEVELOPER_ID" "$APP_NAME.dmg"

# Notarize
xcrun altool --notarize-app \
  --primary-bundle-id "$BUNDLE_ID" \
  --username "team@dinero-coin.com" \
  --password "@keychain:NOTARIZATION_PASSWORD" \
  --file "$APP_NAME.dmg"

# Wait for notarization...

# Staple
xcrun stapler staple "$APP_NAME.dmg"
```

#### Windows

```powershell
# sign-windows.ps1

$CERT_PATH = "cert.pfx"
$CERT_PASSWORD = $env:CERT_PASSWORD
$TIMESTAMP_SERVER = "http://timestamp.digicert.com"

# Build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release

# Sign EXE
signtool sign `
  /f $CERT_PATH `
  /p $CERT_PASSWORD `
  /t $TIMESTAMP_SERVER `
  /d "DineroCoin Wallet" `
  DineroCoin.exe

# Create installer
makensis installer.nsi

# Sign installer
signtool sign `
  /f $CERT_PATH `
  /p $CERT_PASSWORD `
  /t $TIMESTAMP_SERVER `
  DineroCoin-Setup.exe
```

#### Linux

```bash
#!/bin/bash
# sign-linux.sh

VERSION="1.0.0"
TARBALL="dinero-$VERSION-linux-x86_64.tar.gz"

# Build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Package
make package

# Generate checksums
sha256sum $TARBALL > SHA256SUMS

# GPG sign
gpg --detach-sign --armor SHA256SUMS
```

---

### 4. Release Pipeline (GitHub Actions)

```yaml
# .github/workflows/release.yml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  build-linux:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v3

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake ninja-build

      - name: Build
        run: |
          mkdir build && cd build
          cmake -GNinja -DCMAKE_BUILD_TYPE=Release ..
          ninja

      - name: Package
        run: |
          cd build
          ninja package

      - name: Sign
        run: |
          cd build
          sha256sum dinero-*.tar.gz > SHA256SUMS
          echo "$GPG_PRIVATE_KEY" | gpg --import
          gpg --detach-sign --armor SHA256SUMS
        env:
          GPG_PRIVATE_KEY: ${{ secrets.GPG_PRIVATE_KEY }}

      - name: Upload
        uses: actions/upload-artifact@v3
        with:
          name: linux-build
          path: build/dinero-*.tar.gz

  build-macos:
    runs-on: macos-13
    steps:
      - uses: actions/checkout@v3

      - name: Build
        run: |
          mkdir build && cd build
          cmake -DCMAKE_BUILD_TYPE=Release ..
          make -j$(sysctl -n hw.ncpu)

      - name: Sign
        run: |
          codesign --sign "$DEVELOPER_ID" --options runtime DineroCoin.app
        env:
          DEVELOPER_ID: ${{ secrets.MACOS_DEVELOPER_ID }}

      - name: Notarize
        run: |
          # Notarization process...
          echo "Notarizing..."

      - name: Upload
        uses: actions/upload-artifact@v3
        with:
          name: macos-build
          path: build/DineroCoin.dmg

  build-windows:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v3

      - name: Build
        run: |
          mkdir build
          cd build
          cmake -DCMAKE_BUILD_TYPE=Release ..
          cmake --build . --config Release

      - name: Sign
        run: |
          signtool sign /f cert.pfx /p $env:CERT_PASSWORD DineroCoin.exe
        env:
          CERT_PASSWORD: ${{ secrets.WINDOWS_CERT_PASSWORD }}

      - name: Upload
        uses: actions/upload-artifact@v3
        with:
          name: windows-build
          path: build/DineroCoin.exe

  create-release:
    needs: [build-linux, build-macos, build-windows]
    runs-on: ubuntu-latest
    steps:
      - name: Download artifacts
        uses: actions/download-artifact@v3

      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: |
            linux-build/*
            macos-build/*
            windows-build/*
          body: |
            ## DineroCoin ${{ github.ref_name }}

            ### What's New
            - Confidential transactions
            - Bulletproof range proofs
            - View key scanning

            ### Downloads
            - Linux: `dinero-$VERSION-linux-x86_64.tar.gz`
            - macOS: `DineroCoin.dmg`
            - Windows: `DineroCoin.exe`

            ### Checksums
            See `SHA256SUMS` for file hashes.
```

---

### 5. Changelog Generation

```bash
#!/bin/bash
# generate-changelog.sh

VERSION="1.0.0"
PREVIOUS_VERSION="0.9.0"

echo "# DineroCoin $VERSION Release Notes"
echo ""
echo "## Notable Changes"
echo ""

# Get commits since last release
git log --pretty=format:"- %s (%h)" v$PREVIOUS_VERSION..v$VERSION

echo ""
echo "## Checksums"
echo ""
cat SHA256SUMS
```

---

## 🚨 Incident Response Plan

### 1. Critical Bug Discovery

**Severity Levels**:
- **P0 (Critical)**: Consensus bug, funds at risk
- **P1 (High)**: Privacy leak, DoS vulnerability
- **P2 (Medium)**: Performance issue, minor bug
- **P3 (Low)**: Cosmetic, documentation

**Response Time**:
- P0: Immediate (< 1 hour)
- P1: Urgent (< 24 hours)
- P2: Standard (< 1 week)
- P3: Low priority (next release)

### 2. Emergency Release Process

```bash
# 1. Create hotfix branch
git checkout -b hotfix/v1.0.1 v1.0.0

# 2. Fix bug
git commit -m "Fix critical bug XYZ"

# 3. Tag
git tag -s v1.0.1 -m "Emergency hotfix for bug XYZ"

# 4. Build
./build-release.sh

# 5. Sign
./sign-all.sh

# 6. Release
gh release create v1.0.1 --notes "Emergency hotfix for bug XYZ"

# 7. Notify
# - Twitter/X
# - Discord
# - Email exchanges
```

---

## ✅ Final Checklist

### Pre-Release

- [ ] All tests passing
- [ ] Security audit complete
- [ ] Consensus frozen
- [ ] Genesis block finalized
- [ ] Code signed
- [ ] Deterministic builds verified
- [ ] Release notes written

### Release Day

- [ ] Tag release
- [ ] Build all platforms
- [ ] Sign all binaries
- [ ] Upload to GitHub
- [ ] Update website
- [ ] Announce on social media
- [ ] Notify exchanges
- [ ] Monitor for issues

### Post-Release

- [ ] Monitor network health
- [ ] Track block production
- [ ] Monitor miner activity
- [ ] Track exchange integration
- [ ] Collect user feedback

---

**Last Updated**: 2025-11-17
**Document Owner**: DineroCoin Security Team
**Next Review**: Before mainnet launch

---
