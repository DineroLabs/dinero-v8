# Dinero Core v1.1 - Testing Infrastructure

**Complete guide to the Dinero Core testing ecosystem**

## 📋 Table of Contents

1. [Overview](#overview)
2. [Test Architecture](#test-architecture)
3. [Running Tests](#running-tests)
4. [Test Coverage](#test-coverage)
5. [CI/CD Integration](#cicd-integration)
6. [Development Workflow](#development-workflow)
7. [Troubleshooting](#troubleshooting)

---

## Overview

Dinero Core has **4 tiers of testing** to ensure stability before production:

| Tier | Type | Purpose | Runtime |
|------|------|---------|---------|
| 🟢 **T1** | Unit Tests | Fast component tests | < 5 sec |
| 🟡 **T2** | Integration Tests | Service interaction tests | < 30 sec |
| 🟠 **T3** | Regression Tests | Critical user scenarios | < 2 min |
| 🔴 **T4** | Fuzzing & Stress | Edge cases & DoS resistance | Hours |

### When to Run Each Tier

```
Every commit        → T1 (unit tests)
Before PR           → T1 + T2 (integration)
Before merge        → T1 + T2 + T3 (regression)
Before release      → T1 + T2 + T3 + T4 (full suite)
```

---

## Test Architecture

### Directory Structure

```
tests/
├── unit/                      # T1: Unit tests (existing)
│   ├── test_bip39.cpp
│   ├── test_crypto_*.cpp
│   └── ...
│
├── integration/               # T2: Integration tests (existing)
│   ├── test_rpc_integration.cpp
│   ├── test_p2p_*.cpp
│   └── ...
│
├── regression/               # T3: NEW in v1.1
│   ├── test_wallet_recovery.cpp     ← BIP39, HD wallet restore
│   └── test_deep_reorg.cpp          ← 20-200 block reorgs
│
├── stress/                   # T4: NEW in v1.1
│   └── test_mempool_stress.cpp      ← 10k+ transaction load
│
├── fuzz/                     # T4: NEW in v1.1
│   ├── fuzz_block_validation.cpp
│   ├── fuzz_transaction.cpp
│   ├── fuzz_script.cpp
│   ├── fuzz_deserialize.cpp
│   ├── CMakeLists.txt
│   └── README.md
│
├── support/                  # Test helpers
│   ├── test_daemon_context.h        ← Mock DaemonContext
│   └── test_stubs.cpp               ← Stub implementations
│
└── README_REGRESSION_FUZZING.md
```

### Test Infrastructure Components

#### TestDaemonContext (support/test_daemon_context.h)

Provides isolated test context for deterministic testing:

```cpp
#include "support/test_daemon_context.h"

TEST_F(MyTest, TestBlockCreation) {
    auto test_ctx = std::make_unique<TestDaemonContext>();
    DaemonContext ctx = test_ctx->make();

    // Use ctx.chainstate, ctx.mempool, ctx.consensus, etc.
}
```

**Key features:**
- Isolated temp directories
- Mock services (no network, no disk I/O)
- Deterministic RNG seeds
- Automatic cleanup

#### Test Stubs (support/test_stubs.cpp)

Minimal stub implementations for linking tests without full daemon:

- `P2PManager` stub
- `NetworkManager` stub
- `ScannerManager` stub
- Global pointer stubs

---

## Running Tests

### Quick Commands

```bash
# Build tests
cmake -B build && cmake --build build

# Run all regression tests (T1 + T2 + T3)
./run_regression_tests.sh quick

# Run full suite including stress tests
./run_regression_tests.sh full

# Run fuzzing (requires Clang)
./run_fuzzing_suite.sh 300  # 5 minutes per fuzzer
```

### Individual Test Suites

#### T1: Unit Tests (GoogleTest)

```bash
# Run all unit tests
cd build && ctest --output-on-failure

# Run specific test
./build/tests/test_bip39
./build/tests/test_hd_wallet

# With verbose output
./build/tests/test_bip39 --gtest_filter="*Mnemonic*" --gtest_verbose
```

#### T2: Integration Tests

```bash
# RPC integration
./build/tests/test_rpc_integration

# Mining integration
./tests/test_mining.py

# P2P integration
./build/tests/test_p2p_integration
```

#### T3: Regression Tests (NEW)

```bash
# Wallet recovery (BIP39, HD wallet, encryption)
./build/tests/regression/test_wallet_recovery

# Deep reorgs (20-200 blocks, halving boundary)
./build/tests/regression/test_deep_reorg
```

**Expected output:**
```
[==========] Running 6 tests from 1 test suite.
[----------] 6 tests from WalletRecoveryTest
[ RUN      ] WalletRecoveryTest.BIP39MnemonicRecovery
[       OK ] WalletRecoveryTest.BIP39MnemonicRecovery (120 ms)
[ RUN      ] WalletRecoveryTest.WalletDatabaseBackupRestore
[       OK ] WalletRecoveryTest.WalletDatabaseBackupRestore (85 ms)
...
[==========] 6 tests from 1 test suite ran. (512 ms total)
[  PASSED  ] 6 tests.
```

#### T4: Stress & Fuzzing Tests (NEW)

```bash
# Mempool stress (10,000 transactions)
./build/tests/stress/test_mempool_stress

# Fuzzing with libFuzzer
cd build-fuzz/tests/fuzz

# Block validation fuzzer
./fuzz_block_validation corpus_block -max_total_time=300

# Transaction fuzzer
./fuzz_transaction corpus_transaction -max_total_time=300

# Script execution fuzzer
./fuzz_script corpus_script -max_total_time=300

# All deserialization paths
./fuzz_deserialize corpus_deserialize -max_total_time=300
```

**Fuzzer output:**
```
INFO: Running with entropic power schedule (0xFF, 100).
INFO: Seed: 3157347241
INFO: Loaded 1 modules   (65536 inline 8-bit counters)
INFO: Loaded 1 PC tables (65536 PCs)
INFO:      127 files found in corpus_block
INFO: -max_total_time=300 seconds
#2048   INITED cov: 234 ft: 456 corp: 127/15Kb exec/s: 1024 rss: 42Mb
#4096   NEW    cov: 256 ft: 478 corp: 128/16Kb exec/s: 2048 rss: 44Mb
...
INFO: Max total time reached.
===== DONE (300 seconds) =====
```

---

## Test Coverage

### Coverage Goals by Subsystem

| Subsystem | Target | Critical Paths | Status |
|-----------|--------|----------------|--------|
| **Wallet** | 100% | BIP39, HD derivation, encryption | ✅ T3 |
| **Consensus** | 100% | Block validation, reorgs, halvings | ✅ T3 |
| **Mempool** | 90% | TX validation, eviction, CPFP | ✅ T4 |
| **P2P** | 85% | Message parsing, peer management | ✅ T1 |
| **Mining** | 95% | Block assembly, template validation | ✅ T1 |
| **RPC** | 90% | All RPC endpoints | ✅ T2 |
| **Script** | 95% | All opcodes, signature verification | ✅ T4 |

### Generating Coverage Reports

```bash
# Build with coverage instrumentation
cmake -B build-coverage \
    -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
cmake --build build-coverage

# Run tests
cd build-coverage
LLVM_PROFILE_FILE="coverage.profraw" ctest

# Generate report
llvm-profdata merge -sparse coverage.profraw -o coverage.profdata
llvm-cov show ./tests/test_* -instr-profile=coverage.profdata > coverage.txt
llvm-cov report ./tests/test_* -instr-profile=coverage.profdata
```

**Example report:**
```
Filename            Regions    Missed   Coverage
-----------------------------------------------------
wallet/hd_wallet.cpp     156         0    100.00%
consensus/pow.cpp         89         5     94.38%
mempool/pool.cpp         234        23     90.17%
```

---

## CI/CD Integration

### GitHub Actions Workflow

**File:** `.github/workflows/regression-tests.yml`

**Jobs:**

1. **regression-tests** (Ubuntu + macOS)
   - Runs T1 + T2 + T3
   - Matrix: Debug & Release builds
   - Uploads test results as artifacts

2. **fuzzing-tests** (Ubuntu, Clang)
   - Runs all fuzzers for 30 minutes each
   - Caches corpus between runs
   - Uploads crashes if found
   - **Fails build if crashes detected**

3. **sanitizer-tests** (Ubuntu, Clang)
   - AddressSanitizer (detects memory errors)
   - UBSan (detects undefined behavior)
   - Runs all tests with sanitizers enabled

**Triggers:**
- Every push to `main` or `develop`
- Every pull request
- Nightly at 2 AM UTC (scheduled)

### Local CI Simulation

```bash
# Simulate GitHub Actions locally with act
brew install act  # or apt install act

# Run regression workflow
act -j regression-tests

# Run fuzzing workflow
act -j fuzzing-tests
```

---

## Development Workflow

### Adding New Tests

#### 1. Add Unit Test (T1)

```cpp
// tests/unit/test_my_feature.cpp
#include <gtest/gtest.h>
#include "my_feature.h"

TEST(MyFeatureTest, BasicFunctionality) {
    MyFeature feature;
    EXPECT_EQ(feature.compute(10), 20);
}
```

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(test_my_feature unit/test_my_feature.cpp)
target_link_libraries(test_my_feature PRIVATE gtest gtest_main dinero_core)
add_test(NAME MyFeature COMMAND test_my_feature)
```

#### 2. Add Regression Test (T3)

```cpp
// tests/regression/test_my_critical_scenario.cpp
#include <gtest/gtest.h>
#include "support/test_daemon_context.h"

class MyCriticalTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_ctx = std::make_unique<TestDaemonContext>();
        ctx = test_ctx->make();
    }

    std::unique_ptr<TestDaemonContext> test_ctx;
    DaemonContext ctx;
};

TEST_F(MyCriticalTest, UserCannotLoseFunds) {
    // Test critical user scenario
}
```

#### 3. Add Fuzzer (T4)

```cpp
// tests/fuzz/fuzz_my_parser.cpp
#include <cstdint>
#include <cstddef>
#include "my_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4) return 0;

    MyParser parser;
    parser.Parse(data, size);

    return 0;
}
```

Add to `tests/fuzz/CMakeLists.txt`:
```cmake
add_fuzzer(fuzz_my_parser fuzz_my_parser.cpp)
```

### Pre-Commit Checklist

Before committing code:

```bash
# 1. Run unit tests
cd build && ctest --output-on-failure

# 2. Run affected integration tests
./build/tests/test_rpc_integration  # if you modified RPC

# 3. (Optional) Run regression tests
./run_regression_tests.sh quick

# 4. Format code
clang-format -i src/**/*.cpp include/**/*.h

# 5. Commit
git add .
git commit -m "feat: add new feature with tests"
```

### Pre-PR Checklist

Before creating pull request:

```bash
# 1. Run full regression suite
./run_regression_tests.sh full

# 2. Build on multiple platforms
cmake -B build-linux && cmake --build build-linux
cmake -B build-macos && cmake --build build-macos

# 3. Check for compilation warnings
cmake -B build -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror"
cmake --build build

# 4. (Optional) Quick fuzzing run
./run_fuzzing_suite.sh 60  # 1 minute per fuzzer
```

### Pre-Release Checklist

Before tagging a release:

```bash
# 1. Run full test suite
./run_regression_tests.sh full

# 2. Run extended fuzzing (overnight)
./run_fuzzing_suite.sh 28800  # 8 hours per fuzzer

# 3. Run on testnet
./dinerod --testnet
# Let it sync and run for 24 hours

# 4. Trigger testnet reorg
# (See docs/TESTING_REORG_TESTNET.md)

# 5. Test wallet recovery on mainnet copy
cp ~/.dinero/wallet.db ~/.dinero/wallet_backup.db
# Test restoration from mnemonic

# 6. Performance benchmarks
./build/bench-simd  # Crypto benchmarks
# Monitor memory usage, CPU usage

# 7. Security audit
# Review all fuzzer crashes
# Check for known vulnerabilities
```

---

## Troubleshooting

### Common Issues

#### Issue: Test fails only on CI

**Symptoms:**
- Passes locally
- Fails on GitHub Actions

**Solutions:**

1. **Timing-dependent test:**
   ```cpp
   // Bad: assumes fast machine
   EXPECT_LT(elapsed, 100ms);

   // Good: use reasonable timeout
   EXPECT_LT(elapsed, 1000ms);
   ```

2. **Filesystem path differences:**
   ```cpp
   // Bad: hardcoded path
   std::filesystem::path test_dir = "/tmp/test";

   // Good: portable temp directory
   auto test_dir = std::filesystem::temp_directory_path() / "dinero_test";
   ```

3. **Missing cleanup:**
   ```cpp
   void TearDown() override {
       // Always cleanup temp files
       std::filesystem::remove_all(test_dir);
   }
   ```

#### Issue: Fuzzer runs slowly (< 100 exec/sec)

**Solutions:**

1. **Disable expensive checks in fuzzing build:**
   ```cpp
   #ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
       // Expensive validation
       assert(VerifyExpensiveInvariant());
   #endif
   ```

2. **Reduce max input size:**
   ```bash
   ./fuzz_block_validation corpus_block -max_len=100000
   ```

3. **Profile with perf:**
   ```bash
   perf record ./fuzz_block_validation corpus_block -runs=10000
   perf report
   ```

#### Issue: Crash only reproduces with sanitizers

**Symptoms:**
- Fuzzer finds crash
- Crash doesn't reproduce in normal build

**Explanation:** Likely uninitialized memory or use-after-free

**Solution:**
```bash
# Build with AddressSanitizer
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address"
cmake --build build-asan

# Reproduce
./build-asan/dinerod < crash-file
```

---

## Additional Resources

- **Fuzzing Guide:** `tests/fuzz/README.md`
- **Test API Reference:** `tests/support/test_daemon_context.h`
- **CI Configuration:** `.github/workflows/regression-tests.yml`
- **Coverage Reports:** `build/coverage/index.html` (after generating)

---

**Last Updated:** 2025-11-06
**Version:** 1.1.0
**Maintainers:** Dinero Core Team
