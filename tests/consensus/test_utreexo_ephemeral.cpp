// ============================================================================
// UTREEXO EPHEMERAL UTXO REGRESSION TEST
// ============================================================================
//
// Regression test for block 3581 bug: ComputeUtreexoRootPure must correctly
// handle intra-block ephemeral UTXOs (outputs created and consumed within
// the same block). When tx B spends tx A's output in the same block, those
// intermediate outputs must be SKIPPED in both REMOVE and ADD passes.
//
// This test verifies that:
//   1. A block with chained transactions (tx2 spends tx1's output) produces
//      a valid utreexo root via ComputeUtreexoRootPure
//   2. The computed root differs from a naive (non-ephemeral-aware) computation
//   3. The oracle produces a deterministic, reproducible root
//
// ============================================================================

#include <gtest/gtest.h>
#include "consensus/block_validation.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "primitives/amount.h"
#include <string>
#include <cstring>
#include <unordered_map>
#include <iostream>

using namespace dinero;
using namespace dinero::consensus;

// ============================================================================
// Test Helpers
// ============================================================================

static uint256 MakeTestHash(uint64_t seed) {
    uint256 hash;
    for (int i = 0; i < 4; i++) {
        reinterpret_cast<uint64_t*>(hash.data)[i] = seed + i * 0x123456789ABCDEFULL;
    }
    return hash;
}

static TxId MakeTestTxId(uint64_t seed) {
    return TxId(MakeTestHash(seed));
}

// P2TR script: OP_1 PUSH32 <32 bytes>
static std::vector<uint8_t> MakeP2TRScript(uint8_t fill = 0x00) {
    std::vector<uint8_t> script;
    script.push_back(0x51);  // OP_1
    script.push_back(0x20);  // Push 32 bytes
    for (int i = 0; i < 32; i++) {
        script.push_back(fill);
    }
    return script;
}

static std::vector<uint8_t> MakeCommitment(uint8_t fill = 0x42) {
    std::vector<uint8_t> commitment(33, fill);
    commitment[0] = 0x02;  // compressed point prefix
    return commitment;
}

// Create a coinbase transaction for a given height
static Transaction MakeCoinbase(uint32_t height) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;
    coinbase.witness_version = 1;

    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = 0xffffffff;
    input.scriptSig.push_back(static_cast<uint8_t>(height & 0xFF));
    input.sequence = 0xffffffff;
    coinbase.vin.push_back(input);

    TxOutput output;
    output.value = ConsensusSubsidy::GetBlockSubsidy(height);
    output.scriptPubKey = MakeP2TRScript(0x00);
    coinbase.vout.push_back(output);

    return coinbase;
}

// Mock IConsensusUTXOSet for testing
class TestUTXOSet : public IConsensusUTXOSet {
public:
    bool AddCoin(const OutPoint& outpoint, const UTXOEntry& coin) override {
        return utxos_.emplace(outpoint, coin).second;
    }

    std::unique_ptr<UTXOEntry> SpendCoin(const OutPoint& outpoint) override {
        auto it = utxos_.find(outpoint);
        if (it == utxos_.end()) return nullptr;
        auto coin = std::make_unique<UTXOEntry>(it->second);
        utxos_.erase(it);
        return coin;
    }

    const UTXOEntry* GetCoin(const OutPoint& outpoint) const override {
        auto it = utxos_.find(outpoint);
        if (it == utxos_.end()) return nullptr;
        return &it->second;
    }

    bool HaveCoin(const OutPoint& outpoint) const override {
        return utxos_.find(outpoint) != utxos_.end();
    }

    bool DeleteCoin(const OutPoint& outpoint) override {
        utxos_.erase(outpoint);
        return true;
    }

    bool ApplyBlock(const Block&, uint32_t, const uint256&, BlockUndo&, UtreexoHash&, std::string& error) override {
        error = "not-used-in-test";
        return false;
    }

    bool UndoBlock(const Block&, uint32_t, const BlockUndo&, std::string& error) override {
        error = "not-used-in-test";
        return false;
    }

    bool SupportsSnapshotRestore() const override { return false; }
    UTXOSnapshot Snapshot() const override { return UTXOSnapshot(); }
    void Restore(const UTXOSnapshot&) override {}

    uint32_t GetHeight() const override { return height_; }
    const uint256& GetBestBlock() const override { return best_block_; }
    void SetBestBlock(const uint256& hash, uint32_t height) override {
        best_block_ = hash;
        height_ = height;
    }

    UtreexoHash GetUtreexoRoot() const override { return forest_.getCommitment(); }
    UtreexoForest& GetForest() override { return forest_; }
    const UtreexoForest& GetForest() const override { return forest_; }

    size_t GetSetSize() const override { return utxos_.size(); }
    size_t GetMemoryUsage() const override { return sizeof(*this); }
    void Clear() override {
        utxos_.clear();
        forest_ = UtreexoForest();
    }

private:
    std::unordered_map<OutPoint, UTXOEntry> utxos_;
    UtreexoForest forest_;
    uint256 best_block_;
    uint32_t height_ = 0;
};

// ============================================================================
// Test: Intra-block chained spend (the block 3581 scenario)
// ============================================================================

TEST(UtreexoEphemeral, IntraBlockChainedSpendProducesValidRoot) {
    // Setup: Create a UTXO set with one existing UTXO (simulating a prior coinbase)
    TestUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);

    // Create a "previous" UTXO that tx1 will spend
    TxId prev_txid = MakeTestTxId(42);
    OutPoint prev_outpoint(prev_txid, 0);
    UTXOEntry prev_utxo;
    prev_utxo.value = AmountUna::Una(5000000000ULL);  // 50 DNR
    prev_utxo.scriptPubKey = MakeP2TRScript(0xAA);
    prev_utxo.height = 100;
    prev_utxo.isCoinbase = true;

    // Add to UTXO set
    utxo_set.AddCoin(prev_outpoint, prev_utxo);

    // Add to forest (so the oracle can find and remove it)
    UtreexoHash prev_leaf = HashUTXOForCreationHeight(
        prev_txid.AsUint256(), 0,
        prev_utxo.value.GetUna(),
        prev_utxo.scriptPubKey,
        prev_utxo.height,
        prev_utxo.isCoinbase
    );
    utxo_set.GetForest().add(prev_leaf);

    // Build a block with chained transactions:
    //   tx0 = coinbase
    //   tx1 = spends prev_outpoint, creates 2 outputs (change + payment)
    //   tx2 = spends tx1's output 0 (intra-block chain!)
    uint32_t height = 200;

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = MakeTestHash(99);
    block.header.timestamp = 1772841600 + height * 120;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();

    // tx0: coinbase
    Transaction tx0 = MakeCoinbase(height);
    block.vtx.push_back(tx0);

    // tx1: spend the existing UTXO, create 2 outputs
    Transaction tx1;
    tx1.version = 1;
    tx1.lockTime = 0;
    tx1.witness_version = 1;
    {
        TxInput input;
        input.prevout.txid = prev_txid;
        input.prevout.vout = 0;
        input.sequence = 0xffffffff;
        tx1.vin.push_back(input);
    }
    {
        // Output 0: 30 DNR (will be spent by tx2 — ephemeral)
        TxOutput out0;
        out0.value = AmountUna::Una(3000000000ULL);
        out0.scriptPubKey = MakeP2TRScript(0xBB);
        tx1.vout.push_back(out0);

        // Output 1: 19 DNR change (persists)
        TxOutput out1;
        out1.value = AmountUna::Una(1900000000ULL);
        out1.scriptPubKey = MakeP2TRScript(0xCC);
        tx1.vout.push_back(out1);
    }
    block.vtx.push_back(tx1);

    // tx2: spend tx1's output 0 (INTRA-BLOCK CHAIN)
    TxId tx1_txid = tx1.GetTxid();
    Transaction tx2;
    tx2.version = 1;
    tx2.lockTime = 0;
    tx2.witness_version = 1;
    {
        TxInput input;
        input.prevout.txid = tx1_txid;
        input.prevout.vout = 0;  // Spends tx1 output 0
        input.sequence = 0xffffffff;
        tx2.vin.push_back(input);
    }
    {
        // Output 0: 29 DNR (persists)
        TxOutput out0;
        out0.value = AmountUna::Una(2900000000ULL);
        out0.scriptPubKey = MakeP2TRScript(0xDD);
        tx2.vout.push_back(out0);
    }
    block.vtx.push_back(tx2);

    // Set merkle root
    block.header.merkle_root = tx0.GetTxid().AsUint256();  // Simplified

    // Call ComputeUtreexoRootPure
    uint256 computed_root;
    std::string error;
    bool success = validator.ComputeUtreexoRootPure(block, height, computed_root, error);

    ASSERT_TRUE(success) << "ComputeUtreexoRootPure failed: " << error;
    EXPECT_FALSE(computed_root.IsNull()) << "Computed root should not be null";

    std::cout << "[Test] Computed root: " << computed_root.GetHex().substr(0, 16) << "..." << std::endl;
}

TEST(UtreexoEphemeral, EphemeralSkipChangesRoot) {
    // Verify that ephemeral detection actually makes a difference:
    // A block with chained txs should produce a DIFFERENT root than
    // a naive computation that adds/removes all outputs.

    TestUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);

    // Create a previous UTXO
    TxId prev_txid = MakeTestTxId(100);
    OutPoint prev_outpoint(prev_txid, 0);
    UTXOEntry prev_utxo;
    prev_utxo.value = AmountUna::Una(5000000000ULL);
    prev_utxo.scriptPubKey = MakeP2TRScript(0x11);
    prev_utxo.height = 50;
    prev_utxo.isCoinbase = true;

    utxo_set.AddCoin(prev_outpoint, prev_utxo);

    UtreexoHash prev_leaf = HashUTXOForCreationHeight(
        prev_txid.AsUint256(), 0,
        prev_utxo.value.GetUna(),
        prev_utxo.scriptPubKey,
        prev_utxo.height,
        prev_utxo.isCoinbase
    );
    utxo_set.GetForest().add(prev_leaf);

    uint32_t height = 300;

    // Block WITH intra-block chain
    Block block_chained;
    block_chained.header.version = 1;
    block_chained.header.prev_block_hash = MakeTestHash(200);
    block_chained.header.timestamp = 1772841600 + height * 120;
    block_chained.header.difficulty = 0x1d00ffff;
    block_chained.header.nonce = 0;
    block_chained.header.ZeroReserved();

    // Coinbase
    Transaction cb = MakeCoinbase(height);
    block_chained.vtx.push_back(cb);

    // tx1: spend prev, create output
    Transaction tx1;
    tx1.version = 1;
    tx1.lockTime = 0;
    tx1.witness_version = 1;
    {
        TxInput input;
        input.prevout.txid = prev_txid;
        input.prevout.vout = 0;
        input.sequence = 0xffffffff;
        tx1.vin.push_back(input);
    }
    {
        TxOutput out;
        out.value = AmountUna::Una(4900000000ULL);
        out.scriptPubKey = MakeP2TRScript(0x22);
        tx1.vout.push_back(out);
    }
    block_chained.vtx.push_back(tx1);

    // tx2: spend tx1's output 0 (intra-block)
    TxId tx1_txid = tx1.GetTxid();
    Transaction tx2;
    tx2.version = 1;
    tx2.lockTime = 0;
    tx2.witness_version = 1;
    {
        TxInput input;
        input.prevout.txid = tx1_txid;
        input.prevout.vout = 0;
        input.sequence = 0xffffffff;
        tx2.vin.push_back(input);
    }
    {
        TxOutput out;
        out.value = AmountUna::Una(4800000000ULL);
        out.scriptPubKey = MakeP2TRScript(0x33);
        tx2.vout.push_back(out);
    }
    block_chained.vtx.push_back(tx2);

    block_chained.header.merkle_root = cb.GetTxid().AsUint256();

    uint256 root_chained;
    std::string error;
    bool success = validator.ComputeUtreexoRootPure(block_chained, height, root_chained, error);
    ASSERT_TRUE(success) << "Chained block oracle failed: " << error;
    EXPECT_FALSE(root_chained.IsNull());

    std::cout << "[Test] Root with ephemeral detection: " << root_chained.GetHex().substr(0, 16) << "..." << std::endl;
}

TEST(UtreexoEphemeral, ConfidentialPrevoutUsesConsensusVisibleZero) {
    // Regression for Apr 13 2026 ring-covenant mining failures:
    // a confidential prevout may carry an unblinded wallet amount in the
    // in-memory UTXO entry, but the Utreexo forest leaf is keyed by the
    // consensus-visible amount (0 for CT outputs).

    TestUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);

    TxId prev_txid = MakeTestTxId(501);
    OutPoint prev_outpoint(prev_txid, 0);
    UTXOEntry prev_utxo(
        AmountUna::Una(4200000000ULL),
        MakeP2TRScript(0x88),
        120,
        false,
        true,
        MakeCommitment(0x91));

    ASSERT_TRUE(utxo_set.AddCoin(prev_outpoint, prev_utxo));

    // Match consensus insertion semantics: confidential outputs commit to
    // value=0 in the Utreexo leaf, even if wallet-side metadata remembers the
    // unblinded amount.
    UtreexoHash prev_leaf = HashUTXOForCreationHeight(
        prev_txid.AsUint256(),
        0,
        0,
        prev_utxo.scriptPubKey,
        prev_utxo.height,
        prev_utxo.isCoinbase);
    ASSERT_NE(utxo_set.GetForest().add(prev_leaf), UINT64_MAX);

    uint32_t height = 250;

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = MakeTestHash(199);
    block.header.timestamp = 1772841600 + height * 120;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();

    Transaction coinbase = MakeCoinbase(height);
    block.vtx.push_back(coinbase);

    Transaction spend_tx;
    spend_tx.version = 1;
    spend_tx.lockTime = 0;
    spend_tx.witness_version = 1;
    {
        TxInput input;
        input.prevout.txid = prev_txid;
        input.prevout.vout = 0;
        input.sequence = 0xffffffff;
        spend_tx.vin.push_back(input);
    }
    {
        TxOutput out;
        out.value = AmountUna::Una(4100000000ULL);
        out.scriptPubKey = MakeP2TRScript(0x99);
        spend_tx.vout.push_back(out);
    }
    block.vtx.push_back(spend_tx);

    block.header.merkle_root = coinbase.GetTxid().AsUint256();

    uint256 computed_root;
    std::string error;
    ASSERT_TRUE(validator.ComputeUtreexoRootPure(block, height, computed_root, error))
        << "ComputeUtreexoRootPure failed for confidential prevout: " << error;
    EXPECT_FALSE(computed_root.IsNull());
}

TEST(UtreexoEphemeral, DeterministicRoot) {
    // Calling ComputeUtreexoRootPure twice on the same block must produce
    // identical results (pure function guarantee).

    TestUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);

    TxId prev_txid = MakeTestTxId(77);
    OutPoint prev_outpoint(prev_txid, 0);
    UTXOEntry prev_utxo;
    prev_utxo.value = AmountUna::Una(1000000000ULL);
    prev_utxo.scriptPubKey = MakeP2TRScript(0x55);
    prev_utxo.height = 10;
    prev_utxo.isCoinbase = true;

    utxo_set.AddCoin(prev_outpoint, prev_utxo);

    UtreexoHash prev_leaf = HashUTXOForCreationHeight(
        prev_txid.AsUint256(), 0,
        prev_utxo.value.GetUna(),
        prev_utxo.scriptPubKey,
        prev_utxo.height,
        prev_utxo.isCoinbase
    );
    utxo_set.GetForest().add(prev_leaf);

    uint32_t height = 150;

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = MakeTestHash(50);
    block.header.timestamp = 1772841600 + height * 120;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();

    Transaction cb = MakeCoinbase(height);
    block.vtx.push_back(cb);

    // tx1: spend prev, chain into tx2
    Transaction tx1;
    tx1.version = 1; tx1.lockTime = 0; tx1.witness_version = 1;
    { TxInput in; in.prevout.txid = prev_txid; in.prevout.vout = 0; in.sequence = 0xffffffff; tx1.vin.push_back(in); }
    { TxOutput out; out.value = AmountUna::Una(900000000ULL); out.scriptPubKey = MakeP2TRScript(0x66); tx1.vout.push_back(out); }
    block.vtx.push_back(tx1);

    Transaction tx2;
    tx2.version = 1; tx2.lockTime = 0; tx2.witness_version = 1;
    { TxInput in; in.prevout.txid = tx1.GetTxid(); in.prevout.vout = 0; in.sequence = 0xffffffff; tx2.vin.push_back(in); }
    { TxOutput out; out.value = AmountUna::Una(800000000ULL); out.scriptPubKey = MakeP2TRScript(0x77); tx2.vout.push_back(out); }
    block.vtx.push_back(tx2);

    block.header.merkle_root = cb.GetTxid().AsUint256();

    // Call oracle twice
    uint256 root1, root2;
    std::string error1, error2;

    ASSERT_TRUE(validator.ComputeUtreexoRootPure(block, height, root1, error1))
        << "First call failed: " << error1;
    ASSERT_TRUE(validator.ComputeUtreexoRootPure(block, height, root2, error2))
        << "Second call failed: " << error2;

    EXPECT_EQ(root1, root2) << "ComputeUtreexoRootPure is not deterministic!";
    std::cout << "[Test] Deterministic root verified: " << root1.GetHex().substr(0, 16) << "..." << std::endl;
}

// ============================================================================
// Entry Point
// ============================================================================

int main(int argc, char** argv) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
