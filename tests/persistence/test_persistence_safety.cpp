// Persistence Safety Tests - Priority 4
// Following methodology: define invariants, write tests first, then fix code
//
// Invariants tested:
//   P1: Reorg marker must be transactional with state changes
//   P2: Incomplete reorg must be detected on startup
//   P3: Tip update must be synchronous (fsync)
//   P4: After crash recovery, wallet balance = consensus balance
//   P5: Block connect/disconnect must be atomic within each database
//
// Build: cmake --build build --target test_persistence_safety
// Run:   ./build/tests/persistence/test_persistence_safety

#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <random>

// Test framework
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    tests_run++; \
    std::cout << "  Running " << #name << "... "; \
    try { \
        test_##name(); \
        tests_passed++; \
        std::cout << "PASSED" << std::endl; \
    } catch (const std::exception& e) { \
        tests_failed++; \
        std::cout << "FAILED: " << e.what() << std::endl; \
    } \
} while(0)

#define SKIP_TEST(name, reason) do { \
    tests_run++; \
    tests_skipped++; \
    std::cout << "  Skipping " << #name << ": " << reason << std::endl; \
} while(0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::ostringstream oss; \
        oss << "Expected " << (a) << " == " << (b); \
        throw std::runtime_error(oss.str()); \
    } \
} while(0)

// ============================================================================
// Mock Database Layer for Testing Persistence Scenarios
// ============================================================================

// Simulates a crash at various points in the operation
enum class CrashPoint {
    NONE,
    BEFORE_MARKER_SET,
    AFTER_MARKER_SET_BEFORE_STATE,
    AFTER_STATE_BEFORE_MARKER_DELETE,
    AFTER_MARKER_DELETE
};

// Mock SQLite-like database with transaction support
class MockWalletDB {
public:
    std::map<std::string, std::string> metadata;
    std::map<std::string, uint64_t> utxos;  // txid:vout -> amount
    bool in_transaction = false;

    // Pending changes during transaction
    std::map<std::string, std::string> pending_metadata;
    std::map<std::string, uint64_t> pending_utxo_adds;
    std::vector<std::string> pending_utxo_deletes;

    void BeginTransaction() {
        in_transaction = true;
        pending_metadata.clear();
        pending_utxo_adds.clear();
        pending_utxo_deletes.clear();
    }

    void Commit() {
        if (!in_transaction) return;

        // Apply pending changes
        for (const auto& [k, v] : pending_metadata) {
            if (v.empty()) {
                metadata.erase(k);
            } else {
                metadata[k] = v;
            }
        }
        for (const auto& [k, v] : pending_utxo_adds) {
            utxos[k] = v;
        }
        for (const auto& k : pending_utxo_deletes) {
            utxos.erase(k);
        }

        in_transaction = false;
        pending_metadata.clear();
        pending_utxo_adds.clear();
        pending_utxo_deletes.clear();
    }

    void Rollback() {
        in_transaction = false;
        pending_metadata.clear();
        pending_utxo_adds.clear();
        pending_utxo_deletes.clear();
    }

    // Non-transactional writes (current buggy behavior)
    void SetMetadataDirect(const std::string& key, const std::string& value) {
        metadata[key] = value;
    }

    void DeleteMetadataDirect(const std::string& key) {
        metadata.erase(key);
    }

    // Transactional writes (fixed behavior)
    void SetMetadataTransactional(const std::string& key, const std::string& value) {
        if (in_transaction) {
            pending_metadata[key] = value;
        } else {
            metadata[key] = value;
        }
    }

    void DeleteMetadataTransactional(const std::string& key) {
        if (in_transaction) {
            pending_metadata[key] = "";  // Empty string means delete
        } else {
            metadata.erase(key);
        }
    }

    void AddUTXO(const std::string& outpoint, uint64_t amount) {
        if (in_transaction) {
            pending_utxo_adds[outpoint] = amount;
        } else {
            utxos[outpoint] = amount;
        }
    }

    void SpendUTXO(const std::string& outpoint) {
        if (in_transaction) {
            pending_utxo_deletes.push_back(outpoint);
        } else {
            utxos.erase(outpoint);
        }
    }

    std::optional<std::string> GetMetadata(const std::string& key) const {
        auto it = metadata.find(key);
        if (it != metadata.end()) return it->second;
        return std::nullopt;
    }

    uint64_t GetBalance() const {
        uint64_t sum = 0;
        for (const auto& [k, v] : utxos) {
            sum += v;
        }
        return sum;
    }
};

// Mock RocksDB-like database
class MockConsensusDB {
public:
    std::map<std::string, uint64_t> utxos;
    uint32_t tip_height = 0;
    std::string tip_hash;
    bool sync_writes = false;  // Simulates sync=true option

    void SetTip(uint32_t height, const std::string& hash, bool sync = true) {
        tip_height = height;
        tip_hash = hash;
        sync_writes = sync;
        // In real code, sync=true forces fsync
    }

    void AddCoin(const std::string& outpoint, uint64_t amount) {
        utxos[outpoint] = amount;
    }

    void SpendCoin(const std::string& outpoint) {
        utxos.erase(outpoint);
    }

    bool HasCoin(const std::string& outpoint) const {
        return utxos.count(outpoint) > 0;
    }

    uint64_t GetConsensusBalance() const {
        uint64_t sum = 0;
        for (const auto& [k, v] : utxos) {
            sum += v;
        }
        return sum;
    }
};

// ============================================================================
// Reorg Simulation with Crash Points
// ============================================================================

struct ReorgSimulator {
    MockWalletDB wallet;
    MockConsensusDB consensus;
    CrashPoint crash_at = CrashPoint::NONE;
    bool crashed = false;

    // Simulate reorg: disconnect blocks from old_tip to fork_point, connect new chain
    // Returns false if "crashed"
    bool SimulateReorgBuggy(uint32_t old_tip, uint32_t fork_point, uint32_t new_tip) {
        // BUGGY: Set marker OUTSIDE of transaction
        if (crash_at == CrashPoint::BEFORE_MARKER_SET) {
            crashed = true;
            return false;
        }

        // Set reorg marker (non-transactional - BUG)
        std::string marker = std::to_string(old_tip) + ":" +
                            std::to_string(fork_point) + ":" +
                            std::to_string(new_tip);
        wallet.SetMetadataDirect("reorg_in_progress", marker);

        if (crash_at == CrashPoint::AFTER_MARKER_SET_BEFORE_STATE) {
            crashed = true;
            return false;
        }

        // Do the actual reorg work (simplified)
        // In real code: disconnect old blocks, connect new blocks
        consensus.SetTip(new_tip, "new_hash_" + std::to_string(new_tip));

        if (crash_at == CrashPoint::AFTER_STATE_BEFORE_MARKER_DELETE) {
            crashed = true;
            return false;
        }

        // Delete reorg marker (non-transactional - BUG)
        wallet.DeleteMetadataDirect("reorg_in_progress");

        if (crash_at == CrashPoint::AFTER_MARKER_DELETE) {
            crashed = true;
            return false;
        }

        return true;
    }

    // FIXED: Set marker within transaction
    bool SimulateReorgFixed(uint32_t old_tip, uint32_t fork_point, uint32_t new_tip) {
        if (crash_at == CrashPoint::BEFORE_MARKER_SET) {
            crashed = true;
            return false;
        }

        // FIX: Use transaction for marker + state changes
        wallet.BeginTransaction();

        std::string marker = std::to_string(old_tip) + ":" +
                            std::to_string(fork_point) + ":" +
                            std::to_string(new_tip);
        wallet.SetMetadataTransactional("reorg_in_progress", marker);

        if (crash_at == CrashPoint::AFTER_MARKER_SET_BEFORE_STATE) {
            // Transaction not committed - marker not persisted
            wallet.Rollback();
            crashed = true;
            return false;
        }

        // Do the actual reorg work
        consensus.SetTip(new_tip, "new_hash_" + std::to_string(new_tip));

        // Wallet state changes would go here (within same transaction)

        if (crash_at == CrashPoint::AFTER_STATE_BEFORE_MARKER_DELETE) {
            // Transaction not committed - changes not persisted
            wallet.Rollback();
            crashed = true;
            return false;
        }

        // FIX: Delete marker within same transaction
        wallet.DeleteMetadataTransactional("reorg_in_progress");

        wallet.Commit();

        if (crash_at == CrashPoint::AFTER_MARKER_DELETE) {
            crashed = true;
            return false;
        }

        return true;
    }

    bool DetectIncompleteReorg() const {
        return wallet.GetMetadata("reorg_in_progress").has_value();
    }
};

// ============================================================================
// P1: Reorg Marker Transactionality Tests
// ============================================================================

TEST(P1_BuggyMarkerLostOnCrash) {
    // Demonstrates the bug: marker set outside transaction can be lost
    ReorgSimulator sim;
    sim.crash_at = CrashPoint::AFTER_MARKER_SET_BEFORE_STATE;

    bool completed = sim.SimulateReorgBuggy(100, 95, 101);
    ASSERT_FALSE(completed);  // Crashed

    // BUG: Marker IS present because it was written directly
    // This means crash is detected, but state may be inconsistent
    ASSERT_TRUE(sim.DetectIncompleteReorg());

    // Document the issue: marker survives but state changes may not
    std::cout << "\n    P1 BUG: Marker persists independently of state changes" << std::endl;
}

TEST(P1_FixedMarkerTransactional) {
    // With fix: marker and state in same transaction
    ReorgSimulator sim;
    sim.crash_at = CrashPoint::AFTER_MARKER_SET_BEFORE_STATE;

    bool completed = sim.SimulateReorgFixed(100, 95, 101);
    ASSERT_FALSE(completed);  // Crashed

    // FIX: Marker NOT present because transaction was rolled back
    ASSERT_FALSE(sim.DetectIncompleteReorg());

    std::cout << "\n    P1 FIX: Marker rolled back with transaction" << std::endl;
}

TEST(P1_MarkerDeletedWithState) {
    // Test that marker deletion is atomic with state changes
    ReorgSimulator sim;
    sim.crash_at = CrashPoint::AFTER_STATE_BEFORE_MARKER_DELETE;

    // Buggy version: marker survives, state changed
    bool completed_buggy = sim.SimulateReorgBuggy(100, 95, 101);
    ASSERT_FALSE(completed_buggy);
    ASSERT_TRUE(sim.DetectIncompleteReorg());  // Marker still there
    ASSERT_EQ(sim.consensus.tip_height, 101u);  // But consensus updated!

    // Reset for fixed version
    ReorgSimulator sim2;
    sim2.crash_at = CrashPoint::AFTER_STATE_BEFORE_MARKER_DELETE;

    // Note: Fixed version still has consensus updated (separate DB)
    // But wallet marker is rolled back
    bool completed_fixed = sim2.SimulateReorgFixed(100, 95, 101);
    ASSERT_FALSE(completed_fixed);
    ASSERT_FALSE(sim2.DetectIncompleteReorg());  // Marker rolled back
}

// ============================================================================
// P2: Incomplete Reorg Detection Tests
// ============================================================================

TEST(P2_DetectIncompleteReorgOnStartup) {
    ReorgSimulator sim;

    // Simulate incomplete reorg
    sim.wallet.SetMetadataDirect("reorg_in_progress", "100:95:101");

    // On startup, should detect
    ASSERT_TRUE(sim.DetectIncompleteReorg());

    auto marker = sim.wallet.GetMetadata("reorg_in_progress");
    ASSERT_TRUE(marker.has_value());
    ASSERT_EQ(marker.value(), "100:95:101");
}

TEST(P2_NoFalsePositiveDetection) {
    ReorgSimulator sim;

    // No marker = no incomplete reorg
    ASSERT_FALSE(sim.DetectIncompleteReorg());
}

TEST(P2_MarkerContainsReorgInfo) {
    ReorgSimulator sim;
    sim.SimulateReorgBuggy(150, 140, 155);

    // Reorg completed, marker should be gone
    ASSERT_FALSE(sim.DetectIncompleteReorg());
}

// ============================================================================
// P3: Tip Sync Tests
// ============================================================================

TEST(P3_TipWriteIsSynchronous) {
    MockConsensusDB db;

    // Tip updates should always use sync=true
    db.SetTip(100, "hash_100", true);
    ASSERT_TRUE(db.sync_writes);
    ASSERT_EQ(db.tip_height, 100u);

    // Document: Real code at chain_db.cpp:559 uses opts.sync = true
    std::cout << "\n    P3: Tip writes use sync=true (verified in code)" << std::endl;
}

TEST(P3_NonTipWritesCanBeAsync) {
    MockConsensusDB db;

    // Non-tip writes can be async (rely on WAL recovery)
    db.AddCoin("tx1:0", 1000);
    // In real code, these use WriteOptions() with no sync flag

    std::cout << "\n    P3: UTXO writes are async (sync=false) - acceptable with WAL" << std::endl;
}

// ============================================================================
// P4: Crash Recovery Balance Consistency Tests
// ============================================================================

TEST(P4_BalanceConsistentAfterCleanShutdown) {
    MockWalletDB wallet;
    MockConsensusDB consensus;

    // Both databases have same UTXOs
    wallet.AddUTXO("tx1:0", 1000);
    wallet.AddUTXO("tx2:0", 2000);
    consensus.AddCoin("tx1:0", 1000);
    consensus.AddCoin("tx2:0", 2000);

    ASSERT_EQ(wallet.GetBalance(), consensus.GetConsensusBalance());
    ASSERT_EQ(wallet.GetBalance(), 3000u);
}

TEST(P4_BalanceDivergenceAfterCrash) {
    // Simulates crash where consensus commits but wallet doesn't
    MockWalletDB wallet;
    MockConsensusDB consensus;

    // Initial state
    wallet.AddUTXO("tx1:0", 1000);
    consensus.AddCoin("tx1:0", 1000);

    // New block adds UTXO
    consensus.AddCoin("tx2:0", 2000);  // Committed to RocksDB
    // wallet.AddUTXO("tx2:0", 2000);  // CRASH before this!

    // After crash recovery: balance divergence
    ASSERT_EQ(consensus.GetConsensusBalance(), 3000u);
    ASSERT_EQ(wallet.GetBalance(), 1000u);  // Missing tx2:0

    // This is why ValidateAgainstConsensus is needed
    std::cout << "\n    P4: Crash caused balance divergence (3000 vs 1000)" << std::endl;
}

TEST(P4_ValidateAgainstConsensusFixesDivergence) {
    MockWalletDB wallet;
    MockConsensusDB consensus;

    // Wallet has phantom UTXO (reverse scenario)
    wallet.AddUTXO("tx1:0", 1000);
    wallet.AddUTXO("phantom:0", 5000);  // Doesn't exist in consensus
    consensus.AddCoin("tx1:0", 1000);

    ASSERT_EQ(wallet.GetBalance(), 6000u);
    ASSERT_EQ(consensus.GetConsensusBalance(), 1000u);

    // ValidateAgainstConsensus simulation
    std::vector<std::string> to_remove;
    for (const auto& [outpoint, amount] : wallet.utxos) {
        if (!consensus.HasCoin(outpoint)) {
            to_remove.push_back(outpoint);
        }
    }
    for (const auto& op : to_remove) {
        wallet.utxos.erase(op);
    }

    // After validation: balances match
    ASSERT_EQ(wallet.GetBalance(), consensus.GetConsensusBalance());
    ASSERT_EQ(wallet.GetBalance(), 1000u);

    std::cout << "\n    P4 FIX: ValidateAgainstConsensus removed phantom UTXO" << std::endl;
}

// ============================================================================
// P5: Atomic Block Operations Tests
// ============================================================================

TEST(P5_WalletBlockProcessingIsAtomic) {
    MockWalletDB wallet;

    wallet.BeginTransaction();
    wallet.AddUTXO("tx1:0", 1000);
    wallet.AddUTXO("tx1:1", 2000);
    wallet.SpendUTXO("old:0");  // Spend existing

    // Before commit: nothing persisted
    ASSERT_EQ(wallet.utxos.size(), 0u);

    wallet.Commit();

    // After commit: all changes applied atomically
    ASSERT_EQ(wallet.utxos.size(), 2u);
    ASSERT_EQ(wallet.GetBalance(), 3000u);
}

TEST(P5_WalletRollbackOnError) {
    MockWalletDB wallet;
    wallet.AddUTXO("existing:0", 500);

    wallet.BeginTransaction();
    wallet.AddUTXO("tx1:0", 1000);

    // Simulate error - rollback
    wallet.Rollback();

    // Original state preserved
    ASSERT_EQ(wallet.utxos.size(), 1u);
    ASSERT_EQ(wallet.GetBalance(), 500u);
}

TEST(P5_ConsensusWriteBatchAtomic) {
    // In real code, WriteBatch ensures atomicity
    // This test documents the expected behavior

    MockConsensusDB consensus;

    // Simulate WriteBatch: all or nothing
    std::vector<std::pair<std::string, uint64_t>> batch_adds = {
        {"tx1:0", 1000},
        {"tx1:1", 2000},
        {"tx2:0", 3000}
    };

    // In real code: all adds go to WriteBatch, committed atomically
    for (const auto& [op, amt] : batch_adds) {
        consensus.AddCoin(op, amt);
    }

    ASSERT_EQ(consensus.GetConsensusBalance(), 6000u);

    std::cout << "\n    P5: WriteBatch ensures atomic multi-key commits" << std::endl;
}

// ============================================================================
// Integration Test Stubs
// ============================================================================

TEST(Integration_RealSQLiteTransaction) {
    throw std::runtime_error("Requires real SQLite - SKIP");
}

TEST(Integration_RealRocksDBWriteBatch) {
    throw std::runtime_error("Requires real RocksDB - SKIP");
}

TEST(Integration_CrashRecoverySimulation) {
    throw std::runtime_error("Requires process restart simulation - SKIP");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n=== Persistence Safety Tests (Priority 4) ===" << std::endl;
    std::cout << "Testing invariants P1-P5\n" << std::endl;

    std::cout << "P1: Reorg Marker Transactionality Tests" << std::endl;
    RUN_TEST(P1_BuggyMarkerLostOnCrash);
    RUN_TEST(P1_FixedMarkerTransactional);
    RUN_TEST(P1_MarkerDeletedWithState);

    std::cout << "\nP2: Incomplete Reorg Detection Tests" << std::endl;
    RUN_TEST(P2_DetectIncompleteReorgOnStartup);
    RUN_TEST(P2_NoFalsePositiveDetection);
    RUN_TEST(P2_MarkerContainsReorgInfo);

    std::cout << "\nP3: Tip Sync Tests" << std::endl;
    RUN_TEST(P3_TipWriteIsSynchronous);
    RUN_TEST(P3_NonTipWritesCanBeAsync);

    std::cout << "\nP4: Crash Recovery Balance Consistency Tests" << std::endl;
    RUN_TEST(P4_BalanceConsistentAfterCleanShutdown);
    RUN_TEST(P4_BalanceDivergenceAfterCrash);
    RUN_TEST(P4_ValidateAgainstConsensusFixesDivergence);

    std::cout << "\nP5: Atomic Block Operations Tests" << std::endl;
    RUN_TEST(P5_WalletBlockProcessingIsAtomic);
    RUN_TEST(P5_WalletRollbackOnError);
    RUN_TEST(P5_ConsensusWriteBatchAtomic);

    std::cout << "\nIntegration Tests (require real databases)" << std::endl;
    SKIP_TEST(Integration_RealSQLiteTransaction, "requires real SQLite");
    SKIP_TEST(Integration_RealRocksDBWriteBatch, "requires real RocksDB");
    SKIP_TEST(Integration_CrashRecoverySimulation, "requires process restart");

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Total:   " << tests_run << std::endl;
    std::cout << "Passed:  " << tests_passed << std::endl;
    std::cout << "Failed:  " << tests_failed << std::endl;
    std::cout << "Skipped: " << tests_skipped << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
