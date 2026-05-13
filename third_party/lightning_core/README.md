# Lightning Core - Unified Lightning Network Dependencies

This module provides a **unified CMake interface** for all DineroCoin Lightning Network cryptographic libraries.

## 🎯 **Purpose**

Instead of manually linking against individual Lightning libraries (`libwally-core`, `secp256k1-zkp`, `blake3`), you can link against a single target:

```cmake
# Before (manual linking):
target_link_libraries(dinero_lightning PUBLIC
    wallycore
    secp256k1_zkp
    blake3
)

# After (unified target):
target_link_libraries(dinero_lightning PUBLIC
    lightning_core_static
)
```

---

## 📦 **Included Libraries**

| Library | Priority | Purpose | Status |
|---------|----------|---------|--------|
| **libwally-core** | P0 (CRITICAL) | PSBT, BOLT #3 commitment txs | Conditional |
| **secp256k1-zkp** | P1 (ADVANCED) | MuSig2, BOLT #12 offers | Conditional |
| **blake3** | P2 (PERFORMANCE) | 10x faster hashing | Conditional |

**Conditional** = Library is included if found, otherwise skipped with a warning

---

## 🚀 **Usage**

### In Your CMakeLists.txt

```cmake
# Add Lightning Core module
add_subdirectory(third_party/lightning_core)

# Link against unified target
target_link_libraries(dinero_lightning PUBLIC
    lightning_core_static
)
```

### Automatic Feature Detection

The module automatically enables compile definitions based on available libraries:

```cpp
// In your C++ code:
#ifdef HAVE_LIGHTNING_PSBT
    // libwally-core is available
    #include <wally_psbt.h>
#endif

#ifdef HAVE_LIGHTNING_MuSig2
    // secp256k1-zkp is available
    #include <secp256k1_musig.h>
#endif

#ifdef HAVE_LIGHTNING_BLAKE3_HASHING
    // blake3 is available
    #include <blake3.h>
#endif
```

---

## ⚙️ **Configuration Options**

Control library requirements at CMake configuration time:

```bash
# Require libwally-core (fail if missing)
cmake -DLIGHTNING_REQUIRE_WALLY=ON ..

# Require secp256k1-zkp (fail if missing)
cmake -DLIGHTNING_REQUIRE_ZKP=ON ..

# Require blake3 (fail if missing)
cmake -DLIGHTNING_REQUIRE_BLAKE3=ON ..

# Require all Lightning libraries
cmake -DLIGHTNING_REQUIRE_WALLY=ON \
      -DLIGHTNING_REQUIRE_ZKP=ON \
      -DLIGHTNING_REQUIRE_BLAKE3=ON ..
```

---

## 🛠️ **Building Dependencies**

Before using `lightning_core_static`, build the required libraries:

### Priority 0: Critical (libwally-core)

```bash
./scripts/vendor-libwally.sh
```

### Priority 1: Advanced (secp256k1-zkp)

```bash
./scripts/vendor-secp256k1-zkp.sh
```

### Priority 2: Performance (blake3)

```bash
./scripts/vendor-blake3.sh
```

### All at once

```bash
./scripts/vendor-libwally.sh && \
./scripts/vendor-secp256k1-zkp.sh && \
./scripts/vendor-blake3.sh
```

---

## 📊 **CMake Output**

When you configure DineroCoin, you'll see a summary:

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Lightning Core: Unified BOLT-compliant dependency module
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  ✅ libwally-core    (PSBT, BOLT #3 primitives)
     Location: /path/to/third_party/libwally-core/src/.libs/libwallycore.a
     Features: PSBT, commitment txs, script utilities
  ✅ secp256k1-zkp    (MuSig2, BOLT #12 offers)
     Location: /path/to/third_party/secp256k1-zkp/.libs/libsecp256k1.a
     Features: MuSig2, adaptor sigs, Taproot channels
  ✅ blake3           (10x faster hashing)
     Location: /path/to/third_party/blake3/c/build/libblake3.a
     Features: Optimized onion routing, channel updates

  🎯 lightning_core_static target created
     Enabled features: PSBT BOLT3 BIP174 MuSig2 BOLT12 Taproot BLAKE3_HASHING
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

## ❌ **Missing Library Warnings**

If a library is missing, you'll see helpful warnings:

```
  ⚠️  libwally-core   (PSBT, BOLT #3) - NOT FOUND
     Build with: ./scripts/vendor-libwally.sh
     Lightning channels will lack PSBT support!
```

---

## 🎯 **Target Properties**

The `lightning_core_static` target provides:

| Property | Description |
|----------|-------------|
| **Type** | `INTERFACE` library (header-only aggregation) |
| **Dependencies** | Dynamically links available Lightning libs |
| **Include dirs** | All Lightning library include directories |
| **Definitions** | `HAVE_LIGHTNING_*` for feature detection |
| **Alias** | `lightning::core` for modern CMake |

---

## 🧪 **Example Integration**

### DineroCoin Lightning Module

```cmake
# In src/lightning/CMakeLists.txt

add_library(dinero_lightning STATIC
    lightning_crypto.cpp
    channel_manager.cpp
    htlc_manager.cpp
    invoice.cpp
    onion.cpp
)

# Link against unified Lightning core
target_link_libraries(dinero_lightning PUBLIC
    lightning_core_static      # All Lightning crypto libs
    secp256k1                  # Base ECDSA (already vendored)
    OpenSSL::Crypto            # AES-GCM, ECDH
    rocksdb                    # Channel persistence
)

# Optionally check what features are enabled
if(TARGET lightning_wally)
    message(STATUS "Lightning: PSBT support enabled")
endif()

if(TARGET lightning_zkp)
    message(STATUS "Lightning: MuSig2 support enabled")
endif()

if(TARGET lightning_blake3)
    message(STATUS "Lightning: BLAKE3 performance mode enabled")
endif()
```

---

## 📚 **Further Reading**

- **Quick start:** [`LIGHTNING_VENDOR_QUICKSTART.md`](../../LIGHTNING_VENDOR_QUICKSTART.md)
- **Detailed analysis:** [`LIGHTNING_DEPENDENCIES.md`](../../LIGHTNING_DEPENDENCIES.md)
- **General vendoring:** [`VENDORED_DEPENDENCIES.md`](../../VENDORED_DEPENDENCIES.md)

---

## 🔧 **Troubleshooting**

### "No Lightning Network libraries found!"

**Cause:** None of the Lightning libraries are built.

**Solution:**
```bash
# Install at least libwally-core (Priority 0)
./scripts/vendor-libwally.sh

# Then reconfigure
cmake -B build -S . --fresh
```

### "libwally-core REQUIRED but not found!"

**Cause:** You set `-DLIGHTNING_REQUIRE_WALLY=ON` but didn't build libwally.

**Solution:**
```bash
./scripts/vendor-libwally.sh
cmake -B build -S .
```

### "Undefined reference to wally_psbt_*"

**Cause:** Your code uses libwally functions but `lightning_core_static` didn't find the library.

**Solution:**
1. Verify library is built: `ls third_party/libwally-core/src/.libs/libwallycore.a`
2. Reconfigure CMake: `cmake -B build -S . --fresh`
3. Check CMake output for warnings

---

**Last updated:** November 2025
**Maintainer:** DineroCoin Lightning Team
**Status:** Production-ready
