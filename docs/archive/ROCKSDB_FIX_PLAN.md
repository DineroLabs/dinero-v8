# RocksDB Build Fix Plan

**Status:** Build system issue (NOT protocol issue)
**Created:** December 13, 2025
**Context:** ChainDB invariant locking complete, blocked on RocksDB vendoring

## Problem Statement

The vendored RocksDB copy in `third_party/rocksdb/` has incomplete source:
- Source files compiled with test/debug hooks enabled
- Test utility headers missing from vendor directory
- Specifically: `test_util/sync_point.h` referenced but not included

**Build Error:**
```
/Users/haydarevich/Documents/DineroCoin/third_party/rocksdb/util/compression.h:27:10:
fatal error: 'test_util/sync_point.h' file not found
```

## What This Is NOT

❌ ChainDB design problem
❌ Consensus problem
❌ Invariant design problem
❌ DineroCoin architectural problem

This is a **RocksDB packaging error** - pure engineering logistics.

## Solution Options (Priority Order)

### Option 1: Use System RocksDB (RECOMMENDED)

**Why:** Clean, maintainable, no vendoring issues

**Steps:**
1. Install RocksDB via system package manager:
   ```bash
   # macOS
   brew install rocksdb

   # Ubuntu/Debian
   sudo apt-get install librocksdb-dev
   ```

2. Configure CMake to use system RocksDB:
   ```bash
   cmake -DUSE_SYSTEM_ROCKSDB=ON -DUSE_SYSTEM_OPENSSL=ON -B build
   ```

3. Verify build:
   ```bash
   cmake --build build -j$(sysctl -n hw.ncpu)
   ```

**Pros:**
- Clean separation of concerns
- System package manager handles updates
- No vendoring maintenance
- Proven approach (Bitcoin Core uses system libs)

**Cons:**
- Requires package manager
- Different versions across platforms
- Users must install dependency

### Option 2: Re-vendor Complete RocksDB

**Why:** Self-contained build, controlled version

**Steps:**
1. Download official RocksDB release:
   ```bash
   cd /tmp
   wget https://github.com/facebook/rocksdb/archive/refs/tags/v8.6.7.tar.gz
   tar xzf v8.6.7.tar.gz
   ```

2. Replace vendored copy:
   ```bash
   cd /Users/haydarevich/Documents/DineroCoin
   rm -rf third_party/rocksdb
   cp -r /tmp/rocksdb-8.6.7 third_party/rocksdb
   ```

3. Update CMakeLists.txt if needed (RocksDB version, options)

4. Verify build:
   ```bash
   cmake -DUSE_SYSTEM_OPENSSL=ON -B build
   cmake --build build -j$(sysctl -n hw.ncpu)
   ```

**Pros:**
- Self-contained (no external dependencies)
- Controlled version (reproducible builds)
- Works on systems without package manager

**Cons:**
- Must maintain vendored copy
- Larger repository size
- Manual updates required

### Option 3: Disable RocksDB Sync Points

**Why:** Quick workaround if above options fail

**Steps:**
1. Edit `third_party/rocksdb/CMakeLists.txt`:
   ```cmake
   # Add near top of file
   add_compile_definitions(ROCKSDB_LITE)
   # OR
   add_compile_definitions(NDEBUG)
   ```

2. Verify sync point code is disabled:
   ```bash
   grep -r "ROCKSDB_LITE" third_party/rocksdb/util/compression.h
   ```

3. Build:
   ```bash
   cmake -DUSE_SYSTEM_OPENSSL=ON -B build
   cmake --build build -j$(sysctl -n hw.ncpu)
   ```

**Pros:**
- Minimal changes
- Fast workaround

**Cons:**
- May disable other features
- Not a root cause fix
- Could mask future issues

## Recommended Approach

**For Development (macOS):**
Use Option 1 (system RocksDB via Homebrew)

**For Production Releases:**
Use Option 2 (complete vendored copy) for reproducible builds

**For Quick Testing:**
Use Option 3 (disable sync points) if needed

## Post-Fix Validation

After implementing fix, run these checks:

1. **Build succeeds:**
   ```bash
   cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tee build.log
   ```

2. **Binary runs:**
   ```bash
   ./build/dinerod --version
   ./build/dinerod --help
   ```

3. **ChainDB tests pass:**
   ```bash
   ./build/test_mine_blocks_standalone
   ./tests/test_tx_index_reorg.sh  # TX index rollback validation
   ```

4. **No missing symbols:**
   ```bash
   nm -u ./build/dinerod | grep -i rocksdb
   ```

## Next Steps After Fix

Once RocksDB build succeeds, proceed with:

1. **Reorg Stress Testing (Phase 4C):**
   - Run TX index reorg shell test
   - Run minimal 2-block fork test
   - Run deep reorg C++ test (100+ blocks)
   - Run TX edge case test
   - Document results
   - Fix any issues exposed
   - Tag v0.10.1 if needed

2. **Mempool Hardening (v0.11.0):**
   - Safe because ChainDB is locked
   - Pure read-only + policy logic
   - RBF, mempool persistence, fee logic, DoS rules

## Notes

- This is **NOT protocol work** - ChainDB invariants are complete
- Build system issues don't invalidate consensus work
- The architecture is correct, this is packaging logistics
- Fix when resources allow, no rush on protocol timeline
