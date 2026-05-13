# Dinero Core - Testing

## Testing Policy

- **Tests must match current APIs and architecture.** Tests reflect reality, not history.
- **Obsolete tests are deleted, not patched.** Bad tests are worse than no tests.
- **Tests assert invariants, not implementation details.** Test *what*, not *how*.

### When to Delete Tests

- Reference deleted files or removed interfaces
- Mock interfaces that no longer exist
- Assume old ownership or outdated architecture
- Test behavior no longer part of the system

### When to Rewrite (Not Patch)

- Underlying API signature changed
- Test structure no longer matches code structure
- Test requires hacks to compile

---

## Test & Fuzzing Integration

This document describes the unified test and fuzzing infrastructure for DineroCoin.

---

## 🎯 Phase 2: Validation Without Refactoring (Current)

**Testing Philosophy:** Freeze structure, validate behavior, defer refactoring.

**Status:** Active testing phase - focus on correctness & stability
**Scope:**
- ✅ Test all L1 (consensus, UTXO, Utreexo) functionality
- ✅ Test Lightning integration (channel lifecycle, force-close, HTLCs)
- ✅ Test cross-cutting concerns (reorgs, RBF, fee estimation)
- ❌ **DO NOT** refactor core components during this phase
- ❌ **DO NOT** introduce new abstractions

**Known Architectural Work (Deferred to Phase 3):**
- Lightning build-time decoupling (see `docs/PHASE3_BUILD_DECOUPLING.md`)
- Wallet core extraction (see `docs/ARCHITECTURE_LIGHTNING_SEPARATION.md`)
- Symbol migration, library splits

**Rationale:** Refactoring during intensive testing introduces new bugs and invalidates baselines. Phase 2 validates what exists; Phase 3 improves structure.

**When to Move to Phase 3:** After mainnet stable 2+ weeks, Utreexo audit complete, all critical bugs fixed.

---

## Overview

The test system includes:
- **Regression tests**: Tests for wallet recovery, deep reorgs, and other critical functionality
- **Stress tests**: Mempool stress testing and performance validation
- **Fuzzing tests**: LibFuzzer-based fuzz testing for input validation
- **Unit tests**: Individual component tests

## Quick Start

### Running All Tests

```bash
cd ~/Documents/DineroCoin
./scripts/run_all_tests.sh
```

This will:
1. Build the project with tests enabled
2. Run all regression and stress tests via `ctest`
3. Display a summary of results

### Running Fuzzing Tests

```bash
./scripts/run_all_tests.sh --fuzz
```

This runs all tests plus fuzzing targets (requires Clang + libFuzzer).

### Running Tests via CMake

```bash
cd ~/Documents/DineroCoin
cmake -B build -DENABLE_TESTS=ON -DENABLE_FUZZING=OFF
cmake --build build -j$(sysctl -n hw.logicalcpu)
cd build
ctest --output-on-failure
```

### Running Individual Tests

```bash
cd ~/Documents/DineroCoin/build
./test_wallet_recovery
./test_deep_reorg
./test_mempool_stress
```

## Test Categories

### 1. Regression Tests (`tests/regression/`)
- `test_wallet_recovery`: Wallet recovery from mnemonic
- `test_deep_reorg`: Deep chain reorganization handling

### 2. Stress Tests (`tests/stress/`)
- `test_mempool_stress`: Mempool under high transaction load

### 3. Fuzzing Tests (`tests/fuzz/`)
- `fuzz_block_validation`: Block validation fuzzing
- `fuzz_deserialize`: Transaction deserialization fuzzing
- `fuzz_script`: Script parsing fuzzing
- `fuzz_transaction`: Transaction validation fuzzing

### 4. Unit Tests (`tests/`)
Over 180 unit tests covering:
- Wallet functions (BIP39, BIP32, BIP84, HD wallets)
- Mining functions (block assembly, difficulty adjustment)
- Cryptography (SHA256, RIPEMD160, secp256k1)
- Consensus (PoW validation, chain selection)
- Storage (RocksDB, SQLite)
- RPC (all RPC methods)
- P2P (network protocol, message handling)

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TESTS` | `ON` | Enable regression and stress tests |
| `ENABLE_FUZZING` | `OFF` | Enable fuzzing tests (requires Clang) |

### Examples

```bash
# Build with tests disabled
cmake -B build -DENABLE_TESTS=OFF

# Build with fuzzing enabled
cmake -B build -DENABLE_FUZZING=ON

# Build for coverage analysis
cmake -B build -DENABLE_COVERAGE=ON -DCMAKE_CXX_FLAGS="--coverage"
```

## Continuous Integration

The test suite is designed to run in CI/CD pipelines:

```bash
# Quick smoke test (< 2 minutes)
ctest -L smoke

# Full regression suite (< 10 minutes)
ctest -L regression

# Stress tests (may take longer)
ctest -L stress
```

## Adding New Tests

### 1. Create Test File

Place your test in the appropriate directory:
- `tests/regression/` - For regression tests
- `tests/stress/` - For stress tests
- `tests/fuzz/` - For fuzzing tests

### 2. Update CMakeLists.txt

Add your test to the main `CMakeLists.txt`:

```cmake
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/my_test.cpp)
  add_executable(test_my_feature tests/my_test.cpp)
  target_link_libraries(test_my_feature PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    gtest
    gtest_main
  )
  add_test(NAME MyFeature COMMAND test_my_feature)
endif()
```

### 3. Verify

```bash
cmake -B build -DENABLE_TESTS=ON
cmake --build build
ctest -R MyFeature
```

## Troubleshooting

### Tests Not Building

Check that you have all dependencies:
```bash
brew install cmake googletest jsoncpp rocksdb sqlite secp256k1
```

### Tests Failing

Run individual tests with verbose output:
```bash
cd build
./test_wallet_recovery --gtest_verbose
```

Check test logs:
```bash
cat build/test_results.log
```

### Fuzzing Issues

Fuzzing requires Clang with libFuzzer support:
```bash
# macOS
export CC=clang
export CXX=clang++
cmake -B build -DENABLE_FUZZING=ON
```

## Performance

Expected test execution times on Apple M1:
- Unit tests: ~30 seconds
- Regression tests: ~2 minutes
- Stress tests: ~5 minutes
- Fuzzing (60s per target): ~4 minutes

Total: ~12 minutes for complete test suite

## References

- [Google Test Documentation](https://google.github.io/googletest/)
- [CMake CTest Documentation](https://cmake.org/cmake/help/latest/manual/ctest.1.html)
- [LibFuzzer Documentation](https://llvm.org/docs/LibFuzzer.html)
