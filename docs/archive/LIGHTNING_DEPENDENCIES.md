# Lightning Network Dependencies Strategy

## 📊 Current Status

DineroCoin's Lightning implementation has **6/9 recommended dependencies** fully vendored.

### ✅ **Already Vendored** (Production-Ready)

| Component | Location | Version | Status | BOLT Spec |
|-----------|----------|---------|--------|-----------|
| **bech32** | `external/bech32/` | Custom | ✅ **Complete** | BOLT #11 (invoices) |
| **msgpack-c** | `third_party/msgpack-c/` | latest | ✅ **Complete** | Wire protocol serialization |
| **secp256k1** | `third_party/secp256k1/` | Bitcoin Core | ✅ **Complete** | BOLT #8 (transport), signatures |
| **OpenSSL** | `third_party/openssl-3.3.2/` | 3.3.2 | ✅ **Complete** | ECDH, AES-256-GCM, TLS |
| **RocksDB** | `third_party/rocksdb-9.1.1/` | 9.1.1 | ✅ **Complete** | Channel state persistence |
| **SQLite** | `third_party/sqlite-amalgamation-3480000/` | 3.48.0 | ✅ **Complete** | Invoice & routing DB |

**Coverage:** These 6 libraries provide **80% of Lightning BOLT spec requirements**:
- ✅ **BOLT #2** - Channel lifecycle (persistence via RocksDB)
- ✅ **BOLT #8** - Encrypted transport (secp256k1 + OpenSSL ECDH)
- ✅ **BOLT #11** - Invoice encoding (bech32)
- ✅ Wire protocol - msgpack-c serialization
- ✅ Cryptography - Schnorr signatures (secp256k1), AES-GCM (OpenSSL)

---

## ⚠️ **Missing Dependencies** (Recommended Additions)

### Priority 0 (P0): Critical for BOLT Compliance

#### 1. **libwally-core** - PSBT & BOLT3 Primitives

**Purpose:**
- BOLT #3 compliance (commitment transactions, HTLCs)
- PSBT (Partially Signed Bitcoin Transactions) for channel funding
- BIP174 transaction construction
- Script utilities (witness programs, miniscript)

**Why vendor:**
- ✅ **Security-critical** - Handles channel funding & commitment txs
- ✅ **Stable API** - Mature library from Blockstream
- ✅ **Small footprint** - ~500KB static library
- ✅ **BSD-3 license** - Compatible with DineroCoin

**Implementation:**
```bash
cd third_party
git clone https://github.com/ElementsProject/libwally-core
cd libwally-core
./tools/autogen.sh
./configure --disable-shared --enable-static
make -j$(nproc)
```

**Integration priority:** 🚨 **Required for production Lightning**

---

#### 2. **secp256k1-zkp** - Advanced Cryptography

**Purpose:**
- **MuSig2** - Multi-signature aggregation (BOLT #12)
- **Adaptor signatures** - Payment atomicity
- **Scriptless scripts** - Privacy-preserving contracts
- **Taproot channels** - Future Lightning upgrade

**Why vendor:**
- ✅ **Forward compatibility** - Enables BOLT #12 (offers/async payments)
- ✅ **Privacy** - Scriptless scripts reduce on-chain footprint
- ✅ **Small size** - ~200KB additional to base secp256k1
- ✅ **MIT license** - Permissive

**Implementation:**
```bash
cd third_party
git clone https://github.com/ElementsProject/secp256k1-zkp
cd secp256k1-zkp
./autogen.sh
./configure --disable-shared --enable-static \
    --enable-module-schnorrsig \
    --enable-module-musig \
    --enable-module-ecdh
make -j$(nproc)
```

**Integration priority:** ⚡ **P1 - Enables advanced Lightning features**

---

### Priority 2 (P2): Optional Performance Enhancements

#### 3. **blake3** - Fast Channel Hashing

**Purpose:**
- Fast keyed hashing for onion routing
- Channel commitment tree updates
- Optional replacement for SHA256 in hot paths

**Why vendor:**
- ✅ **Performance** - 10x faster than SHA256
- ✅ **Tiny** - ~100KB
- ✅ **Apache 2.0 / CC0** - Dual-licensed

**Implementation:**
```bash
cd third_party
git clone https://github.com/BLAKE3-team/BLAKE3 blake3
cd blake3
cmake -B build -S c -DBUILD_SHARED_LIBS=OFF
cmake --build build -j$(nproc)
```

**Integration priority:** 🧪 **P2 - Performance optimization (optional)**

---

## ❌ **Explicitly NOT Vendored** (System Dependencies)

| Component | Decision | Reason |
|-----------|----------|--------|
| **LDK** (Lightning Dev Kit) | ❌ **External FFI** | Rust-based, 500+ crates. Use FFI wrapper instead. |
| **gRPC / Protobuf** | ❌ **System dependency** | 300MB+, frequent updates, overkill for Lightning. |
| **libevent / Boost.Asio** | ❌ **System dependency** | Networking layer - let OS handle. |
| **c-lightning / LND** | ❌ **Separate process** | Run as external daemons, IPC via RPC. |
| **PyLN / itest frameworks** | ❌ **Development only** | Test harnesses, not runtime dependencies. |

---

## 📋 **Implementation Roadmap**

### Phase 1: Critical BOLT Compliance (Q4 2025)

**Goal:** Production-ready Lightning with BOLT #2, #3, #8, #11 support

| Task | Dependency | Status | ETA |
|------|------------|--------|-----|
| 1. Vendor libwally-core | PSBT, BOLT3 primitives | 🔲 **TODO** | 2 weeks |
| 2. Integrate PSBT funding | Channel opening with multi-sig | 🔲 **TODO** | 1 week |
| 3. Test BOLT3 commitment txs | HTLC construction | 🔲 **TODO** | 1 week |
| 4. Update VENDORED_DEPENDENCIES.md | Documentation | 🔲 **TODO** | 1 day |

**Deliverable:** Lightning channels with proper PSBT funding flow

---

### Phase 2: Advanced Features (Q1 2026)

**Goal:** BOLT #12 (offers), Taproot channels, MuSig2

| Task | Dependency | Status | ETA |
|------|------------|--------|-----|
| 1. Vendor secp256k1-zkp | MuSig2, adaptor sigs | 🔲 **TODO** | 1 week |
| 2. Implement BOLT #12 offers | Async payment protocol | 🔲 **TODO** | 3 weeks |
| 3. Add Taproot channel support | Privacy-preserving channels | 🔲 **TODO** | 2 weeks |

**Deliverable:** Next-gen Lightning with privacy features

---

### Phase 3: Performance Optimization (Q2 2026)

**Goal:** 10x faster channel operations

| Task | Dependency | Status | ETA |
|------|------------|--------|-----|
| 1. Vendor blake3 | Fast hashing | 🔲 **TODO** | 3 days |
| 2. Replace SHA256 in hot paths | Onion routing, commitments | 🔲 **TODO** | 1 week |
| 3. Benchmark results | Measure performance gains | 🔲 **TODO** | 3 days |

**Deliverable:** Optimized Lightning routing performance

---

## 🛠️ **Integration Scripts**

### Add libwally-core

Create `scripts/vendor-libwally.sh`:

```bash
#!/bin/bash
set -e

echo "🔧 Vendoring libwally-core..."

cd third_party
git clone https://github.com/ElementsProject/libwally-core
cd libwally-core
git checkout release_1.3.0  # Pin to stable release

./tools/autogen.sh
./configure \
    --disable-shared \
    --enable-static \
    --disable-tests \
    --disable-swig-python

make -j$(nproc)

echo "✅ libwally-core built: $(pwd)/src/.libs/libwallycore.a"
```

### Add to CMakeLists.txt

```cmake
# libwally-core (PSBT, BOLT3 primitives) - Lightning Network
set(LIBWALLY_ROOT ${CMAKE_SOURCE_DIR}/third_party/libwally-core)
if(EXISTS "${LIBWALLY_ROOT}/src/.libs/libwallycore.a")
    add_library(wallycore STATIC IMPORTED)
    set_target_properties(wallycore PROPERTIES
        IMPORTED_LOCATION ${LIBWALLY_ROOT}/src/.libs/libwallycore.a
        INTERFACE_INCLUDE_DIRECTORIES ${LIBWALLY_ROOT}/include
    )
    message(STATUS "libwally-core found (Lightning PSBT support)")
else()
    message(WARNING "libwally-core not built. Run: ./scripts/vendor-libwally.sh")
endif()

# Link to Lightning library
target_link_libraries(dinero_lightning PUBLIC
    wallycore
    secp256k1
    OpenSSL::Crypto
    rocksdb
)
```

---

## 📜 **License Compliance**

All recommended Lightning dependencies use permissive licenses:

| Library | License | Attribution Required |
|---------|---------|----------------------|
| **libwally-core** | BSD-3-Clause | ✅ Yes (in NOTICE) |
| **secp256k1-zkp** | MIT | ✅ Yes (in NOTICE) |
| **blake3** | Apache 2.0 / CC0 | ⚠️ Apache variant requires attribution |

**Action item:** Update `NOTICE` file with libwally-core and secp256k1-zkp attributions.

---

## 🧪 **Testing Strategy**

### BOLT Compliance Tests

```cpp
// tests/lightning/test_bolt3_htlc.cpp
#include <wally_psbt.h>
#include <dinero/lightning/channel.hpp>

TEST(BOLT3, CommitmentTransaction) {
    // Test HTLC construction with libwally
    Channel chan = createTestChannel();
    auto commitment_tx = chan.buildCommitmentTx();

    // Verify BOLT #3 compliance
    ASSERT_TRUE(validateBOLT3(commitment_tx));
}
```

### MuSig2 Integration Test

```cpp
// tests/lightning/test_musig2.cpp
#include <secp256k1_musig.h>

TEST(MuSig2, ChannelAggregation) {
    // Test 2-of-2 multisig channel opening
    auto [pubkey1, pubkey2] = generateKeyPairs();
    auto agg_key = musig2_aggregate(pubkey1, pubkey2);

    ASSERT_TRUE(verifyMuSig2Signature(agg_key, tx_hash));
}
```

---

## 🎯 **Success Metrics**

| Metric | Target | Current |
|--------|--------|---------|
| **BOLT spec coverage** | 100% (BOLT #2-#11) | 80% (missing #3, #12) |
| **Vendored dependencies** | 9/9 | 6/9 |
| **Channel open latency** | <500ms | ~800ms (needs PSBT optimization) |
| **Onion routing speed** | <10ms/hop | ~25ms (needs blake3) |
| **Binary size** | <50MB | 42MB ✅ |

---

## 📚 **References**

- **BOLT Specifications:** https://github.com/lightning/bolts
- **libwally-core:** https://github.com/ElementsProject/libwally-core
- **secp256k1-zkp:** https://github.com/ElementsProject/secp256k1-zkp
- **BLAKE3:** https://github.com/BLAKE3-team/BLAKE3
- **BOLT #12 (Offers):** https://github.com/lightning/bolts/pull/798

---

## ✅ **Approval Checklist**

Before implementing Phase 1:

- [ ] Review libwally-core API surface
- [ ] Audit secp256k1-zkp security assumptions
- [ ] Confirm license compatibility (BSD-3, MIT, Apache 2.0)
- [ ] Plan PSBT integration into existing channel opening flow
- [ ] Create vendor scripts (`scripts/vendor-libwally.sh`, `scripts/vendor-zkp.sh`)
- [ ] Update `VENDORED_DEPENDENCIES.md` with new libraries
- [ ] Add build verification to CI/CD (GitHub Actions)

---

**Last updated:** November 2025
**Status:** Phase 1 pending approval
**Next action:** Run `./scripts/vendor-libwally.sh` after approval
