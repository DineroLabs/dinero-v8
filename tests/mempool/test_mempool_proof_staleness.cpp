/**
 * Mempool Proof Staleness Tests (#2)
 *
 * Verifies that:
 * 1. onBlockConnected evicts conflicting TXs and marks remaining as stale
 * 2. onBlockDisconnected marks all TXs stale
 * 3. refreshProof clears staleness
 * 4. Stats correctly reflect staleness counts
 */

#include "daemon/mempool.h"
#include "primitives/block.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <stdexcept>

using namespace dinero;

// ============================================================================
// Test Infrastructure
// ============================================================================

static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while(0)

// ============================================================================
// Helpers
// ============================================================================

// Create a dummy TxId from an integer seed
static TxId makeTxId(uint32_t seed) {
    TxId id;
    std::memset(id.v.data, 0, 32);
    std::memcpy(id.v.data, &seed, 4);
    return id;
}

// Create a test transaction that spends given outpoints
static Transaction makeTx(const std::vector<std::pair<TxId, uint32_t>>& inputs,
                           uint64_t output_value = 1000) {
    Transaction tx;
    tx.version = 2;
    tx.witness_version = 0xFF;
    for (const auto& [txid, vout] : inputs) {
        TxInput inp;
        inp.prevout.txid = txid;
        inp.prevout.vout = vout;
        tx.vin.push_back(inp);
    }
    TxOutput out;
    out.value = AmountUna::Una(output_value);
    out.scriptPubKey = {0x51, 0x20};
    out.scriptPubKey.resize(34, 0x00);
    tx.vout.push_back(out);
    return tx;
}

// Create a coinbase block spending given outpoints (non-coinbase tx)
static Block makeBlock(uint32_t height,
                       const std::vector<Transaction>& extra_txs = {}) {
    Block block;
    std::memset(&block.header, 0, sizeof(BlockHeader));
    block.header.version = 1;
    block.header.timestamp = 1700000000 + height * 600;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = height;

    // Coinbase tx
    Transaction coinbase;
    coinbase.version = 2;
    coinbase.witness_version = 0xFF;
    TxInput cb_in;
    cb_in.prevout.vout = 0xFFFFFFFF;
    cb_in.scriptSig.resize(4);
    std::memcpy(cb_in.scriptSig.data(), &height, 4);
    coinbase.vin.push_back(cb_in);
    TxOutput cb_out;
    cb_out.value = AmountUna::Una(5000);
    cb_out.scriptPubKey = {0x51, 0x20};
    cb_out.scriptPubKey.resize(34, 0x00);
    coinbase.vout.push_back(cb_out);
    block.vtx.push_back(coinbase);

    for (const auto& tx : extra_txs) {
        block.vtx.push_back(tx);
    }

    return block;
}

static std::vector<uint8_t> makeFakeRoot(uint8_t seed) {
    std::vector<uint8_t> root(32, seed);
    return root;
}

// Add a TX to the mempool bypassing validation (no ChainDB needed)
// Returns the txid
static uint256 addTxToMempool(Mempool& pool, const Transaction& tx) {
    pool.addUnchecked(tx);
    return tx.GetTxid().AsUint256();
}

// ============================================================================
// Tests
// ============================================================================

// Test 1: staleness marked on block connect
static void test_staleness_marked_on_block_connect() {
    std::cout << "Test 1: staleness marked on block connect..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(1), 0}});
    uint256 txid = addTxToMempool(pool, tx);

    // Set proof metadata
    auto root1 = makeFakeRoot(0x01);
    pool.refreshProof(txid, root1, 10);

    // Verify not stale initially
    TEST_ASSERT(pool.getStaleCount() == 0, "No stale TXs initially");

    // Connect a block (different root)
    Block block = makeBlock(11);
    auto root2 = makeFakeRoot(0x02);
    pool.onBlockConnected(block, 11, root2);

    TEST_ASSERT(pool.getStaleCount() == 1, "TX should be stale after block connect");

    std::cout << "  PASSED" << std::endl;
}

// Test 2: conflict eviction on block connect
static void test_conflict_eviction() {
    std::cout << "Test 2: conflict eviction on block connect..." << std::endl;

    Mempool pool(nullptr);
    TxId spent_txid = makeTxId(100);
    auto tx = makeTx({{spent_txid, 0}});
    uint256 txid = addTxToMempool(pool, tx);

    TEST_ASSERT(pool.size() == 1, "Pool should have 1 TX");

    // Block that also spends the same input
    Transaction block_spend = makeTx({{spent_txid, 0}}, 500);
    Block block = makeBlock(11, {block_spend});

    size_t evicted = pool.onBlockConnected(block, 11, makeFakeRoot(0x03));
    TEST_ASSERT(evicted == 1, "Should evict 1 conflicting TX");
    TEST_ASSERT(pool.size() == 0, "Pool should be empty after eviction");

    std::cout << "  PASSED" << std::endl;
}

// Test 3: confirmed TX removed on block connect
static void test_confirmed_tx_removed() {
    std::cout << "Test 3: confirmed TX removed on block connect..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(200), 0}});
    uint256 txid = addTxToMempool(pool, tx);

    TEST_ASSERT(pool.size() == 1, "Pool should have 1 TX");

    // Block containing the same TX
    Block block = makeBlock(12);
    block.vtx.push_back(tx);

    pool.onBlockConnected(block, 12, makeFakeRoot(0x04));
    TEST_ASSERT(pool.size() == 0, "TX should be removed as confirmed");

    std::cout << "  PASSED" << std::endl;
}

// Test 4: no staleness without root
static void test_no_staleness_without_root() {
    std::cout << "Test 4: no staleness without root..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(300), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    pool.refreshProof(txid, makeFakeRoot(0x05), 10);

    // Connect block with EMPTY root (non-CSN mode)
    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, {});

    TEST_ASSERT(pool.getStaleCount() == 0, "No staleness marking without root");

    std::cout << "  PASSED" << std::endl;
}

static void test_addunchecked_rejects_private_transactions() {
    std::cout << "Test 4b: addUnchecked rejects confidential transactions..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(350), 0}});
    if (!tx.vout.empty()) {
        tx.vout[0].is_confidential = true;
    }

    bool threw = false;
    try {
        pool.addUnchecked(tx);
    } catch (const std::logic_error&) {
        threw = true;
    }

    TEST_ASSERT(threw, "addUnchecked should reject confidential transactions");
    TEST_ASSERT(pool.size() == 0, "Rejected confidential transaction must not enter the mempool");

    std::cout << "  PASSED" << std::endl;
}

// Test 5: refresh clears staleness
static void test_refresh_clears_staleness() {
    std::cout << "Test 5: refresh clears staleness..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(400), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    pool.refreshProof(txid, makeFakeRoot(0x06), 10);

    // Make stale
    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, makeFakeRoot(0x07));
    TEST_ASSERT(pool.getStaleCount() == 1, "Should be stale");

    // Refresh
    pool.refreshProof(txid, makeFakeRoot(0x07), 11);
    TEST_ASSERT(pool.getStaleCount() == 0, "Should be fresh after refresh");

    std::cout << "  PASSED" << std::endl;
}

// Test 6: stale count accurate
static void test_stale_count_accurate() {
    std::cout << "Test 6: stale count accurate..." << std::endl;

    Mempool pool(nullptr);
    std::vector<uint256> txids;
    for (int i = 0; i < 5; i++) {
        auto tx = makeTx({{makeTxId(500 + i), 0}});
        uint256 txid = addTxToMempool(pool, tx);
        pool.refreshProof(txid, makeFakeRoot(0x08), 10);
        txids.push_back(txid);
    }

    TEST_ASSERT(pool.size() == 5, "Should have 5 TXs");

    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, makeFakeRoot(0x09));
    TEST_ASSERT(pool.getStaleCount() == 5, "All 5 should be stale");

    std::cout << "  PASSED" << std::endl;
}

// Test 7: get stale TX IDs
static void test_get_stale_tx_ids() {
    std::cout << "Test 7: get stale TX IDs..." << std::endl;

    Mempool pool(nullptr);
    auto tx1 = makeTx({{makeTxId(600), 0}});
    auto tx2 = makeTx({{makeTxId(601), 0}});
    uint256 txid1 = addTxToMempool(pool, tx1);
    uint256 txid2 = addTxToMempool(pool, tx2);
    pool.refreshProof(txid1, makeFakeRoot(0x0A), 10);
    // tx2 has no proof metadata — won't be marked stale

    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, makeFakeRoot(0x0B));

    auto stale_ids = pool.getStaleTxIds();
    TEST_ASSERT(stale_ids.size() == 1, "Only TX with proof metadata should be stale");
    TEST_ASSERT(stale_ids[0] == txid1, "Stale TX should be tx1");

    std::cout << "  PASSED" << std::endl;
}

// Test 8: stats include staleness
static void test_stats_include_staleness() {
    std::cout << "Test 8: stats include staleness..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(700), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    pool.refreshProof(txid, makeFakeRoot(0x0C), 10);

    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, makeFakeRoot(0x0D));

    auto stats = pool.getStats();
    TEST_ASSERT(stats.stale_tx_count == 1, "Stats should show 1 stale TX");
    TEST_ASSERT(stats.last_connected_height == 11, "Stats should show height 11");

    std::cout << "  PASSED" << std::endl;
}

// Test 9: block disconnect marks stale
static void test_block_disconnect_marks_stale() {
    std::cout << "Test 9: block disconnect marks stale..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(800), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    pool.refreshProof(txid, makeFakeRoot(0x0E), 10);
    TEST_ASSERT(pool.getStaleCount() == 0, "Fresh initially");

    Block block = makeBlock(10);
    pool.onBlockDisconnected(block, 10);
    TEST_ASSERT(pool.getStaleCount() == 1, "Stale after disconnect");

    std::cout << "  PASSED" << std::endl;
}

// Test 10: multiple blocks cumulative
static void test_multiple_blocks_cumulative() {
    std::cout << "Test 10: multiple blocks cumulative..." << std::endl;

    Mempool pool(nullptr);
    TxId spent1 = makeTxId(900);
    TxId spent2 = makeTxId(901);
    auto tx1 = makeTx({{spent1, 0}});
    auto tx2 = makeTx({{spent2, 0}});
    auto tx3 = makeTx({{makeTxId(902), 0}});
    uint256 txid1 = addTxToMempool(pool, tx1);
    uint256 txid2 = addTxToMempool(pool, tx2);
    uint256 txid3 = addTxToMempool(pool, tx3);
    pool.refreshProof(txid1, makeFakeRoot(0x10), 10);
    pool.refreshProof(txid2, makeFakeRoot(0x10), 10);
    pool.refreshProof(txid3, makeFakeRoot(0x10), 10);

    TEST_ASSERT(pool.size() == 3, "Should have 3 TXs");

    // Block 1 spends spent1
    Transaction block1_tx = makeTx({{spent1, 0}}, 500);
    Block block1 = makeBlock(11, {block1_tx});
    pool.onBlockConnected(block1, 11, makeFakeRoot(0x11));

    TEST_ASSERT(pool.size() == 2, "tx1 evicted, 2 remain");
    TEST_ASSERT(pool.getStaleCount() == 2, "Remaining 2 are stale");

    // Block 2 spends spent2
    Transaction block2_tx = makeTx({{spent2, 0}}, 500);
    Block block2 = makeBlock(12, {block2_tx});
    pool.onBlockConnected(block2, 12, makeFakeRoot(0x12));

    TEST_ASSERT(pool.size() == 1, "tx2 evicted, 1 remains");
    TEST_ASSERT(pool.getStaleCount() == 1, "Remaining 1 is stale");

    std::cout << "  PASSED" << std::endl;
}

// Test 11: non-CSN TX unaffected
static void test_non_csn_tx_unaffected() {
    std::cout << "Test 11: non-CSN TX unaffected..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(1000), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    // Do NOT call refreshProof — this TX has no proof metadata

    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, makeFakeRoot(0x13));

    TEST_ASSERT(pool.getStaleCount() == 0, "TX without proof metadata should not be stale");
    TEST_ASSERT(pool.size() == 1, "TX should still be in pool");

    std::cout << "  PASSED" << std::endl;
}

// Test 12: refresh then stale again
static void test_refresh_then_stale_again() {
    std::cout << "Test 12: refresh then stale again..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(1100), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    pool.refreshProof(txid, makeFakeRoot(0x14), 10);

    // First block: becomes stale
    Block block1 = makeBlock(11);
    pool.onBlockConnected(block1, 11, makeFakeRoot(0x15));
    TEST_ASSERT(pool.getStaleCount() == 1, "Stale after block 1");

    // Refresh
    pool.refreshProof(txid, makeFakeRoot(0x15), 11);
    TEST_ASSERT(pool.getStaleCount() == 0, "Fresh after refresh");

    // Second block: stale again
    Block block2 = makeBlock(12);
    pool.onBlockConnected(block2, 12, makeFakeRoot(0x16));
    TEST_ASSERT(pool.getStaleCount() == 1, "Stale again after block 2");

    std::cout << "  PASSED" << std::endl;
}

// Test 13: single-attempt refresh policy then eviction
static void test_refresh_attempt_limit_policy() {
    std::cout << "Test 13: refresh attempt limit policy..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(1200), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    pool.refreshProof(txid, makeFakeRoot(0x21), 10);

    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, makeFakeRoot(0x22));
    TEST_ASSERT(pool.getStaleCount() == 1, "TX should be stale");

    auto round1 = pool.selectStaleForRefresh(11, 20, 2, 1, 256);
    TEST_ASSERT(round1.size() == 1, "First refresh round should schedule TX");
    TEST_ASSERT(round1[0] == txid, "Scheduled txid should match");

    auto round2 = pool.selectStaleForRefresh(11, 20, 2, 1, 256);
    TEST_ASSERT(round2.empty(), "Second round should not schedule same TX");
    TEST_ASSERT(pool.size() == 0, "TX should be evicted after attempt limit");

    std::cout << "  PASSED" << std::endl;
}

// Test 14: proof age cutoff evicts stale entry
static void test_proof_age_cutoff_policy() {
    std::cout << "Test 14: proof age cutoff policy..." << std::endl;

    Mempool pool(nullptr);
    auto tx = makeTx({{makeTxId(1300), 0}});
    uint256 txid = addTxToMempool(pool, tx);
    pool.refreshProof(txid, makeFakeRoot(0x23), 5);

    Block block = makeBlock(12);
    pool.onBlockConnected(block, 12, makeFakeRoot(0x24));
    TEST_ASSERT(pool.getStaleCount() == 1, "TX should be stale");

    auto refresh = pool.selectStaleForRefresh(12, 20, 2, 1, 256);
    TEST_ASSERT(refresh.empty(), "Old stale proof should not be refreshed");
    TEST_ASSERT(pool.size() == 0, "Old stale proof TX should be evicted");

    std::cout << "  PASSED" << std::endl;
}

// Test 15: overload bulk drop switches from refresh to eviction
static void test_stale_overload_bulk_drop_policy() {
    std::cout << "Test 15: stale overload bulk drop policy..." << std::endl;

    Mempool pool(nullptr);
    for (int i = 0; i < 3; i++) {
        auto tx = makeTx({{makeTxId(1400 + i), 0}});
        uint256 txid = addTxToMempool(pool, tx);
        pool.refreshProof(txid, makeFakeRoot(0x25), 10);
    }

    Block block = makeBlock(11);
    pool.onBlockConnected(block, 11, makeFakeRoot(0x26));
    TEST_ASSERT(pool.getStaleCount() == 3, "All TXs should be stale");

    auto refresh = pool.selectStaleForRefresh(11, 20, 2, 1, 3);
    TEST_ASSERT(refresh.empty(), "Overload mode should not schedule refresh");
    TEST_ASSERT(pool.size() == 0, "Overload mode should bulk-evict stale TXs");

    auto stats = pool.getStats();
    TEST_ASSERT(stats.refresh_dropped_budget_total >= 3, "Budget-drop counter should increase");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  Mempool Proof Staleness Tests" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    test_staleness_marked_on_block_connect();
    test_conflict_eviction();
    test_confirmed_tx_removed();
    test_no_staleness_without_root();
    test_addunchecked_rejects_private_transactions();
    test_refresh_clears_staleness();
    test_stale_count_accurate();
    test_get_stale_tx_ids();
    test_stats_include_staleness();
    test_block_disconnect_marks_stale();
    test_multiple_blocks_cumulative();
    test_non_csn_tx_unaffected();
    test_refresh_then_stale_again();
    test_refresh_attempt_limit_policy();
    test_proof_age_cutoff_policy();
    test_stale_overload_bulk_drop_policy();

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total
              << " assertions passed!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    return tests_passed == tests_total ? 0 : 1;
}
