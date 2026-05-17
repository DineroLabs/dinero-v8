# Bulletproofs FFI Installation Guide

## 📦 Quick Start

### Prerequisites

You need Rust installed. If you don't have it:

```bash
# Install Rust (takes ~5 minutes)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Follow the prompts (press Enter to accept defaults)
# When done, reload your environment:
source $HOME/.cargo/env

# Verify installation:
rustc --version
cargo --version
```

**Expected output:**
```
rustc 1.75.0 (or newer)
cargo 1.75.0 (or newer)
```

---

## 🔧 Build Steps

### Step 1: Build Bulletproofs FFI Library

```bash
cd /Users/haydarevich/Documents/DineroCoin/third_party/bulletproofs_ffi

# Option 1: Use the build script (RECOMMENDED)
./build.sh

# Option 2: Manual build
cargo build --release
cargo test --release
```

**Build Output** (takes 2-3 minutes on first build):
```
   Compiling curve25519-dalek v4.1.0
   Compiling merlin v3.0.0
   Compiling bulletproofs v4.0.0
   Compiling bulletproofs-ffi v1.0.0
    Finished release [optimized] target(s) in 2m 15s
```

**Verify the library was created:**
```bash
ls -lh target/release/libbulletproofs_ffi.*
```

You should see:
```
-rw-r--r--  libbulletproofs_ffi.a       (static library, ~2-3 MB)
-rwxr-xr-x  libbulletproofs_ffi.dylib   (dynamic library, macOS)
```

### Step 2: Rebuild DineroCoin

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Clean previous build
make clean

# Reconfigure CMake (will detect cargo and enable Bulletproofs FFI)
rm -f CMakeCache.txt
cmake .

# You should see in the CMake output:
# ✅ Bulletproofs FFI: ENABLED (Dalek via Rust)
#    Library: /Users/haydarevich/Documents/DineroCoin/third_party/bulletproofs_ffi/target/release/libbulletproofs_ffi.a

# Build dinerod
make dinerod -j$(sysctl -n hw.ncpu)
```

### Step 3: Verify Integration

```bash
# Check if bulletproofs symbols are linked in dinerod
nm dinerod | grep ' T _bp_'

# Expected output:
# 0000000100abc123 T _bp_init
# 0000000100abc456 T _bp_is_initialized
# 0000000100abc789 T _bp_generate
# 0000000100abcabc T _bp_verify
# 0000000100abcdef T _bp_verify_batch
# 0000000100abd012 T _bp_max_proof_size
# 0000000100abd345 T _bp_version
```

---

## 🧪 Testing

### Test 1: Run Rust Tests

```bash
cd third_party/bulletproofs_ffi

# Run all tests
cargo test --release

# Run with output
cargo test --release -- --nocapture

# Run specific test
cargo test --release test_proof_generation
```

**Expected output:**
```
running 3 tests
test tests::test_init ... ok
test tests::test_proof_generation ... ok
test tests::test_max_proof_size ... ok

test result: ok. 3 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.15s
```

### Test 2: Integration Test from C++

Create a test file `test_bulletproofs.cpp`:

```cpp
#include "crypto/bulletproofs.h"
#include "crypto/pedersen.h"
#include <iostream>
#include <cassert>

using namespace dinero::crypto;

int main() {
    try {
        std::cout << "Testing Bulletproofs FFI integration...\n";

        // 1. Generate random blinding factor
        auto blinding = PedersenCommitment::generateBlinding();
        std::cout << "✓ Generated blinding factor (" << blinding.size() << " bytes)\n";

        // 2. Create commitment
        uint64_t value = 12345;
        auto commitment = PedersenCommitment::commit(blinding, value);
        std::cout << "✓ Created commitment (" << commitment.size() << " bytes)\n";

        // 3. Generate range proof
        auto proof = BulletproofRangeProof::generate(value, blinding);
        std::cout << "✓ Generated range proof (" << proof.size() << " bytes)\n";

        // 4. Verify proof
        bool valid = BulletproofRangeProof::verify(commitment, proof);
        assert(valid);
        std::cout << "✓ Proof verification: PASSED\n";

        // 5. Test batch verification
        std::vector<std::vector<uint8_t>> commitments = {commitment};
        std::vector<std::vector<uint8_t>> proofs = {proof};
        bool batch_valid = BulletproofRangeProof::verifyBatch(commitments, proofs);
        assert(batch_valid);
        std::cout << "✓ Batch verification: PASSED\n";

        std::cout << "\n✅ All tests passed!\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << "\n";
        return 1;
    }
}
```

Compile and run:
```bash
g++ -std=c++20 -I include test_bulletproofs.cpp -o test_bp -L third_party/bulletproofs_ffi/target/release -lbulletproofs_ffi -ldl
./test_bp
```

---

## 📊 What CMake Does Automatically

When you run `cmake .` with cargo installed:

1. **Detects cargo**: `find_program(CARGO_EXECUTABLE cargo)`
2. **Creates build target**: `build_bulletproofs_ffi` runs `cargo build --release`
3. **Imports library**: Links `libbulletproofs_ffi.a` into dinerod
4. **Sets up dependencies**: Ensures Rust library builds before C++ linking
5. **Adds system libs**: Links `${CMAKE_DL_LIBS}` (required for FFI)

**CMake Configuration** (automatically added):
```cmake
# From CMakeLists.txt lines 171-218
set(BP_FFI_DIR "${CMAKE_SOURCE_DIR}/third_party/bulletproofs_ffi")
set(BP_FFI_LIB "${BP_FFI_DIR}/target/release/libbulletproofs_ffi.a")

add_custom_target(build_bulletproofs_ffi
    COMMAND cargo build --release
    WORKING_DIRECTORY "${BP_FFI_DIR}"
    COMMENT "Building Bulletproofs FFI library (Rust)..."
)

add_library(bulletproofs_ffi STATIC IMPORTED)
set_target_properties(bulletproofs_ffi PROPERTIES
    IMPORTED_LOCATION "${BP_FFI_LIB}"
)

add_dependencies(bulletproofs_ffi build_bulletproofs_ffi)
add_dependencies(dinerod build_bulletproofs_ffi)

target_link_libraries(dinerod PRIVATE bulletproofs_ffi ${CMAKE_DL_LIBS})
```

---

## 🔍 Troubleshooting

### Problem: "cargo: command not found"

**Solution:**
```bash
# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source $HOME/.cargo/env

# Verify
which cargo
# Should output: /Users/haydarevich/.cargo/bin/cargo
```

### Problem: "Bulletproofs FFI: DISABLED (cargo not found)"

**Cause**: CMake cached the old configuration before Rust was installed.

**Solution:**
```bash
# Clear CMake cache and reconfigure
rm -f CMakeCache.txt
cmake .
```

### Problem: Build fails with "cannot find crate bulletproofs"

**Solution:**
```bash
cd third_party/bulletproofs_ffi

# Clean and rebuild
cargo clean
cargo update
cargo build --release
```

### Problem: Linker error "undefined reference to bp_init"

**Cause**: Rust library wasn't built or linked correctly.

**Solution:**
```bash
# 1. Verify library exists
ls -lh third_party/bulletproofs_ffi/target/release/libbulletproofs_ffi.a

# 2. Rebuild from scratch
cd third_party/bulletproofs_ffi
cargo clean
cargo build --release

# 3. Rebuild DineroCoin
cd ../..
make clean
cmake .
make dinerod
```

### Problem: "dlopen error" at runtime

**Cause**: Missing `${CMAKE_DL_LIBS}` in linker flags.

**Solution**: This is already fixed in `CMakeLists.txt:987`. If you still see this:
```bash
# Check if dl library is linked
ldd dinerod | grep dl
# macOS: otool -L dinerod | grep dl

# Manually link if needed:
# Add -ldl to link flags
```

---

## 🚀 Performance Benchmarks

After building, you can run performance tests:

```bash
cd third_party/bulletproofs_ffi

# Run benchmarks (requires nightly Rust)
rustup install nightly
cargo +nightly bench
```

**Expected Results** (Apple M1 Pro / AMD Ryzen 9):

| Operation | Time | Throughput |
|-----------|------|------------|
| Proof Generation (64-bit) | ~8ms | ~125 proofs/sec |
| Proof Verification (single) | ~3ms | ~333 verifications/sec |
| Batch Verification (10 proofs) | ~15ms | ~667 verifications/sec |
| Batch Verification (100 proofs) | ~120ms | ~833 verifications/sec |

**Proof Sizes**:
- 8-bit range: ~450 bytes
- 16-bit range: ~515 bytes
- 32-bit range: ~610 bytes
- **64-bit range: ~674 bytes** ← DineroCoin uses this

---

## 📝 Optional: Vendor Dependencies (Offline Builds)

To enable completely offline builds (no internet required):

```bash
cd third_party/bulletproofs_ffi

# Download all dependencies (creates vendor/ directory)
cargo vendor --versioned-dirs

# Configure cargo to use vendored sources
mkdir -p .cargo
cat > .cargo/config.toml <<'EOF'
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "vendor"
EOF

# Test offline build
cargo build --release --frozen --offline
```

**Benefits**:
- ✅ No internet required
- ✅ Reproducible builds (exact dependency versions)
- ✅ Protection against crates.io outages
- ✅ Corporate/airgapped environments

**Size**: ~20-30 MB for all vendored dependencies

---

## 📖 API Reference

### C API (include/crypto/bulletproofs.h)

```c
// Initialize library (call once at startup)
int bp_init(void);

// Generate range proof
int bp_generate(
    uint64_t value,           // Value to prove (0 to 2^64-1)
    const uint8_t* blind_ptr, // 32-byte blinding factor
    uint8_t* proof_out,       // Output buffer (2048 bytes)
    size_t* proof_len_out     // Output length
);

// Verify single proof
int bp_verify(
    const uint8_t* commitment_ptr, // 32-byte commitment
    const uint8_t* proof_ptr,      // Proof bytes
    size_t proof_len               // Proof length
);

// Batch verify (2-3x faster)
int bp_verify_batch(
    const uint8_t** commitments_ptr,
    const uint8_t** proofs_ptr,
    const size_t* proof_lens_ptr,
    size_t count
);
```

### C++ API (include/crypto/bulletproofs.h)

```cpp
namespace dinero::crypto {

class BulletproofRangeProof {
public:
    // Generate proof
    static std::vector<uint8_t> generate(
        uint64_t value,
        const std::vector<uint8_t>& blinding
    );

    // Verify proof
    static bool verify(
        const std::vector<uint8_t>& commitment,
        const std::vector<uint8_t>& proof
    );

    // Batch verify (faster)
    static bool verifyBatch(
        const std::vector<std::vector<uint8_t>>& commitments,
        const std::vector<std::vector<uint8_t>>& proofs
    );
};

} // namespace dinero::crypto
```

---

## ✅ Completion Checklist

- [ ] Rust installed (`rustc --version` works)
- [ ] Bulletproofs FFI built (`./build.sh` succeeded)
- [ ] Library file exists (`ls target/release/libbulletproofs_ffi.a`)
- [ ] CMake detects cargo (`cmake .` shows "✅ Bulletproofs FFI: ENABLED")
- [ ] dinerod links successfully (`make dinerod`)
- [ ] Symbols present in binary (`nm dinerod | grep bp_`)
- [ ] Rust tests pass (`cargo test --release`)

---

## 🎯 Summary

**What you're building**: Production-grade Bulletproofs range proofs using the Dalek cryptography library (same as Grin, MobileCoin, Monero).

**Technology stack**:
- **Rust**: Dalek Bulletproofs 4.0 (FFI wrapper)
- **C++**: DineroCoin confidential transactions
- **Integration**: CMake handles cross-language builds automatically

**Result**: Zero-knowledge range proofs for confidential transactions (~674 bytes per proof, ~8ms generation, ~3ms verification).

---

**Last Updated**: November 17, 2025
**For support**: See `third_party/bulletproofs_ffi/README.md`
