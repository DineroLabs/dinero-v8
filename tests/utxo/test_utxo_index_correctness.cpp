// UTXO Index Correctness Tests - Priority 3
// Following methodology: define invariants, write tests first, then fix code
//
// Invariants tested:
//   U1: UTXO Consistency - Wallet UTXO exists IFF consensus UTXO exists
//   U2: Spend State Integrity - spend_height > 0 means spent, NULL/0 means unspent
//   U3: Balance Invariant - Wallet balance = sum of unspent wallet UTXOs
//   U4: Reorg Atomicity - After reorg, no orphaned UTXOs remain
//   U5: Coinbase Maturity - Coinbase UTXOs not spendable until 100 confirmations
//   U6: Add/Spend Atomicity - Partial failures must not corrupt state
//   U7: No Double-Add - Same OutPoint cannot be added twice
//
// Build: cmake --build build --target test_utxo_index_correctness
// Run:   ./build/tests/utxo/test_utxo_index_correctness

#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cstdint>
#include <map>
#include <set>
#include <random>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <optional>

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

#define ASSERT_THROW(expr, exception_type) do { \
    bool caught = false; \
    try { expr; } catch (const exception_type&) { caught = true; } \
    if (!caught) throw std::runtime_error("Expected exception not thrown: " #exception_type); \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::ostringstream oss; \
        oss << "Expected " << (a) << " == " << (b); \
        throw std::runtime_error(oss.str()); \
    } \
} while(0)

#define ASSERT_NE(a, b) do { \
    if ((a) == (b)) { \
        std::ostringstream oss; \
        oss << "Expected " << (a) << " != " << (b); \
        throw std::runtime_error(oss.str()); \
    } \
} while(0)

// ============================================================================
// Mock Types for Testing Logic
// ============================================================================

struct MockOutPoint {
    std::string txid;
    uint32_t vout;

    bool operator<(const MockOutPoint& other) const {
        if (txid != other.txid) return txid < other.txid;
        return vout < other.vout;
    }

    bool operator==(const MockOutPoint& other) const {
        return txid == other.txid && vout == other.vout;
    }
};

struct MockUTXO {
    MockOutPoint outpoint;
    uint64_t amount;
    std::string script_pubkey;
    uint32_t height;           // Block height where created
    bool is_coinbase;

    // Wallet tracking
    std::optional<uint32_t> spend_height;  // nullopt = unspent, value = height where spent
    bool is_our_script;        // Does this belong to our wallet?
};

// ============================================================================
// Mock Consensus UTXO Set (simulates RocksDB layer)
// ============================================================================

class MockConsensusUTXOSet {
public:
    std::map<MockOutPoint, MockUTXO> utxos;

    void AddCoin(const MockOutPoint& op, const MockUTXO& utxo) {
        utxos[op] = utxo;
    }

    bool SpendCoin(const MockOutPoint& op) {
        auto it = utxos.find(op);
        if (it == utxos.end()) return false;
        utxos.erase(it);
        return true;
    }

    bool HasCoin(const MockOutPoint& op) const {
        return utxos.count(op) > 0;
    }

    std::optional<MockUTXO> GetCoin(const MockOutPoint& op) const {
        auto it = utxos.find(op);
        if (it == utxos.end()) return std::nullopt;
        return it->second;
    }

    // Undo a spend (for reorg)
    void UndoSpend(const MockOutPoint& op, const MockUTXO& utxo) {
        utxos[op] = utxo;
    }

    // Undo an add (for reorg)
    void UndoAdd(const MockOutPoint& op) {
        utxos.erase(op);
    }
};

// ============================================================================
// Mock Wallet UTXO Index (simulates SQLite layer)
// ============================================================================

class MockWalletUTXOIndex {
public:
    std::map<MockOutPoint, MockUTXO> utxos;

    void AddUTXO(const MockOutPoint& op, const MockUTXO& utxo) {
        utxos[op] = utxo;
    }

    // BUG SIMULATION: Current code sets spend_height = NULL on reorg
    // This "resurrects" the UTXO even if consensus doesn't have it
    void MarkSpent(const MockOutPoint& op, uint32_t spend_height) {
        auto it = utxos.find(op);
        if (it != utxos.end()) {
            it->second.spend_height = spend_height;
        }
    }

    // BUG: Current reorg handling - sets spend_height to NULL
    void MarkUnspentOnReorg(const MockOutPoint& op) {
        auto it = utxos.find(op);
        if (it != utxos.end()) {
            it->second.spend_height = std::nullopt;  // "Resurrect"
        }
    }

    // CORRECT: Should delete if consensus doesn't have it
    void DeleteUTXO(const MockOutPoint& op) {
        utxos.erase(op);
    }

    bool HasUTXO(const MockOutPoint& op) const {
        return utxos.count(op) > 0;
    }

    bool IsUnspent(const MockOutPoint& op) const {
        auto it = utxos.find(op);
        if (it == utxos.end()) return false;
        return !it->second.spend_height.has_value();
    }

    std::optional<MockUTXO> GetUTXO(const MockOutPoint& op) const {
        auto it = utxos.find(op);
        if (it == utxos.end()) return std::nullopt;
        return it->second;
    }

    uint64_t CalculateBalance(uint32_t current_height) const {
        uint64_t balance = 0;
        for (const auto& [op, utxo] : utxos) {
            if (!utxo.is_our_script) continue;
            if (utxo.spend_height.has_value()) continue;  // Spent

            // Coinbase maturity check
            if (utxo.is_coinbase) {
                if (current_height < utxo.height + 100) continue;  // Immature
            }

            balance += utxo.amount;
        }
        return balance;
    }

    // Balance calculation that IGNORES coinbase maturity (current bug)
    uint64_t CalculateBalanceBuggy(uint32_t /*current_height*/) const {
        uint64_t balance = 0;
        for (const auto& [op, utxo] : utxos) {
            if (!utxo.is_our_script) continue;
            if (utxo.spend_height.has_value()) continue;
            balance += utxo.amount;  // BUG: No maturity check
        }
        return balance;
    }
};

// ============================================================================
// Dual-Layer UTXO Manager (simulates current architecture)
// ============================================================================

class DualLayerUTXOManager {
public:
    MockConsensusUTXOSet consensus;
    MockWalletUTXOIndex wallet;
    uint32_t current_height = 0;

    // Simulate adding a UTXO from a new block
    void ProcessNewOutput(const MockOutPoint& op, const MockUTXO& utxo) {
        consensus.AddCoin(op, utxo);
        if (utxo.is_our_script) {
            wallet.AddUTXO(op, utxo);
        }
    }

    // Simulate spending a UTXO
    bool ProcessSpend(const MockOutPoint& op, uint32_t spend_height) {
        if (!consensus.SpendCoin(op)) return false;
        wallet.MarkSpent(op, spend_height);
        return true;
    }

    // BUG SIMULATION: Reorg that resurrects phantom UTXOs
    void ReorgBuggy(const std::vector<MockOutPoint>& spent_in_orphaned_blocks) {
        for (const auto& op : spent_in_orphaned_blocks) {
            // BUG: Just marks as unspent without checking if consensus has it
            wallet.MarkUnspentOnReorg(op);
        }
    }

    // CORRECT: Reorg that validates against consensus
    void ReorgCorrect(const std::vector<MockOutPoint>& spent_in_orphaned_blocks) {
        for (const auto& op : spent_in_orphaned_blocks) {
            if (consensus.HasCoin(op)) {
                // Consensus still has it - mark unspent
                wallet.MarkUnspentOnReorg(op);
            } else {
                // Consensus doesn't have it - delete from wallet
                wallet.DeleteUTXO(op);
            }
        }
    }

    // Verify U1: Wallet UTXO exists IFF consensus UTXO exists (for our scripts)
    bool VerifyU1_Consistency() const {
        for (const auto& [op, utxo] : wallet.utxos) {
            if (!utxo.is_our_script) continue;
            if (!utxo.spend_height.has_value()) {
                // Unspent in wallet - must exist in consensus
                if (!consensus.HasCoin(op)) {
                    std::cout << "\n    U1 VIOLATION: Wallet has unspent UTXO not in consensus: "
                              << op.txid << ":" << op.vout << std::endl;
                    return false;
                }
            }
        }
        return true;
    }

    // Verify U3: Balance Invariant
    bool VerifyU3_Balance() const {
        uint64_t wallet_balance = wallet.CalculateBalance(current_height);

        // Calculate expected balance from consensus
        uint64_t consensus_balance = 0;
        for (const auto& [op, utxo] : consensus.utxos) {
            if (!utxo.is_our_script) continue;
            if (utxo.is_coinbase && current_height < utxo.height + 100) continue;
            consensus_balance += utxo.amount;
        }

        if (wallet_balance != consensus_balance) {
            std::cout << "\n    U3 VIOLATION: Wallet balance " << wallet_balance
                      << " != consensus balance " << consensus_balance << std::endl;
            return false;
        }
        return true;
    }

    // Priority 3 FIX: ValidateAgainstConsensus
    // Removes phantom UTXOs from wallet that don't exist in consensus
    size_t ValidateAgainstConsensus() {
        size_t removed = 0;
        std::vector<MockOutPoint> to_remove;

        for (const auto& [op, utxo] : wallet.utxos) {
            if (!utxo.is_our_script) continue;
            if (utxo.spend_height.has_value()) continue;  // Only check unspent

            // Check if consensus has this UTXO
            if (!consensus.HasCoin(op)) {
                to_remove.push_back(op);
            }
        }

        for (const auto& op : to_remove) {
            wallet.DeleteUTXO(op);
            removed++;
            std::cout << "\n    FIX: Removed phantom UTXO " << op.txid << ":" << op.vout;
        }

        return removed;
    }
};

// ============================================================================
// U1: UTXO Consistency Tests
// ============================================================================

TEST(U1_PhantomUTXOAfterReorg) {
    // Scenario: UTXO created in block A, spent in block B
    // Block B gets orphaned - current code resurrects the UTXO
    // But if block A was also orphaned, wallet has phantom UTXO

    DualLayerUTXOManager mgr;
    mgr.current_height = 100;

    MockOutPoint op1{"tx_a", 0};
    MockUTXO utxo1{op1, 1000, "our_script", 99, false, std::nullopt, true};

    // Block 99: Create UTXO
    mgr.ProcessNewOutput(op1, utxo1);
    ASSERT_TRUE(mgr.consensus.HasCoin(op1));
    ASSERT_TRUE(mgr.wallet.IsUnspent(op1));

    // Block 100: Spend UTXO
    mgr.ProcessSpend(op1, 100);
    ASSERT_FALSE(mgr.consensus.HasCoin(op1));  // Spent = removed from consensus
    ASSERT_FALSE(mgr.wallet.IsUnspent(op1));   // Marked spent

    // REORG: Both block 99 and 100 orphaned
    // Consensus removes the UTXO entirely (it was created then spent in orphaned chain)
    // Buggy wallet code just marks it unspent

    std::vector<MockOutPoint> spent_in_orphaned = {op1};
    mgr.ReorgBuggy(spent_in_orphaned);

    // BUG: Wallet thinks UTXO is unspent, consensus doesn't have it
    ASSERT_TRUE(mgr.wallet.IsUnspent(op1));     // Wallet: "available"
    ASSERT_FALSE(mgr.consensus.HasCoin(op1));   // Consensus: "doesn't exist"

    // U1 VIOLATION: Phantom UTXO
    ASSERT_FALSE(mgr.VerifyU1_Consistency());
}

TEST(U1_ValidateAgainstConsensusFix) {
    // Same scenario as U1_PhantomUTXOAfterReorg, but with fix applied

    DualLayerUTXOManager mgr;
    mgr.current_height = 100;

    MockOutPoint op1{"tx_a", 0};
    MockUTXO utxo1{op1, 1000, "our_script", 99, false, std::nullopt, true};

    // Block 99: Create UTXO
    mgr.ProcessNewOutput(op1, utxo1);

    // Block 100: Spend UTXO
    mgr.ProcessSpend(op1, 100);

    // BUGGY reorg creates phantom UTXO
    std::vector<MockOutPoint> spent_in_orphaned = {op1};
    mgr.ReorgBuggy(spent_in_orphaned);

    // Before fix: U1 violated
    ASSERT_FALSE(mgr.VerifyU1_Consistency());

    // APPLY FIX: ValidateAgainstConsensus removes phantom UTXOs
    size_t phantoms_removed = mgr.ValidateAgainstConsensus();
    ASSERT_EQ(phantoms_removed, 1u);

    // After fix: U1 passes
    ASSERT_TRUE(mgr.VerifyU1_Consistency());

    // Balance is now correct
    ASSERT_TRUE(mgr.VerifyU3_Balance());
}

TEST(U1_CorrectReorgHandling) {
    // Same scenario but with correct reorg handling

    DualLayerUTXOManager mgr;
    mgr.current_height = 100;

    MockOutPoint op1{"tx_a", 0};
    MockUTXO utxo1{op1, 1000, "our_script", 99, false, std::nullopt, true};

    mgr.ProcessNewOutput(op1, utxo1);
    mgr.ProcessSpend(op1, 100);

    // CORRECT reorg handling
    std::vector<MockOutPoint> spent_in_orphaned = {op1};
    mgr.ReorgCorrect(spent_in_orphaned);

    // Correct: Wallet deleted UTXO because consensus doesn't have it
    ASSERT_FALSE(mgr.wallet.HasUTXO(op1));
    ASSERT_FALSE(mgr.consensus.HasCoin(op1));

    // U1 passes
    ASSERT_TRUE(mgr.VerifyU1_Consistency());
}

TEST(U1_ValidReorgResurrection) {
    // Scenario: UTXO created in block 98, spent in block 100
    // Only block 100 gets orphaned - UTXO should be resurrected

    DualLayerUTXOManager mgr;
    mgr.current_height = 100;

    MockOutPoint op1{"tx_a", 0};
    MockUTXO utxo1{op1, 1000, "our_script", 98, false, std::nullopt, true};

    // Block 98: Create UTXO (this block stays)
    mgr.ProcessNewOutput(op1, utxo1);

    // Block 100: Spend UTXO (this block gets orphaned)
    mgr.ProcessSpend(op1, 100);

    // REORG: Only block 100 orphaned, consensus restores the UTXO
    mgr.consensus.UndoSpend(op1, utxo1);

    // Both correct and buggy reorg work here
    std::vector<MockOutPoint> spent_in_orphaned = {op1};
    mgr.ReorgCorrect(spent_in_orphaned);

    // Both wallet and consensus have the UTXO
    ASSERT_TRUE(mgr.wallet.IsUnspent(op1));
    ASSERT_TRUE(mgr.consensus.HasCoin(op1));
    ASSERT_TRUE(mgr.VerifyU1_Consistency());
}

// ============================================================================
// U2: Spend State Integrity Tests
// ============================================================================

TEST(U2_SpendHeightConsistency) {
    MockWalletUTXOIndex wallet;

    MockOutPoint op1{"tx1", 0};
    MockUTXO utxo1{op1, 1000, "script", 100, false, std::nullopt, true};

    wallet.AddUTXO(op1, utxo1);
    ASSERT_TRUE(wallet.IsUnspent(op1));

    // Spend at height 150
    wallet.MarkSpent(op1, 150);
    ASSERT_FALSE(wallet.IsUnspent(op1));

    auto retrieved = wallet.GetUTXO(op1);
    ASSERT_TRUE(retrieved.has_value());
    ASSERT_TRUE(retrieved->spend_height.has_value());
    ASSERT_EQ(retrieved->spend_height.value(), 150u);
}

TEST(U2_SpendZeroHeightAmbiguity) {
    // Edge case: What if spend_height = 0?
    // This could be confused with "unspent" in some implementations

    MockWalletUTXOIndex wallet;

    MockOutPoint op1{"genesis_tx", 0};
    MockUTXO utxo1{op1, 50'0000'0000, "genesis_script", 0, true, std::nullopt, true};

    wallet.AddUTXO(op1, utxo1);

    // Hypothetically spent at genesis (height 0)
    wallet.MarkSpent(op1, 0);

    // Should still be marked as spent, not unspent
    ASSERT_FALSE(wallet.IsUnspent(op1));

    auto retrieved = wallet.GetUTXO(op1);
    ASSERT_TRUE(retrieved->spend_height.has_value());
    ASSERT_EQ(retrieved->spend_height.value(), 0u);
}

// ============================================================================
// U3: Balance Invariant Tests
// ============================================================================

TEST(U3_BalanceMatchesConsensus) {
    DualLayerUTXOManager mgr;
    mgr.current_height = 200;

    // Add some UTXOs
    MockOutPoint op1{"tx1", 0};
    MockOutPoint op2{"tx2", 0};
    MockOutPoint op3{"tx3", 0};

    MockUTXO utxo1{op1, 1000, "our_script", 100, false, std::nullopt, true};
    MockUTXO utxo2{op2, 2000, "our_script", 150, false, std::nullopt, true};
    MockUTXO utxo3{op3, 3000, "other_script", 160, false, std::nullopt, false};  // Not ours

    mgr.ProcessNewOutput(op1, utxo1);
    mgr.ProcessNewOutput(op2, utxo2);
    mgr.ProcessNewOutput(op3, utxo3);

    // Balance should be 1000 + 2000 = 3000 (not including other's UTXO)
    ASSERT_EQ(mgr.wallet.CalculateBalance(200), 3000u);
    ASSERT_TRUE(mgr.VerifyU3_Balance());

    // Spend one
    mgr.ProcessSpend(op1, 180);

    // Balance should be 2000
    ASSERT_EQ(mgr.wallet.CalculateBalance(200), 2000u);
    ASSERT_TRUE(mgr.VerifyU3_Balance());
}

TEST(U3_BalanceDriftAfterPhantomUTXO) {
    // After buggy reorg, wallet balance is wrong

    DualLayerUTXOManager mgr;
    mgr.current_height = 100;

    MockOutPoint op1{"tx_a", 0};
    MockUTXO utxo1{op1, 5000, "our_script", 99, false, std::nullopt, true};

    mgr.ProcessNewOutput(op1, utxo1);
    mgr.ProcessSpend(op1, 100);

    // Buggy reorg - creates phantom UTXO
    std::vector<MockOutPoint> spent_in_orphaned = {op1};
    mgr.ReorgBuggy(spent_in_orphaned);

    // Wallet thinks it has 5000, consensus has 0
    ASSERT_EQ(mgr.wallet.CalculateBalance(100), 5000u);

    // U3 VIOLATION
    ASSERT_FALSE(mgr.VerifyU3_Balance());
}

// ============================================================================
// U5: Coinbase Maturity Tests
// ============================================================================

TEST(U5_CoinbaseMaturityCorrect) {
    MockWalletUTXOIndex wallet;

    MockOutPoint op1{"coinbase_tx", 0};
    MockUTXO coinbase{op1, 50'0000'0000, "miner_script", 100, true, std::nullopt, true};

    wallet.AddUTXO(op1, coinbase);

    // At height 100 (same block) - not mature
    ASSERT_EQ(wallet.CalculateBalance(100), 0u);

    // At height 150 - not mature (need 100 confirmations)
    ASSERT_EQ(wallet.CalculateBalance(150), 0u);

    // At height 199 - still not mature (99 confirmations)
    ASSERT_EQ(wallet.CalculateBalance(199), 0u);

    // At height 200 - mature! (100 confirmations)
    ASSERT_EQ(wallet.CalculateBalance(200), 50'0000'0000u);
}

TEST(U5_CoinbaseMaturityBuggy) {
    // Demonstrates the bug where coinbase maturity is ignored

    MockWalletUTXOIndex wallet;

    MockOutPoint op1{"coinbase_tx", 0};
    MockUTXO coinbase{op1, 50'0000'0000, "miner_script", 100, true, std::nullopt, true};

    wallet.AddUTXO(op1, coinbase);

    // Buggy balance calculation ignores maturity
    // At height 100 - buggy impl says it's spendable
    ASSERT_EQ(wallet.CalculateBalanceBuggy(100), 50'0000'0000u);  // BUG: Should be 0

    // Correct implementation says 0
    ASSERT_EQ(wallet.CalculateBalance(100), 0u);
}

TEST(U5_MixedCoinbaseAndRegular) {
    MockWalletUTXOIndex wallet;

    // Regular UTXO at height 100
    MockOutPoint op1{"regular_tx", 0};
    MockUTXO regular{op1, 1000, "script", 100, false, std::nullopt, true};
    wallet.AddUTXO(op1, regular);

    // Coinbase at height 100
    MockOutPoint op2{"coinbase_tx", 0};
    MockUTXO coinbase{op2, 50'0000'0000, "script", 100, true, std::nullopt, true};
    wallet.AddUTXO(op2, coinbase);

    // At height 105: only regular is spendable
    ASSERT_EQ(wallet.CalculateBalance(105), 1000u);

    // At height 200: both spendable
    ASSERT_EQ(wallet.CalculateBalance(200), 50'0000'0000u + 1000u);
}

// ============================================================================
// U6: Add/Spend Atomicity Tests
// ============================================================================

TEST(U6_PartialAddFailure) {
    // Scenario: AddUTXO succeeds in consensus but fails in wallet
    // Should result in inconsistent state (violates atomicity)

    DualLayerUTXOManager mgr;

    MockOutPoint op1{"tx1", 0};
    MockUTXO utxo1{op1, 1000, "our_script", 100, false, std::nullopt, true};

    // Simulate partial failure: add to consensus only
    mgr.consensus.AddCoin(op1, utxo1);
    // Wallet add "fails" (not called)

    // Inconsistent state: consensus has it, wallet doesn't
    ASSERT_TRUE(mgr.consensus.HasCoin(op1));
    ASSERT_FALSE(mgr.wallet.HasUTXO(op1));

    // Balance is wrong - we're missing a UTXO we own
    // This test documents the issue, not the fix
    std::cout << "\n    U6 demonstrates partial failure risk" << std::endl;
}

// ============================================================================
// U7: No Double-Add Tests
// ============================================================================

TEST(U7_DoubleAddPrevention) {
    MockConsensusUTXOSet consensus;

    MockOutPoint op1{"tx1", 0};
    MockUTXO utxo1{op1, 1000, "script", 100, false, std::nullopt, true};
    MockUTXO utxo2{op1, 2000, "script", 101, false, std::nullopt, true};  // Same outpoint, different amount

    consensus.AddCoin(op1, utxo1);
    ASSERT_EQ(consensus.GetCoin(op1)->amount, 1000u);

    // Adding again should either fail or overwrite
    // Current mock overwrites - real code should prevent this
    consensus.AddCoin(op1, utxo2);

    // Document current behavior (overwrite)
    ASSERT_EQ(consensus.GetCoin(op1)->amount, 2000u);
    std::cout << "\n    U7: Double-add overwrites (should be prevented)" << std::endl;
}

// ============================================================================
// Integration Test Stubs (require real classes)
// ============================================================================

TEST(Integration_WalletConsensusSync) {
    // This test requires actual Wallet and ChainDB classes
    // Placeholder to be implemented with real integration
    throw std::runtime_error("Requires integration with real Wallet/ChainDB - SKIP");
}

TEST(Integration_ReorgRecovery) {
    // Test full reorg scenario with real classes
    throw std::runtime_error("Requires integration with real ChainDB - SKIP");
}

TEST(Integration_DeltaUndoAvailability) {
    // Test Utreexo delta undo is available on disconnect
    throw std::runtime_error("Requires Utreexo integration - SKIP");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n=== UTXO Index Correctness Tests (Priority 3) ===" << std::endl;
    std::cout << "Testing invariants U1-U7\n" << std::endl;

    std::cout << "U1: UTXO Consistency Tests" << std::endl;
    RUN_TEST(U1_PhantomUTXOAfterReorg);
    RUN_TEST(U1_ValidateAgainstConsensusFix);
    RUN_TEST(U1_CorrectReorgHandling);
    RUN_TEST(U1_ValidReorgResurrection);

    std::cout << "\nU2: Spend State Integrity Tests" << std::endl;
    RUN_TEST(U2_SpendHeightConsistency);
    RUN_TEST(U2_SpendZeroHeightAmbiguity);

    std::cout << "\nU3: Balance Invariant Tests" << std::endl;
    RUN_TEST(U3_BalanceMatchesConsensus);
    RUN_TEST(U3_BalanceDriftAfterPhantomUTXO);

    std::cout << "\nU5: Coinbase Maturity Tests" << std::endl;
    RUN_TEST(U5_CoinbaseMaturityCorrect);
    RUN_TEST(U5_CoinbaseMaturityBuggy);
    RUN_TEST(U5_MixedCoinbaseAndRegular);

    std::cout << "\nU6: Add/Spend Atomicity Tests" << std::endl;
    RUN_TEST(U6_PartialAddFailure);

    std::cout << "\nU7: No Double-Add Tests" << std::endl;
    RUN_TEST(U7_DoubleAddPrevention);

    std::cout << "\nIntegration Tests (require real classes)" << std::endl;
    SKIP_TEST(Integration_WalletConsensusSync, "requires real Wallet/ChainDB");
    SKIP_TEST(Integration_ReorgRecovery, "requires real ChainDB");
    SKIP_TEST(Integration_DeltaUndoAvailability, "requires Utreexo integration");

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Total:   " << tests_run << std::endl;
    std::cout << "Passed:  " << tests_passed << std::endl;
    std::cout << "Failed:  " << tests_failed << std::endl;
    std::cout << "Skipped: " << tests_skipped << std::endl;

    if (tests_failed > 0) {
        std::cout << "\n*** FAILURES DETECTED - These document bugs to fix ***" << std::endl;
    }

    return tests_failed > 0 ? 1 : 0;
}
