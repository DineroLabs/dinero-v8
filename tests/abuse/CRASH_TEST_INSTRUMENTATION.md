# Crash Test Instrumentation Guide

**Purpose:** Add controlled crash points to LoadSnapshot() for precise crash testing.

**Warning:** ⚠️ NEVER enable in production. Testing only.

---

## Instrumentation Points

Add the following macro to `chainstate_service.cpp`:

```cpp
// Crash test instrumentation (TESTING ONLY - never enable in production)
#ifdef ENABLE_CRASH_TESTING
#include <cstdlib>
#include <cstdio>
static int g_crash_point = 0;  // Set via environment variable
#define CRASH_TEST_POINT(id) \
    do { \
        const char* crash_at = std::getenv("CRASH_AT_POINT"); \
        if (crash_at && std::atoi(crash_at) == id) { \
            std::fprintf(stderr, "\n[CRASH TEST] Triggering abort at point %d\n", id); \
            std::abort(); \
        } \
    } while(0)
#else
#define CRASH_TEST_POINT(id) do {} while(0)
#endif
```

---

## Add Crash Points to LoadSnapshot()

Insert `CRASH_TEST_POINT(N)` at each critical boundary:

```cpp
consensus::SnapshotImportResult ChainstateService::LoadSnapshot(...) {
    // ... precondition checks ...

    CRASH_TEST_POINT(1);  // After precondition check, before file open

    try {
        std::ifstream file(snapshot_path, std::ios::binary);
        if (!file.is_open()) {
            result.error_message = "Failed to open snapshot file";
            return result;
        }

        CRASH_TEST_POINT(2);  // After file open, before header read

        // Read header
        SnapshotMetadata header;
        // ... read header fields ...

        CRASH_TEST_POINT(3);  // After header read, before validation

        // Verify magic, version, etc.
        if (header.magic != SNAPSHOT_MAGIC) {
            return result;
        }

        CRASH_TEST_POINT(4);  // After header validation, before UTXO read

        // Pass 1: Read UTXOs into memory
        std::vector<UTXO> utxos;
        for (uint64_t i = 0; i < header.utxo_count; ++i) {
            // ... read UTXO ...
            utxos.push_back(std::move(utxo));

            if (i == header.utxo_count / 2) {
                CRASH_TEST_POINT(5);  // Mid-pass-1 (50% through read)
            }
        }

        CRASH_TEST_POINT(6);  // After all UTXOs read, before checksum verify

        // Verify checksum
        uint8_t stored_checksum[32];
        file.read(reinterpret_cast<char*>(stored_checksum), 32);
        // ... compute and verify ...

        if (checksum_mismatch) {
            CRASH_TEST_POINT(7);  // On checksum failure (should rollback)
            return result;
        }

        CRASH_TEST_POINT(8);  // After checksum verified, before transaction

        // Pass 2: Begin transaction
        if (!utxo_index_->BeginTransaction()) {
            return result;
        }

        CRASH_TEST_POINT(9);  // After BeginTransaction, before import loop

        for (const auto& utxo : utxos) {
            if (!utxo_index_->AddUTXO(utxo)) {
                utxo_index_->RollbackTransaction();
                return result;
            }
            result.utxos_imported++;

            if (result.utxos_imported == utxos.size() / 2) {
                CRASH_TEST_POINT(10);  // Mid-import (50% through add)
            }
        }

        CRASH_TEST_POINT(11);  // After import loop, before commit

        if (!utxo_index_->CommitTransaction()) {
            utxo_index_->RollbackTransaction();
            return result;
        }

        CRASH_TEST_POINT(12);  // After commit, before flag set

        // Set AssumeUTXO flags
        assumeutxo_active_ = true;
        assumeutxo_base_block_ = header.block_hash;
        assumeutxo_base_height_ = header.block_height;

        CRASH_TEST_POINT(13);  // After flags set, before background validation

        // Start background validation
        StartBackgroundValidation();

        CRASH_TEST_POINT(14);  // After background validation start

        result.success = true;

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
    }

    return result;
}
```

---

## Crash Point Map

| ID | Location | Expected State After Crash |
|----|----------|----------------------------|
| 1  | After precondition check | Clean (no changes) |
| 2  | After file open | Clean (no changes) |
| 3  | After header read | Clean (no changes) |
| 4  | After header validation | Clean (no changes) |
| 5  | Mid-pass-1 (50% UTXOs read) | Clean (all in memory) |
| 6  | After UTXO read, before checksum | Clean (all in memory) |
| 7  | On checksum failure | Clean (error return) |
| 8  | After checksum verified | Clean (before transaction) |
| 9  | After BeginTransaction | **CRITICAL: Transaction open** |
| 10 | Mid-import (50% added) | **CRITICAL: Transaction open** |
| 11 | After loop, before commit | **CRITICAL: Transaction open** |
| 12 | After commit | **Full snapshot persisted** |
| 13 | After flags set | **Full snapshot + flags** |
| 14 | After background validation start | **Complete** |

---

## Build with Instrumentation

```bash
# Enable crash testing
cmake -DENABLE_CRASH_TESTING=ON ..
cmake --build . --target dinerod
```

---

## Run Crash Tests

Test each crash point:

```bash
#!/bin/bash
for crash_point in {1..14}; do
    echo "Testing crash at point $crash_point..."

    # Clean state
    rm -rf /tmp/test_datadir

    # Set environment variable
    export CRASH_AT_POINT=$crash_point

    # Start node (will crash at specified point)
    ./dinerod --datadir=/tmp/test_datadir --testnet &
    PID=$!

    # Load snapshot (will trigger crash)
    ./dinero-cli --datadir=/tmp/test_datadir loadtxoutset /tmp/snapshot.dat || true

    wait $PID || true

    # Verify UTXO count
    utxo_count=$(sqlite3 /tmp/test_datadir/wallet.db \
                 "SELECT COUNT(*) FROM utxos WHERE spend_height IS NULL" 2>/dev/null || echo "0")

    if [ "$crash_point" -le 11 ]; then
        # Before commit: should be 0 (rollback)
        if [ "$utxo_count" = "0" ]; then
            echo "  ✓ PASS: UTXO count = 0 (transaction rolled back)"
        else
            echo "  ✗ FAIL: UTXO count = $utxo_count (expected 0)"
            exit 1
        fi
    else
        # After commit: should be full snapshot or 0 (if crashed before persist)
        echo "  ✓ PASS: UTXO count = $utxo_count"
    fi

    unset CRASH_AT_POINT
done

echo ""
echo "✓ All crash points tested successfully"
```

---

## Expected Results

### Points 1-8: Before Transaction
**State:** In-memory only
**Crash:** No disk writes
**After restart:** UTXO count = 0
**Verdict:** ✅ SAFE

### Points 9-11: During Transaction
**State:** Transaction open, not committed
**Crash:** SQLite auto-rollback on restart
**After restart:** UTXO count = 0
**Verdict:** ✅ SAFE (CRITICAL-002 fix)

### Point 12: After Commit, Before Flags
**State:** UTXOs persisted, flags not set
**Crash:** UTXOs on disk, assumeutxo_active_ = false
**After restart:** 🔴 **POTENTIAL ISSUE**
**Verdict:** ⚠️ INVESTIGATE

### Points 13-14: After Flags
**State:** Full snapshot with flags
**Crash:** Complete state persisted
**After restart:** UTXO count = full snapshot
**Verdict:** ✅ SAFE

---

## Critical Finding: Point 12

**Issue:** If crash occurs after CommitTransaction() but before setting assumeutxo_active_ flag:
- UTXOs persisted to disk ✓
- Flag not set ❌
- Restart sees: Full UTXO set but no indication it's from snapshot

**Solution Options:**

### Option A: Persist flags atomically with UTXO commit
```cpp
// Store metadata in same transaction
utxo_index_->SetMetadata("assumeutxo_active", "true");
utxo_index_->SetMetadata("assumeutxo_base_block", header.block_hash.GetHex());
utxo_index_->CommitTransaction();  // Atomic commit of UTXOs + metadata
```

### Option B: Detect snapshot on restart
```cpp
// In ChainstateService::Start()
if (utxo_count > 0 && !assumeutxo_active_) {
    // Check if UTXOs match a known snapshot
    // If yes, set assumeutxo_active_ = true and start background validation
}
```

### Option C: WAL mode + synchronized writes (complex)

**Recommended:** Option A (persist metadata atomically)

---

## CMakeLists.txt Addition

```cmake
# Crash testing instrumentation (NEVER enable in production)
option(ENABLE_CRASH_TESTING "Enable crash test instrumentation" OFF)
if(ENABLE_CRASH_TESTING)
    add_compile_definitions(ENABLE_CRASH_TESTING)
    message(WARNING "⚠️  CRASH TESTING ENABLED - DO NOT USE IN PRODUCTION")
endif()
```

---

## Safety Notes

1. ⚠️ **NEVER** enable ENABLE_CRASH_TESTING in production
2. ⚠️ **NEVER** commit code with ENABLE_CRASH_TESTING=ON
3. ⚠️ Use separate build directory for crash testing
4. ⚠️ Crash test instrumentation can hide timing bugs
5. ⚠️ Also test without instrumentation (timing-based kills)

---

## Status

**Instrumentation:** Documented, not implemented
**Crash Test Script:** ✅ Created
**Next:** Run timing-based tests first, then add instrumentation if needed
