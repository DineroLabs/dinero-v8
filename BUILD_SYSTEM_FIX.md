# Build System Permanent Fix - Duplicate Symbol Resolution

## Problem Analysis

### Root Cause: ODR (One Definition Rule) Violation

**Duplicate Symbol:**
```
duplicate symbol 'dinero::Params()' in:
    libdinero_crypto.a[4](params_stub.cpp.o)
    libdinero_consensus.a[2](chainparams_impl.cpp.o)
```

**Two Incompatible Implementations:**

1. **`src/crypto/params_stub.cpp`** (in `dinero_crypto` library)
   ```cpp
   const ChainParamsStub& Params() {  // Returns ChainParamsStub&
       static ChainParamsStub params;
       return params;
   }
   ```

2. **`src/consensus/chainparams_impl.cpp`** (in `dinero_consensus` library)
   ```cpp
   const ChainParams& Params() {  // Returns ChainParams&
       return *g_active;
   }
   ```

**Library Dependency Chain:**
```
dinero_consensus → dinero_crypto
      ↓                  ↓
chainparams_impl    params_stub
```

When both are linked, the linker encounters two definitions of `dinero::Params()` → **ODR violation**

---

## Permanent Fix Options

### Option 1: Weak Symbol (Quick Fix - NOT RECOMMENDED)

**Approach:** Make `params_stub.cpp` use weak linkage
```cpp
__attribute__((weak))
const ChainParams& Params() {
    // ...
}
```

**Pros:** Minimal code changes
**Cons:**
- Platform-specific (not portable to Windows)
- Hides the real problem
- Runtime behavior depends on link order
- **NOT a proper solution**

---

### Option 2: Conditional Compilation (Better)

**Approach:** Use preprocessor to exclude stub when building with full consensus

**File: `src/crypto/params_stub.cpp`**
```cpp
#ifndef DINERO_FULL_CONSENSUS

#include <string>

namespace dinero {

struct ChainParamsStub {
    std::string hrp = "din";
    const std::string& Bech32HRP() const { return hrp; }
};

const ChainParamsStub& Params() {
    static ChainParamsStub params;
    return params;
}

} // namespace dinero

#endif // !DINERO_FULL_CONSENSUS
```

**File: `CMakeLists.txt` (dinero_consensus library)**
```cmake
add_library(dinero_consensus STATIC
  src/consensus/chainparams_impl.cpp
  # ... other files
)

target_compile_definitions(dinero_consensus PUBLIC
  DINERO_FULL_CONSENSUS=1  # Disables params_stub
)
```

**Pros:**
- Standard C++ approach
- Clear intent in code
- Works on all platforms

**Cons:**
- Still has two implementations (even if conditionally compiled)
- Requires careful macro management

---

### Option 3: Separate Test Utility Library (RECOMMENDED)

**Approach:** Create dedicated test utility library for stubs/mocks

**Step 1: Create new library**
```cmake
# Test utilities library (ONLY for tests)
add_library(dinero_test_utils STATIC
  src/crypto/params_stub.cpp
  tests/support/test_stubs.cpp
  # Other test-only utilities
)

target_include_directories(dinero_test_utils PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# This library is INCOMPATIBLE with dinero_consensus
target_compile_definitions(dinero_test_utils PUBLIC
  DINERO_TEST_MODE=1
)
```

**Step 2: Remove params_stub from dinero_crypto**
```cmake
add_library(dinero_crypto STATIC
  src/crypto/sha256.cpp
  src/crypto/ripemd160_standalone.cpp
  # src/crypto/params_stub.cpp  ← REMOVE THIS
  src/crypto/bip39.cpp
  # ... rest
)
```

**Step 3: Make HDWallet not depend on Params() directly**

Create an interface for chain parameters:

**File: `include/wallet/chain_params_provider.h`**
```cpp
#pragma once
#include <string>

namespace dinero {

// Abstract interface for accessing chain parameters
// This allows HDWallet to work without direct Params() dependency
class ChainParamsProvider {
public:
    virtual ~ChainParamsProvider() = default;
    virtual std::string GetHRP() const = 0;
    virtual uint32_t GetCoinType() const = 0;
};

// Default implementation using dinero::Params()
class DefaultChainParamsProvider : public ChainParamsProvider {
public:
    std::string GetHRP() const override;
    uint32_t GetCoinType() const override;
};

// Test stub implementation (doesn't require Params())
class StubChainParamsProvider : public ChainParamsProvider {
private:
    std::string hrp_;
    uint32_t coin_type_;
public:
    StubChainParamsProvider(std::string hrp = "din", uint32_t coin_type = 1447)
        : hrp_(std::move(hrp)), coin_type_(coin_type) {}

    std::string GetHRP() const override { return hrp_; }
    uint32_t GetCoinType() const override { return coin_type_; }
};

} // namespace dinero
```

**File: `src/wallet/chain_params_provider.cpp`**
```cpp
#include "wallet/chain_params_provider.h"
#include "consensus/chainparams.h"

namespace dinero {

std::string DefaultChainParamsProvider::GetHRP() const {
    return Params().hrp;
}

uint32_t DefaultChainParamsProvider::GetCoinType() const {
    // Derive from chain type
    switch (GetActiveChain()) {
        case Chain::MAINNET: return 1447;
        case Chain::TESTNET: return 1;
        case Chain::REGTEST: return 1;
        default: return 1447;
    }
}

} // namespace dinero
```

**File: `include/wallet/hd_wallet.h`** (modify)
```cpp
class HDWallet {
public:
    // Factory methods now accept optional params provider
    static std::unique_ptr<HDWallet> Open(
        const std::string& datadir,
        uint32_t coin_type,
        std::shared_ptr<ChainParamsProvider> params_provider = nullptr
    );

    // ... rest of interface

private:
    std::shared_ptr<ChainParamsProvider> params_provider_;

    // Use params_provider_->GetHRP() instead of dinero::Params().hrp
};
```

**Step 4: Update wallet implementation**

Replace all instances of:
```cpp
std::string hrp = dinero::Params().hrp;
```

With:
```cpp
std::string hrp = params_provider_ ? params_provider_->GetHRP() : "din";
```

**Step 5: Tests use stub provider**
```cpp
// In test_taproot_mining.cpp
auto stub_params = std::make_shared<StubChainParamsProvider>("din", 1447);
auto wallet = HDWallet::Open(testDir, 1447, stub_params);
```

**Pros:**
- ✅ **Clean separation**: Production code vs test code
- ✅ **No ODR violations**: Only one Params() implementation per binary
- ✅ **Testable**: HDWallet can be tested without full consensus
- ✅ **Flexible**: Easy to mock different network configurations
- ✅ **Standard design pattern**: Dependency injection
- ✅ **Future-proof**: Easy to add new chain parameter requirements

**Cons:**
- Requires refactoring HDWallet
- More files to maintain

---

### Option 4: Header-Only Params Accessor (CLEANEST)

**Approach:** Make `Params()` an inline function in header that delegates to implementation

**File: `include/consensus/chainparams.h`**
```cpp
// Forward declaration of implementation detail
namespace detail {
    const ChainParams& ParamsImpl();
}

// Inline accessor (no duplicate symbols possible)
inline const ChainParams& Params() {
    return detail::ParamsImpl();
}
```

**File: `src/consensus/chainparams_impl.cpp`**
```cpp
namespace dinero {
namespace detail {

const ChainParams& ParamsImpl() {  // ← Renamed
    if (g_active == &g_regtest && g_regtest.genesis.merkleRootHex.empty()) {
        // ... init logic
    }
    return *g_active;
}

} // namespace detail
} // namespace dinero
```

**File: `src/crypto/params_stub.cpp`**
```cpp
namespace dinero {
namespace detail {

// Minimal stub that matches interface
static struct {
    std::string hrp = "din";
    // ... minimal ChainParams fields
} stub_params;

const ChainParams& ParamsImpl() {
    return reinterpret_cast<const ChainParams&>(stub_params);
}

} // namespace detail
} // namespace dinero
```

**Pros:**
- ✅ **No ODR violation**: `Params()` is inline, only one symbol per TU
- ✅ **Minimal changes**: Just rename implementation function
- ✅ **Backwards compatible**: All calling code stays the same
- ✅ **Fast**: Inline = no function call overhead

**Cons:**
- `detail::ParamsImpl()` still has duplicate symbols (but different namespace)
- Still need linker tricks to prefer one over the other

---

## Recommended Solution

**Combination of Option 3 + Option 4:**

1. **Option 4 first** (immediate fix, minimal changes)
   - Make `Params()` inline in header
   - Move implementations to `detail::ParamsImpl()`
   - Use conditional compilation or weak symbols for `detail::ParamsImpl()`

2. **Option 3 second** (long-term proper architecture)
   - Refactor HDWallet to use dependency injection
   - Create proper test utilities library
   - Remove params_stub from production libraries

---

## Implementation Steps (Immediate Fix)

### Step 1: Modify chainparams.h

```cpp
// include/consensus/chainparams.h

namespace dinero {

// Forward declarations
namespace detail {
    const ChainParams& ParamsImpl();
}

// Inline accessor (prevents duplicate symbols)
inline const ChainParams& Params() {
    return detail::ParamsImpl();
}

// ... rest of header
```

### Step 2: Update chainparams_impl.cpp

```cpp
// src/consensus/chainparams_impl.cpp

namespace dinero {
namespace detail {

const ChainParams& ParamsImpl() {  // ← Moved to detail namespace
    // ... existing implementation
    if (g_active == &g_regtest && g_regtest.genesis.merkleRootHex.empty()) {
        // ...
    }
    return *g_active;
}

} // namespace detail
} // namespace dinero
```

### Step 3: Update params_stub.cpp with weak symbol

```cpp
// src/crypto/params_stub.cpp

#include "consensus/chainparams.h"

namespace dinero {
namespace detail {

// Weak symbol: will be overridden by chainparams_impl if linked
__attribute__((weak))
const ChainParams& ParamsImpl() {
    static struct {
        char _padding[sizeof(ChainParams)];
    } stub_storage;

    static bool initialized = false;
    if (!initialized) {
        auto* stub = new (&stub_storage) ChainParams{};
        stub->hrp = "din";
        stub->name = "mainnet";
        // ... minimal initialization
        initialized = true;
    }

    return *reinterpret_cast<ChainParams*>(&stub_storage);
}

} // namespace detail
} // namespace dinero
```

### Step 4: Add platform-specific weak symbol for Windows

```cpp
// src/crypto/params_stub.cpp

#ifdef _MSC_VER
    #define WEAK_SYMBOL __declspec(selectany)
#elif defined(__GNUC__) || defined(__clang__)
    #define WEAK_SYMBOL __attribute__((weak))
#else
    #error "Unsupported compiler"
#endif

namespace dinero {
namespace detail {

WEAK_SYMBOL
const ChainParams& ParamsImpl() {
    // ... implementation
}

} // namespace detail
} // namespace dinero
```

---

## Testing the Fix

After implementing, test with:

```bash
# Clean rebuild
rm -rf CMakeCache.txt CMakeFiles/
cmake .

# Try building the problematic test
make test_taproot_mining

# Should link successfully without duplicate symbol errors
./test_taproot_mining
```

---

## Long-term Recommendations

1. **Remove params_stub.cpp entirely** once Option 3 is implemented
2. **Create `dinero_test_utils` library** for all test-only code
3. **Use dependency injection** for HDWallet and other components that need chain params
4. **Establish clear library boundaries**:
   - `dinero_crypto`: Pure cryptographic functions (no chain knowledge)
   - `dinero_consensus`: Consensus rules with chain params
   - `dinero_test_utils`: Test stubs and mocks (never link with production code)

---

## File Changes Summary

| File | Action | Lines |
|------|--------|-------|
| `include/consensus/chainparams.h` | Add inline Params() wrapper | +8 |
| `src/consensus/chainparams_impl.cpp` | Move to detail::ParamsImpl() | ~5 |
| `src/crypto/params_stub.cpp` | Add weak symbol + detail namespace | ~10 |
| `tests/test_taproot_mining.cpp` | No changes needed | 0 |
| `CMakeLists.txt` | (Optional) Add comments about symbol conflict | +5 |

**Total effort:** ~30 minutes of careful refactoring

---

## Why This Matters

This isn't just about fixing one test - it's about **proper software architecture**:

1. **ODR violations are undefined behavior** - can cause crashes, wrong function calls, or silent bugs
2. **Duplicate symbols prevent modular builds** - can't link libraries together
3. **Test code mixing with production** - increases binary size, attack surface
4. **Technical debt accumulation** - makes future changes harder

The proper fix establishes clean boundaries and makes the codebase maintainable long-term.

---

**Status:** 🚧 **NOT YET IMPLEMENTED** (Requires code changes above)
**Recommended Path:** Implement Step 1-4 (immediate fix), then plan Option 3 (long-term)
**Estimated Time:** 30-60 minutes for immediate fix, 4-6 hours for full Option 3 refactor
