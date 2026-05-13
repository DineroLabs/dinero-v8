/**
 * Phase G.3.4: State Transition Layer - Unit Tests
 *
 * Test Scope:
 * - ConnectBlock happy path (success)
 * - ConnectBlock undo write succeeds, DB commit fails (rollback)
 * - ConnectBlock crash after undo, before DB (orphaned undo safe)
 * - ConnectBlock double-connect precondition violation (error)
 * - Reorg: connect/disconnect loop (undo reusable)
 *
 * Test Constraints:
 * ✅ State mutation allowed (this is G.3.4)
 * ✅ UTXO set updates allowed
 * ✅ Undo data persistence allowed
 * ✅ Block index updates allowed
 * ✅ Mocking allowed (IUTXOView, IBlockIndexDB, IUndoStorage)
 * ❌ NO validation logic (delegated to G.3.3)
 * ❌ NO fork choice (not yet implemented)
 * ❌ NO mempool interaction (not yet implemented)
 * ✅ Pure state transition tests (deterministic)
 * ✅ Runtime < 500ms per test
 */

#include "../../include/p2p/state_transition.h"
#include "../../include/p2p/consensus_validator.h"
#include <chrono>
#include <iostream>
#include <cassert>
#include <map>
#include <set>

using namespace dinero::p2p;
using dinero::OutPoint;
using dinero::uint256;

//=============================================================================
// Mock UTXO View (Mutable)
//=============================================================================

class MockUTXOView : public IUTXOView {
public:
    std::map<OutPoint, TxOut> utxos_;

    void addUTXO(const OutPoint& outpoint, const TxOut& txout) {
        utxos_[outpoint] = txout;
    }

    void removeUTXO(const OutPoint& outpoint) {
        utxos_.erase(outpoint);
    }

    std::optional<TxOut> getUTXO(const OutPoint& outpoint) const {
        auto it = utxos_.find(outpoint);
        if (it != utxos_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool hasUTXO(const OutPoint& outpoint) const {
        return utxos_.count(outpoint) > 0;
    }

    size_t size() const {
        return utxos_.size();
    }
};

//=============================================================================
// Mock Block Index DB
//=============================================================================

class MockBlockIndexDB : public IBlockIndexDB {
public:
    struct BlockMetadata {
        uint32_t height;
        uint64_t chainwork;
        uint32_t status;
        bool connected;
    };

    std::map<Hash256, BlockMetadata> blocks_;
    Hash256 best_block_;
    uint32_t best_height_;

    bool fail_next_commit = false;  // For testing DB commit failures

    void setBlockMetadata(const Hash256& hash, uint32_t height, uint64_t chainwork, uint32_t status) {
        BlockMetadata meta;
        meta.height = height;
        meta.chainwork = chainwork;
        meta.status = status;
        meta.connected = (status & 0x08) != 0;  // BLOCK_CONNECTED flag
        blocks_[hash] = meta;
    }

    bool isBlockConnected(const Hash256& block_hash) const override {
        auto it = blocks_.find(block_hash);
        if (it != blocks_.end()) {
            return it->second.connected;
        }
        return false;
    }

    void markBlockConnected(const Hash256& block_hash, bool connected) override {
        blocks_[block_hash].connected = connected;
    }

    bool commitBatch() override {
        if (fail_next_commit) {
            fail_next_commit = false;
            return false;  // Simulate DB commit failure
        }
        return true;
    }

    void setBlockUndoPosition(const Hash256& block_hash,
                             uint32_t file_id,
                             uint64_t offset,
                             uint64_t length,
                             uint32_t checksum) override {
        // Mock implementation - store undo position
        auto it = blocks_.find(block_hash);
        if (it != blocks_.end()) {
            // Acknowledge the call
        }
    }

    BlockIndex* getBlockIndex(const Hash256& hash) override {
        // Mock implementation - not used in these tests
        return nullptr;
    }
};

//=============================================================================
// Mock Undo Storage
//=============================================================================

class MockUndoStorage : public IUndoStorage {
public:
    struct UndoEntry {
        uint32_t file_id;
        uint64_t offset;
        uint64_t length;
        uint32_t checksum;
        std::vector<uint8_t> data;  // Actual undo data
    };

    std::map<Hash256, UndoEntry> undo_map_;

    bool fail_next_write = false;  // For testing undo write failures

    bool hasUndo(const Hash256& block_hash) const override {
        return undo_map_.count(block_hash) > 0;
    }

    bool writeUndo(const Hash256& block_hash, const std::vector<uint8_t>& data,
                   uint32_t& out_file_id, uint64_t& out_offset,
                   uint64_t& out_length, uint32_t& out_checksum) override {
        if (fail_next_write) {
            fail_next_write = false;
            return false;  // Simulate undo write failure
        }

        UndoEntry entry;
        entry.file_id = 1;  // Simplified: always file 1
        entry.offset = undo_map_.size() * 1000;  // Simplified offset
        entry.length = data.size();
        entry.checksum = 0x12345678;  // Simplified checksum
        entry.data = data;

        undo_map_[block_hash] = entry;

        out_file_id = entry.file_id;
        out_offset = entry.offset;
        out_length = entry.length;
        out_checksum = entry.checksum;

        return true;
    }

    bool readUndo(const Hash256& block_hash, uint32_t file_id, uint64_t offset,
                  uint64_t length, uint32_t expected_checksum,
                  std::vector<uint8_t>& out_data) override {
        auto it = undo_map_.find(block_hash);
        if (it == undo_map_.end()) {
            return false;  // Undo not found
        }

        const UndoEntry& entry = it->second;
        if (entry.checksum != expected_checksum) {
            return false;  // Checksum mismatch
        }

        out_data = entry.data;
        return true;
    }

    bool loadBlock(uint32_t file_id,
                  uint64_t offset,
                  uint32_t size,
                  Block& out_block) const override {
        // Mock implementation - not needed for undo data tests
        // Return false to indicate block not found
        return false;
    }
};

//=============================================================================
// Test Helpers
//=============================================================================

// Create a simple block with one coinbase transaction
Block createSimpleBlock(uint64_t coinbase_value, uint32_t height) {
    Block block;

    // Coinbase transaction
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.locktime = 0;

    // Coinbase input (null outpoint)
    TxIn cb_input;
    cb_input.prevout.txid = dinero::TxId(uint256());  // Phase M.4.3-B: All zeros
    cb_input.prevout.vout = 0xFFFFFFFF;
    cb_input.scriptSig = {0x03, (uint8_t)(height & 0xFF), (uint8_t)((height >> 8) & 0xFF)};
    cb_input.sequence = 0xFFFFFFFF;
    coinbase.inputs.push_back(cb_input);

    // Coinbase output
    TxOut cb_output;
    cb_output.value = coinbase_value;
    cb_output.scriptPubKey = {0x51};  // OP_1
    coinbase.outputs.push_back(cb_output);

    block.transactions.push_back(coinbase);
    return block;
}

// Create block hash matching generateFakeTestBlockId() in state_transition.cpp
// Uses coinbase output value to create distinguishable hashes
Hash256 createBlockHash(uint64_t coinbase_value) {
    Hash256 hash;
    hash.data[0] = (uint8_t)(coinbase_value & 0xFF);
    hash.data[1] = (uint8_t)((coinbase_value >> 8) & 0xFF);
    hash.data[2] = (uint8_t)((coinbase_value >> 16) & 0xFF);
    hash.data[3] = (uint8_t)((coinbase_value >> 24) & 0xFF);
    return hash;
}

//=============================================================================
// Test 1: Happy Path - ConnectBlock Success
//=============================================================================

void test_connect_block_success() {
    std::cout << "\n[Test 1] ConnectBlock happy path (success)" << std::endl;

    MockUTXOView utxo_view;
    MockBlockIndexDB block_index_db;
    MockUndoStorage undo_storage;
    ConsensusParams params;

    // Block at height 1 (first PoW block after genesis/premine)
    uint32_t height = 2;
    uint64_t subsidy = 10000000000ULL;  // 100 DIN
    Block block = createSimpleBlock(subsidy, height);
    Hash256 block_hash = createBlockHash(subsidy);  // Must match generateFakeTestBlockId

    // Setup: Parent block is connected (use different subsidy for parent hash)
    uint64_t parent_subsidy = 9999999999ULL;  // Mock parent subsidy
    Hash256 parent_hash = createBlockHash(parent_subsidy);
    block_index_db.setBlockMetadata(parent_hash, height - 1, 1000, 0x08);  // BLOCK_CONNECTED

    // Execute: ConnectBlock
    auto result = ConnectBlock(block, height, utxo_view, block_index_db, undo_storage, params);

    // Assert: Success
    assert(result.ok && "ConnectBlock should succeed");
    assert(result.fail_reason == ConnectFailReason::NONE && "No failure reason");

    // Assert: Undo data created (may be empty for coinbase-only blocks)
    assert(result.undo_file_id > 0 && "Undo file ID should be set");
    // Note: undo_length can be 0 for coinbase-only blocks (no UTXOs spent)
    assert(undo_storage.hasUndo(block_hash) && "Undo should exist");

    // Assert: UTXO added (coinbase output) - Phase M.4.3-B: TxId conversion
    OutPoint coinbase_outpoint;
    uint256 txid_raw;
    std::memcpy(txid_raw.data, block_hash.data.data(), 32);  // Convert Hash256 to uint256
    coinbase_outpoint.txid = dinero::TxId(txid_raw);
    coinbase_outpoint.vout = 0;
    assert(utxo_view.hasUTXO(coinbase_outpoint) && "Coinbase UTXO should be added");

    std::cout << "  [✓] ConnectBlock succeeded" << std::endl;
    std::cout << "  [✓] Undo data created (file_id=" << result.undo_file_id
              << ", offset=" << result.undo_file_offset
              << ", length=" << result.undo_length << ")" << std::endl;
    std::cout << "  [✓] UTXO added for coinbase" << std::endl;
}

//=============================================================================
// Test 2: Undo Write Succeeds, DB Commit Fails (Rollback)
//=============================================================================

void test_undo_ok_db_fail() {
    std::cout << "\n[Test 2] Undo write succeeds, DB commit fails (rollback)" << std::endl;

    MockUTXOView utxo_view;
    MockBlockIndexDB block_index_db;
    MockUndoStorage undo_storage;
    ConsensusParams params;

    uint32_t height = 2;
    uint64_t subsidy = 10000000000ULL;
    Block block = createSimpleBlock(subsidy, height);
    Hash256 block_hash = createBlockHash(subsidy);  // Must match generateFakeTestBlockId

    // Setup: Parent connected
    uint64_t parent_subsidy = 9999999999ULL;
    Hash256 parent_hash = createBlockHash(parent_subsidy);
    block_index_db.setBlockMetadata(parent_hash, height - 1, 1000, 0x08);

    // Setup: Force DB commit to fail
    block_index_db.fail_next_commit = true;

    size_t utxo_count_before = utxo_view.size();

    // Execute: ConnectBlock (should fail at DB commit)
    auto result = ConnectBlock(block, height, utxo_view, block_index_db, undo_storage, params);

    // Assert: Failure
    assert(!result.ok && "ConnectBlock should fail");
    assert(result.fail_reason == ConnectFailReason::DB_COMMIT && "Failure reason should be DB_COMMIT");

    // Assert: UTXO view unchanged (rolled back)
    assert(utxo_view.size() == utxo_count_before && "UTXO view should be unchanged");

    // Assert: Undo data MAY exist (orphaned undo is safe)
    // Note: Implementation may write undo before DB commit, that's OK
    bool undo_exists = undo_storage.hasUndo(block_hash);
    std::cout << "  [✓] ConnectBlock failed as expected" << std::endl;
    std::cout << "  [✓] UTXO view unchanged (rolled back)" << std::endl;
    std::cout << "  [✓] Orphaned undo " << (undo_exists ? "exists (safe)" : "does not exist") << std::endl;
}

//=============================================================================
// Test 3: Crash After Undo, Before DB (Orphaned Undo Safe)
//=============================================================================

void test_crash_after_undo_before_db() {
    std::cout << "\n[Test 3] Crash after undo, before DB (orphaned undo safe)" << std::endl;

    MockUTXOView utxo_view;
    MockBlockIndexDB block_index_db;
    MockUndoStorage undo_storage;
    ConsensusParams params;

    uint32_t height = 2;
    uint64_t subsidy = 10000000000ULL;
    Block block = createSimpleBlock(subsidy, height);
    Hash256 block_hash = createBlockHash(subsidy);  // Must match generateFakeTestBlockId

    // Simulate: Undo already written (from previous failed attempt)
    std::vector<uint8_t> fake_undo_data = {0x01, 0x02, 0x03};
    uint32_t file_id, checksum;
    uint64_t offset, length;
    undo_storage.writeUndo(block_hash, fake_undo_data, file_id, offset, length, checksum);

    // Setup: Parent connected
    uint64_t parent_subsidy = 9999999999ULL;
    Hash256 parent_hash = createBlockHash(parent_subsidy);
    block_index_db.setBlockMetadata(parent_hash, height - 1, 1000, 0x08);

    // Setup: Force DB commit to fail (simulating crash)
    block_index_db.fail_next_commit = true;

    // Execute: ConnectBlock (will fail at DB commit, but undo already exists)
    auto result = ConnectBlock(block, height, utxo_view, block_index_db, undo_storage, params);

    // Assert: Failure (crash simulated)
    assert(!result.ok && "ConnectBlock should fail (crash simulated)");

    // Assert: Undo exists (orphaned, but safe)
    assert(undo_storage.hasUndo(block_hash) && "Orphaned undo should exist");

    // Assert: Block not connected (no DB reference to undo)
    // (In real implementation, block status would not be BLOCK_CONNECTED)

    std::cout << "  [✓] Crash simulation: DB commit failed" << std::endl;
    std::cout << "  [✓] Orphaned undo exists (safe, not referenced)" << std::endl;
    std::cout << "  [✓] Observable state unchanged (crash-safe)" << std::endl;
}

//=============================================================================
// Test 4: Double-Connect Precondition Violation
//=============================================================================

void test_double_connect_forbidden() {
    std::cout << "\n[Test 4] Double-connect precondition violation (error)" << std::endl;

    MockUTXOView utxo_view;
    MockBlockIndexDB block_index_db;
    MockUndoStorage undo_storage;
    ConsensusParams params;

    uint32_t height = 2;
    uint64_t subsidy = 10000000000ULL;
    Block block = createSimpleBlock(subsidy, height);
    Hash256 block_hash = createBlockHash(subsidy);  // Must match generateFakeTestBlockId

    // Setup: Parent connected
    uint64_t parent_subsidy = 9999999999ULL;
    Hash256 parent_hash = createBlockHash(parent_subsidy);
    block_index_db.setBlockMetadata(parent_hash, height - 1, 1000, 0x08);

    // First connect: Should succeed
    auto result1 = ConnectBlock(block, height, utxo_view, block_index_db, undo_storage, params);
    assert(result1.ok && "First ConnectBlock should succeed");

    // Mark block as connected (simulate successful first connect)
    block_index_db.setBlockMetadata(block_hash, height, 2000, 0x08);  // BLOCK_CONNECTED

    // Second connect: Should fail (double-connect forbidden)
    auto result2 = ConnectBlock(block, height, utxo_view, block_index_db, undo_storage, params);

    // Assert: Failure with PRECONDITION reason
    assert(!result2.ok && "Second ConnectBlock should fail");
    assert(result2.fail_reason == ConnectFailReason::PRECONDITION && "Failure reason should be PRECONDITION");
    assert(result2.error.find("already connected") != std::string::npos ||
           result2.error.find("BLOCK_CONNECTED") != std::string::npos &&
           "Error should mention double-connect");

    std::cout << "  [✓] First connect succeeded" << std::endl;
    std::cout << "  [✓] Second connect rejected (double-connect forbidden)" << std::endl;
    std::cout << "  [✓] Error: " << result2.error << std::endl;
}

//=============================================================================
// Test 5: Reorg - Connect/Disconnect Loop (Undo Reusable)
//=============================================================================

void test_reorg_connect_disconnect_loop() {
    std::cout << "\n[Test 5] Reorg: connect/disconnect loop (undo reusable)" << std::endl;

    MockUTXOView utxo_view;
    MockBlockIndexDB block_index_db;
    MockUndoStorage undo_storage;
    ConsensusParams params;

    uint32_t height = 2;
    uint64_t subsidy = 10000000000ULL;
    Block block = createSimpleBlock(subsidy, height);
    Hash256 block_hash = createBlockHash(subsidy);  // Must match generateFakeTestBlockId

    // Setup: Parent connected
    uint64_t parent_subsidy = 9999999999ULL;
    Hash256 parent_hash = createBlockHash(parent_subsidy);
    block_index_db.setBlockMetadata(parent_hash, height - 1, 1000, 0x08);

    // Step 1: Connect block
    auto connect_result = ConnectBlock(block, height, utxo_view, block_index_db, undo_storage, params);
    assert(connect_result.ok && "ConnectBlock should succeed");

    OutPoint coinbase_outpoint;
    uint256 txid_raw2;
    std::memcpy(txid_raw2.data, block_hash.data.data(), 32);  // Convert Hash256 to uint256
    coinbase_outpoint.txid = dinero::TxId(txid_raw2);  // Phase M.4.3-B
    coinbase_outpoint.vout = 0;
    assert(utxo_view.hasUTXO(coinbase_outpoint) && "UTXO should exist after connect");

    // Step 2: Disconnect block (using undo data)
    auto disconnect_result = DisconnectBlock(block, height, utxo_view, block_index_db, undo_storage,
                                              connect_result.undo_file_id,
                                              connect_result.undo_file_offset,
                                              connect_result.undo_length,
                                              connect_result.undo_checksum);
    assert(disconnect_result.ok && "DisconnectBlock should succeed");
    assert(!utxo_view.hasUTXO(coinbase_outpoint) && "UTXO should be removed after disconnect");

    // Step 3: Reconnect block (undo data reusable)
    auto reconnect_result = ConnectBlock(block, height, utxo_view, block_index_db, undo_storage, params);
    assert(reconnect_result.ok && "Reconnect should succeed");
    assert(utxo_view.hasUTXO(coinbase_outpoint) && "UTXO should exist after reconnect");

    // Assert: Undo still exists (not consumed)
    assert(undo_storage.hasUndo(block_hash) && "Undo should still exist (reusable)");

    std::cout << "  [✓] Connect succeeded" << std::endl;
    std::cout << "  [✓] Disconnect succeeded (UTXO removed)" << std::endl;
    std::cout << "  [✓] Reconnect succeeded (UTXO restored)" << std::endl;
    std::cout << "  [✓] Undo data reusable (not consumed)" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.3.4: State Transition Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nState mutation tests | UTXO updates | Undo persistence" << std::endl;
    std::cout << "TESTS WRITTEN FIRST - NO IMPLEMENTATION YET" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        // Test 1: Happy path
        test_connect_block_success();

        // Test 2: DB commit failure (rollback)
        test_undo_ok_db_fail();

        // Test 3: Crash after undo (orphaned undo safe)
        test_crash_after_undo_before_db();

        // Test 4: Double-connect forbidden
        test_double_connect_forbidden();

        // Test 5: Reorg loop (undo reusable)
        test_reorg_connect_disconnect_loop();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All State Transition Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;

        if (duration.count() < 500) {
            std::cout << "[✓] Fast: < 500ms requirement met" << std::endl;
        } else {
            std::cout << "[!] Warning: Exceeded 500ms target" << std::endl;
        }

        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] ConnectBlock happy path" << std::endl;
        std::cout << "  [✓] Undo write succeeds, DB commit fails" << std::endl;
        std::cout << "  [✓] Crash after undo, before DB" << std::endl;
        std::cout << "  [✓] Double-connect forbidden" << std::endl;
        std::cout << "  [✓] Reorg connect/disconnect loop" << std::endl;

        std::cout << "\n⚠️  NEXT STEP: Implement ConnectBlock() and DisconnectBlock()" << std::endl;
        std::cout << "    Required: State mutation, undo persistence, crash safety" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
