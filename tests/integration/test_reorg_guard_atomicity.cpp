/**
 * Day 2.2: ReorgGuard Atomicity Test
 *
 * Tests the RAII guard for atomic reorg commits:
 * 1. Normal case: commit() called → changes persisted
 * 2. Discard case: commit() NOT called → changes discarded
 * 3. Partial state verification: no half-committed state
 *
 * ReorgGuard guarantees:
 * - All changes committed atomically (UTXO + tip + block index)
 * - OR no changes committed at all (guard destroyed without commit)
 * - NO partial state possible
 *
 * This is crash safety at the LOGICAL level (before RocksDB persistence).
 */

#include "integration_test_runner.h"
#include <string>
#include <map>
#include <memory>
#include <stdexcept>

using namespace dinero::test;

//=============================================================================
// Mock Types for ReorgGuard Testing
//=============================================================================

/**
 * MockWriteBatch - Tracks operations for verification
 */
class MockWriteBatch {
public:
    void recordTipUpdate(const std::string& hash, int height, uint64_t work) {
        tip_hash = hash;
        tip_height = height;
        tip_work = work;
        tip_updated = true;
    }

    void recordUTXOFlush(size_t dirty_count) {
        utxo_dirty_count = dirty_count;
        utxo_flushed = true;
    }

    void clear() {
        tip_updated = false;
        utxo_flushed = false;
        tip_hash = "";
        tip_height = 0;
        tip_work = 0;
        utxo_dirty_count = 0;
    }

    bool tip_updated = false;
    bool utxo_flushed = false;
    std::string tip_hash;
    int tip_height = 0;
    uint64_t tip_work = 0;
    size_t utxo_dirty_count = 0;
};

/**
 * MockChainDB - Simulates ChainDB for testing
 */
class MockChainDB {
public:
    bool setTip(const std::string& hash, int height, uint64_t work, MockWriteBatch* batch) {
        if (inject_tip_failure_) {
            return false;
        }

        if (batch) {
            batch->recordTipUpdate(hash, height, work);
        }
        return true;
    }

    bool writeBatch(MockWriteBatch* batch, bool sync) {
        if (inject_write_failure_) {
            return false;
        }

        if (!batch) return false;

        // Commit is atomic - either all changes go through or none
        if (batch->tip_updated && batch->utxo_flushed) {
            committed_tip_hash_ = batch->tip_hash;
            committed_tip_height_ = batch->tip_height;
            committed_tip_work_ = batch->tip_work;
            committed_utxo_count_ = batch->utxo_dirty_count;
            commit_count_++;
            batch->clear();
            return true;
        }

        return false;
    }

    // Test helpers
    void injectTipFailure(bool inject) { inject_tip_failure_ = inject; }
    void injectWriteFailure(bool inject) { inject_write_failure_ = inject; }

    std::string getCommittedTipHash() const { return committed_tip_hash_; }
    int getCommittedTipHeight() const { return committed_tip_height_; }
    size_t getCommitCount() const { return commit_count_; }

private:
    bool inject_tip_failure_ = false;
    bool inject_write_failure_ = false;
    std::string committed_tip_hash_;
    int committed_tip_height_ = 0;
    uint64_t committed_tip_work_ = 0;
    size_t committed_utxo_count_ = 0;
    size_t commit_count_ = 0;
};

/**
 * MockUTXOSet - Simulates UTXOSet for testing
 */
class MockUTXOSet {
public:
    void markDirty(size_t count) {
        dirty_count_ = count;
    }

    bool flush(MockWriteBatch* batch) {
        if (inject_flush_failure_) {
            return false;
        }

        if (batch && dirty_count_ > 0) {
            batch->recordUTXOFlush(dirty_count_);
            dirty_count_ = 0;  // Dirty set cleared after flush
            return true;
        }

        return dirty_count_ == 0;  // Success if nothing to flush
    }

    void injectFlushFailure(bool inject) { inject_flush_failure_ = inject; }
    size_t getDirtyCount() const { return dirty_count_; }

private:
    size_t dirty_count_ = 0;
    bool inject_flush_failure_ = false;
};

/**
 * SimpleReorgGuard - Simplified ReorgGuard for testing
 *
 * Implements the same RAII semantics as the production ReorgGuard:
 * - Constructor prepares batch
 * - Destructor discards batch if not committed
 * - commit() atomically flushes all changes
 */
class SimpleReorgGuard {
public:
    SimpleReorgGuard(MockChainDB& chain_db, MockUTXOSet& utxo_set)
        : chain_db_(chain_db)
        , utxo_set_(utxo_set)
        , batch_(new MockWriteBatch())
        , committed_(false) {
    }

    ~SimpleReorgGuard() {
        if (!committed_) {
            // Batch automatically discarded
            // This is intentional - reorg did not complete successfully
            delete batch_;
        } else {
            delete batch_;
        }
    }

    // Disable copy/move
    SimpleReorgGuard(const SimpleReorgGuard&) = delete;
    SimpleReorgGuard& operator=(const SimpleReorgGuard&) = delete;

    /**
     * Commit all changes atomically
     *
     * Returns false on failure (simulates std::terminate in production)
     */
    bool commit(const std::string& new_tip_hash, int new_height, uint64_t new_work) {
        // Step 1: Flush UTXO changes to batch
        if (!utxo_set_.flush(batch_)) {
            return false;  // In production, this would std::terminate()
        }

        // Step 2: Add tip update to batch
        if (!chain_db_.setTip(new_tip_hash, new_height, new_work, batch_)) {
            return false;  // In production, this would std::terminate()
        }

        // Step 3: Commit batch atomically (all-or-nothing)
        if (!chain_db_.writeBatch(batch_, true)) {
            return false;  // In production, this would std::terminate()
        }

        committed_ = true;
        return true;
    }

    bool isCommitted() const { return committed_; }

private:
    MockChainDB& chain_db_;
    MockUTXOSet& utxo_set_;
    MockWriteBatch* batch_;
    bool committed_;
};

//=============================================================================
// Test 1: Normal Case - Commit Succeeds
//=============================================================================

bool TestNormalCommit() {
    std::cout << "\n========================================\n";
    std::cout << "Test: Normal Commit (All-or-Nothing)\n";
    std::cout << "========================================\n";

    MockChainDB chain_db;
    MockUTXOSet utxo_set;

    std::cout << "  [Setting up reorg state...]\n";

    // Simulate dirty UTXO state (5 coins modified)
    utxo_set.markDirty(5);
    std::cout << "  [✅ Dirty UTXO count: " << utxo_set.getDirtyCount() << "]\n";

    {
        SimpleReorgGuard guard(chain_db, utxo_set);
        std::cout << "  [✅ ReorgGuard created (batch prepared)]\n";

        // Commit the reorg
        std::cout << "  [Committing reorg...]\n";
        bool success = guard.commit("block_new_tip", 100, 12345);

        ASSERT_TRUE(success);
        ASSERT_TRUE(guard.isCommitted());
        std::cout << "  [✅ Reorg committed successfully]\n";

    }  // Guard destroyed here

    // Verify state after guard destruction
    std::cout << "\n  [Verifying committed state...]\n";
    ASSERT_TRUE(chain_db.getCommittedTipHash() == "block_new_tip");
    ASSERT_EQ(chain_db.getCommittedTipHeight(), 100);
    ASSERT_EQ(chain_db.getCommitCount(), static_cast<size_t>(1));
    ASSERT_EQ(utxo_set.getDirtyCount(), static_cast<size_t>(0));  // Dirty set cleared

    std::cout << "  [✅ Tip hash: " << chain_db.getCommittedTipHash() << "]\n";
    std::cout << "  [✅ Tip height: " << chain_db.getCommittedTipHeight() << "]\n";
    std::cout << "  [✅ Commit count: " << chain_db.getCommitCount() << "]\n";
    std::cout << "  [✅ UTXO dirty count: " << utxo_set.getDirtyCount() << " (cleaned)]\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: Normal Commit\n";
    std::cout << "========================================\n";

    return true;
}

//=============================================================================
// Test 2: Discard Case - Commit NOT Called
//=============================================================================

bool TestDiscardWithoutCommit() {
    std::cout << "\n========================================\n";
    std::cout << "Test: Discard Without Commit\n";
    std::cout << "========================================\n";

    MockChainDB chain_db;
    MockUTXOSet utxo_set;

    std::cout << "  [Setting up reorg state...]\n";

    // Simulate dirty UTXO state
    utxo_set.markDirty(5);
    std::cout << "  [✅ Dirty UTXO count: " << utxo_set.getDirtyCount() << "]\n";

    size_t commits_before = chain_db.getCommitCount();

    {
        SimpleReorgGuard guard(chain_db, utxo_set);
        std::cout << "  [✅ ReorgGuard created (batch prepared)]\n";

        // Do NOT call commit() - simulate reorg failure
        std::cout << "  [⚠️  Simulating reorg failure - NOT calling commit()]\n";

    }  // Guard destroyed here - batch should be discarded

    // Verify NO state changes were committed
    std::cout << "\n  [Verifying no changes committed...]\n";
    ASSERT_EQ(chain_db.getCommitCount(), commits_before);  // No new commits
    ASSERT_TRUE(chain_db.getCommittedTipHash().empty());   // Tip not updated

    std::cout << "  [✅ No commits occurred: " << chain_db.getCommitCount() << "]\n";
    std::cout << "  [✅ Tip hash empty (not updated)]\n";
    std::cout << "  [✅ Batch was discarded automatically]\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: Discard Without Commit\n";
    std::cout << "========================================\n";

    return true;
}

//=============================================================================
// Test 3: UTXO Flush Failure - No Partial Commit
//=============================================================================

bool TestUTXOFlushFailure() {
    std::cout << "\n========================================\n";
    std::cout << "Test: UTXO Flush Failure (No Partial State)\n";
    std::cout << "========================================\n";

    MockChainDB chain_db;
    MockUTXOSet utxo_set;

    std::cout << "  [Setting up reorg state...]\n";

    // Simulate dirty UTXO state
    utxo_set.markDirty(5);

    // Inject UTXO flush failure
    utxo_set.injectFlushFailure(true);
    std::cout << "  [⚠️  Injected UTXO flush failure]\n";

    size_t commits_before = chain_db.getCommitCount();

    {
        SimpleReorgGuard guard(chain_db, utxo_set);
        std::cout << "  [✅ ReorgGuard created]\n";

        // Try to commit - should fail at UTXO flush
        std::cout << "  [Attempting commit (will fail)...]\n";
        bool success = guard.commit("block_new_tip", 100, 12345);

        ASSERT_FALSE(success);  // Commit should fail
        ASSERT_FALSE(guard.isCommitted());
        std::cout << "  [✅ Commit failed as expected]\n";

    }  // Guard destroyed - batch discarded

    // Verify NO partial state was committed
    std::cout << "\n  [Verifying no partial state...]\n";
    ASSERT_EQ(chain_db.getCommitCount(), commits_before);  // No new commits
    ASSERT_TRUE(chain_db.getCommittedTipHash().empty());   // Tip not updated

    std::cout << "  [✅ No commits occurred: " << chain_db.getCommitCount() << "]\n";
    std::cout << "  [✅ Tip NOT updated (no partial state)]\n";
    std::cout << "  [✅ All-or-nothing semantics verified]\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: UTXO Flush Failure\n";
    std::cout << "========================================\n";

    return true;
}

//=============================================================================
// Test 4: Tip Update Failure - No Partial Commit
//=============================================================================

bool TestTipUpdateFailure() {
    std::cout << "\n========================================\n";
    std::cout << "Test: Tip Update Failure (No Partial State)\n";
    std::cout << "========================================\n";

    MockChainDB chain_db;
    MockUTXOSet utxo_set;

    std::cout << "  [Setting up reorg state...]\n";

    // Simulate dirty UTXO state
    utxo_set.markDirty(5);

    // Inject tip update failure
    chain_db.injectTipFailure(true);
    std::cout << "  [⚠️  Injected tip update failure]\n";

    size_t commits_before = chain_db.getCommitCount();

    {
        SimpleReorgGuard guard(chain_db, utxo_set);
        std::cout << "  [✅ ReorgGuard created]\n";

        // Try to commit - should fail at tip update
        std::cout << "  [Attempting commit (will fail)...]\n";
        bool success = guard.commit("block_new_tip", 100, 12345);

        ASSERT_FALSE(success);  // Commit should fail
        ASSERT_FALSE(guard.isCommitted());
        std::cout << "  [✅ Commit failed as expected]\n";

    }  // Guard destroyed - batch discarded

    // Verify NO partial state was committed
    std::cout << "\n  [Verifying no partial state...]\n";
    ASSERT_EQ(chain_db.getCommitCount(), commits_before);  // No new commits
    ASSERT_TRUE(chain_db.getCommittedTipHash().empty());   // Tip not updated

    std::cout << "  [✅ No commits occurred: " << chain_db.getCommitCount() << "]\n";
    std::cout << "  [✅ Tip NOT updated (no partial state)]\n";
    std::cout << "  [✅ UTXO flush succeeded but tip update failed → batch discarded]\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: Tip Update Failure\n";
    std::cout << "========================================\n";

    return true;
}

//=============================================================================
// Test 5: WriteBatch Failure - No Partial Commit
//=============================================================================

bool TestWriteBatchFailure() {
    std::cout << "\n========================================\n";
    std::cout << "Test: WriteBatch Failure (No Partial State)\n";
    std::cout << "========================================\n";

    MockChainDB chain_db;
    MockUTXOSet utxo_set;

    std::cout << "  [Setting up reorg state...]\n";

    // Simulate dirty UTXO state
    utxo_set.markDirty(5);

    // Inject write batch failure
    chain_db.injectWriteFailure(true);
    std::cout << "  [⚠️  Injected WriteBatch commit failure]\n";

    size_t commits_before = chain_db.getCommitCount();

    {
        SimpleReorgGuard guard(chain_db, utxo_set);
        std::cout << "  [✅ ReorgGuard created]\n";

        // Try to commit - should fail at WriteBatch commit
        std::cout << "  [Attempting commit (will fail)...]\n";
        bool success = guard.commit("block_new_tip", 100, 12345);

        ASSERT_FALSE(success);  // Commit should fail
        ASSERT_FALSE(guard.isCommitted());
        std::cout << "  [✅ Commit failed as expected]\n";

    }  // Guard destroyed - batch discarded

    // Verify NO partial state was committed
    std::cout << "\n  [Verifying no partial state...]\n";
    ASSERT_EQ(chain_db.getCommitCount(), commits_before);  // No new commits
    ASSERT_TRUE(chain_db.getCommittedTipHash().empty());   // Tip not updated

    std::cout << "  [✅ No commits occurred: " << chain_db.getCommitCount() << "]\n";
    std::cout << "  [✅ Tip NOT updated (no partial state)]\n";
    std::cout << "  [✅ UTXO + Tip prepared but WriteBatch failed → all discarded]\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: WriteBatch Failure\n";
    std::cout << "========================================\n";

    return true;
}

//=============================================================================
// Main
//=============================================================================

int main() {
    IntegrationTestRunner runner;

    std::cout << "════════════════════════════════════════\n";
    std::cout << "  Day 2.2: ReorgGuard Atomicity Tests\n";
    std::cout << "  All-or-Nothing Commit Semantics\n";
    std::cout << "════════════════════════════════════════\n";

    runner.RunTest("Normal Commit (All-or-Nothing)", TestNormalCommit);
    runner.RunTest("Discard Without Commit", TestDiscardWithoutCommit);
    runner.RunTest("UTXO Flush Failure (No Partial State)", TestUTXOFlushFailure);
    runner.RunTest("Tip Update Failure (No Partial State)", TestTipUpdateFailure);
    runner.RunTest("WriteBatch Failure (No Partial State)", TestWriteBatchFailure);

    runner.PrintSummary();

    return runner.AllTestsPassed() ? 0 : 1;
}
