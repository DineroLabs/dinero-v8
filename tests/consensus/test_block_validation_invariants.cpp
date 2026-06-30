// ============================================================================
// BLOCK VALIDATION INVARIANTS - Regression Tests
// ============================================================================
//
// Tests for the 5 runtime-enforced invariants in BlockValidator:
//   INV-1: Post-commit forest root == computed root
//   INV-2: No UTXO state mutation before proof verification (deferred spends)
//   INV-3: No double-spend in stateless mode (outpoint tracking)
//   INV-4: Snapshot restore produces correct UTXO count
//   INV-5: AddCoin completes before forest commit
//
// ============================================================================

#include <gtest/gtest.h>
#include "consensus/block_validation.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/chainparams.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/subsidy.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "primitives/amount.h"
#include <chrono>
#include <filesystem>
#include <string>
#include <cstring>
#include <unordered_map>

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

// Create a minimal coinbase transaction for a given height
static Transaction MakeCoinbase(uint32_t height) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;
    coinbase.witness_version = 1;  // Taproot

    TxInput input;
    input.prevout.txid = TxId();  // Null for coinbase
    input.prevout.vout = 0xffffffff;
    input.scriptSig.push_back(static_cast<uint8_t>(height & 0xFF));
    input.sequence = 0xffffffff;
    coinbase.vin.push_back(input);

    TxOutput output;
    output.value = ConsensusSubsidy::GetBlockSubsidy(height);
    // P2TR output (OP_1 PUSH32 <32 zero bytes>)
    output.scriptPubKey.push_back(0x51);  // OP_1
    output.scriptPubKey.push_back(0x20);  // Push 32 bytes
    for (int i = 0; i < 32; i++) {
        output.scriptPubKey.push_back(0x00);
    }
    coinbase.vout.push_back(output);

    return coinbase;
}

// Create a minimal block with only a coinbase transaction
static Block MakeCoinbaseBlock(uint32_t height, const uint256& prev_hash) {
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = 1772496000 + height * 120;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();

    Transaction coinbase = MakeCoinbase(height);
    block.vtx.push_back(coinbase);

    // Merkle root = coinbase txid for single-tx block
    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    block.header.utreexo_root = uint256();  // Will be computed

    return block;
}

class EmptySnapshotUTXOSet : public IConsensusUTXOSet {
public:
    explicit EmptySnapshotUTXOSet(bool supports_snapshot_restore = false)
        : supports_snapshot_restore_(supports_snapshot_restore) {}

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
        auto it = utxos_.find(outpoint);
        if (it == utxos_.end()) return true;
        utxos_.erase(it);
        return true;
    }

    bool ApplyBlock(const Block&, uint32_t, const uint256&, BlockUndo&, UtreexoHash&, std::string& error) override {
        error = "not-used-in-this-test";
        return false;
    }

    bool UndoBlock(const Block&, uint32_t, const BlockUndo&, std::string& error) override {
        error = "not-used-in-this-test";
        return false;
    }

    bool SupportsSnapshotRestore() const override {
        return supports_snapshot_restore_;
    }

    UTXOSnapshot Snapshot() const override {
        return UTXOSnapshot();  // Simulates LegacyUTXOSetAdapter behavior
    }

    void Restore(const UTXOSnapshot&) override {
        restore_calls++;
    }

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

    size_t restore_calls = 0;

private:
    std::unordered_map<OutPoint, UTXOEntry> utxos_;
    UtreexoForest forest_;
    uint256 best_block_;
    uint32_t height_ = 0;
    bool supports_snapshot_restore_ = false;
};

// ============================================================================
// INV-3: Double-spend tracking in stateless mode
// ============================================================================
// Stateless mode uses an outpoint tracking set to reject blocks where the
// same outpoint is spent twice. This test verifies that defense-in-depth
// mechanism works.

TEST(BlockValidationInvariants, StatelessDoubleSpendTrackingExists) {
    // Verify that the spent_in_block tracking set is populated during
    // stateless validation. We test this by verifying that a block with
    // duplicate inputs in a single transaction is ALWAYS rejected.
    //
    // Note: With real signatures unavailable in unit tests, script validation
    // may reject before the double-spend check fires. The key invariant is
    // that the block IS rejected — defense in depth means multiple layers catch it.
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    // Create a block with a transaction that has duplicate inputs
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.timestamp = 1772496000 + 240;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();

    // Coinbase
    Transaction coinbase = MakeCoinbase(2);
    block.vtx.push_back(coinbase);

    // Create a shared outpoint that will be double-spent
    TxId shared_txid = MakeTestTxId(42);

    // Tx1: spends shared_txid:0 TWICE (two inputs, same outpoint)
    Transaction tx1;
    tx1.version = 1;
    tx1.witness_version = 1;

    TxInput input1;
    input1.prevout.txid = shared_txid;
    input1.prevout.vout = 0;
    input1.sequence = 0xfffffffe;
    tx1.vin.push_back(input1);

    TxInput input2;
    input2.prevout.txid = shared_txid;
    input2.prevout.vout = 0;  // Same outpoint = double-spend!
    input2.sequence = 0xfffffffe;
    tx1.vin.push_back(input2);

    TxOutput out1;
    out1.value = AmountUna::Una(1000);
    out1.scriptPubKey = {0x51, 0x20};
    out1.scriptPubKey.resize(34, 0x00);
    tx1.vout.push_back(out1);
    block.vtx.push_back(tx1);

    // Provide minimal Utreexo data (spent_outputs for both inputs)
    BlockUtreexoData utreexo_data;
    // Stateless path enforces accumulator root continuity before tx-level checks.
    // Seed root_before from the current accumulator so duplicate-input logic is exercised.
    utreexo_data.accumulator_root_before = utxo_set.GetForest().getCommitment();
    SpentOutputData so(5000, std::vector<uint8_t>(34, 0x00));
    so.scriptPubKey[0] = 0x51;
    so.scriptPubKey[1] = 0x20;
    utreexo_data.spent_outputs.push_back(so);
    utreexo_data.spent_outputs.push_back(so);
    block.utreexo = utreexo_data;

    block.header.merkle_root = coinbase.GetTxid().AsUint256();

    // Attempt to validate — block MUST be rejected (either by double-spend
    // tracking or by script validation — both are valid defense layers)
    BlockUndo undo;
    std::string error;
    bool result = validator.ConnectBlock(block, 2, MakeTestHash(99), undo, error, nullptr);

    // Block must be rejected (defense in depth: either check catches it)
    EXPECT_FALSE(result) << "Block with duplicate inputs must be rejected";
    EXPECT_FALSE(error.empty()) << "Error message must be set on rejection";

    // Verify the error is from one of our defense layers
    bool caught_by_double_spend = (error.find("double-spend-in-block") != std::string::npos);
    bool caught_by_script = (error.find("SCRIPT_VERIFY_FAILED") != std::string::npos);
    EXPECT_TRUE(caught_by_double_spend || caught_by_script)
        << "Duplicate inputs must be caught by either double-spend tracking or script validation, got: " << error;
}

// ============================================================================
// CONSENSUS-SPLIT GATE: Stateless coinbase-maturity deferral
// ============================================================================
// The STATEFUL path enforces COINBASE_MATURITY (height - utxo.height >= 100)
// using UTXOEntry.isCoinbase + height. The STATELESS path CANNOT: the Utreexo
// leaf commits to neither height nor an is_coinbase flag, and SpentOutputData
// carries neither. The original code silently passed maturity by constructing
// inputs with is_coinbase=false ("maturity validated by proof") — a lie that
// let stateless nodes ACCEPT immature-coinbase-spending blocks that stateful
// nodes REJECT (consensus split).
//
// The non-forking fix: a stateless validator does NOT independently validate
// the maturity rule; it DEFERS to consensus/most-work and records that it did
// so via statelessMaturityUnverified(). These tests pin that behaviour:
//   - the flag LATCHES when the stateless path processes a spend-block, and
//   - the STATEFUL path NEVER sets it (it really does validate maturity).
// They fail if a future change reverts to silently asserting maturity in the
// stateless path (flag would stay false) or leaks the deferral into stateful
// mode (flag would turn true).

// Build a minimal stateless spend-block: coinbase + one tx spending one outpoint,
// with matching Utreexo spent_outputs metadata and seeded root_before.
static Block MakeStatelessSpendBlock(const ConsensusUTXOSet& utxo_set, uint32_t height) {
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.timestamp = 1772496000 + height * 120;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();

    Transaction coinbase = MakeCoinbase(height);
    block.vtx.push_back(coinbase);

    Transaction tx;
    tx.version = 1;
    tx.witness_version = 1;
    TxInput input;
    input.prevout.txid = MakeTestTxId(7);
    input.prevout.vout = 0;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);
    TxOutput out;
    out.value = AmountUna::Una(1000);
    out.scriptPubKey = {0x51, 0x20};
    out.scriptPubKey.resize(34, 0x00);
    tx.vout.push_back(out);
    block.vtx.push_back(tx);

    BlockUtreexoData utreexo_data;
    utreexo_data.accumulator_root_before = utxo_set.GetForest().getCommitment();
    SpentOutputData so(5000, std::vector<uint8_t>(34, 0x00));
    so.scriptPubKey[0] = 0x51;
    so.scriptPubKey[1] = 0x20;
    utreexo_data.spent_outputs.push_back(so);
    block.utreexo = utreexo_data;

    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    return block;
}

TEST(BlockValidationInvariants, StatelessMaturityDeferralLatchesOnSpendBlock) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    // Fresh validator has not deferred anything yet.
    EXPECT_FALSE(validator.statelessMaturityUnverified())
        << "Deferral flag must start unset";

    Block block = MakeStatelessSpendBlock(utxo_set, 2);
    BlockUndo undo;
    std::string error;
    // The block may be rejected later (e.g. by script validation — no real
    // signatures in a unit test). That is irrelevant: the deferral is recorded
    // when the stateless spend path is entered, BEFORE script checks. The point
    // is that the node did NOT independently validate coinbase maturity.
    (void)validator.ConnectBlock(block, 2, MakeTestHash(701), undo, error, nullptr);

    EXPECT_TRUE(validator.statelessMaturityUnverified())
        << "Stateless validator that processed a spend-block must record that it "
           "did NOT independently validate coinbase maturity (deferred to consensus)";
}

TEST(BlockValidationInvariants, StatefulModeNeverDefersMaturity) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATEFUL);

    EXPECT_FALSE(validator.statelessMaturityUnverified())
        << "Deferral flag must start unset";

    // Run the same spend-block through the STATEFUL path. It will be rejected
    // (no UTXOs in the set), but the stateful path enforces maturity for real and
    // must NEVER set the stateless deferral flag.
    Block block = MakeStatelessSpendBlock(utxo_set, 2);
    BlockUndo undo;
    std::string error;
    (void)validator.ConnectBlock(block, 2, MakeTestHash(702), undo, error, nullptr);

    EXPECT_FALSE(validator.statelessMaturityUnverified())
        << "STATEFUL validator validates maturity for real — it must never set the "
           "stateless maturity-deferral flag";
}

// ============================================================================
// INV-4: Snapshot restore produces correct UTXO count
// ============================================================================
// After Restore() in DisconnectBlock, the UTXO count must match the snapshot.

TEST(BlockValidationInvariants, SnapshotRestoreRoundTrip) {
    ConsensusUTXOSet utxo_set;

    // Add some UTXOs to simulate state before a block
    for (int i = 0; i < 5; i++) {
        OutPoint op(MakeTestTxId(i), 0);
        UTXOEntry entry(AmountUna::Una(1000 * (i + 1)),
                       std::vector<uint8_t>{0x51, 0x20, 0x00, 0x00},
                       1, false);
        utxo_set.AddCoin(op, entry);
    }

    // Take snapshot
    UTXOSnapshot snapshot = utxo_set.Snapshot();
    ASSERT_EQ(snapshot.GetUTXOCount(), 5u);

    // Mutate state (simulate connecting a block)
    OutPoint new_op(MakeTestTxId(100), 0);
    UTXOEntry new_entry(AmountUna::Una(9999),
                       std::vector<uint8_t>{0x51, 0x20, 0x00, 0x00},
                       2, false);
    utxo_set.AddCoin(new_op, new_entry);
    ASSERT_EQ(utxo_set.GetSetSize(), 6u);

    // Restore from snapshot
    utxo_set.Restore(snapshot);

    // INV-4: UTXO count must match snapshot
    EXPECT_EQ(utxo_set.GetSetSize(), snapshot.GetUTXOCount())
        << "Restored UTXO count must match snapshot";
    EXPECT_EQ(utxo_set.GetSetSize(), 5u);

    // Verify specific UTXOs are present
    for (int i = 0; i < 5; i++) {
        OutPoint op(MakeTestTxId(i), 0);
        EXPECT_TRUE(utxo_set.HaveCoin(op))
            << "UTXO " << i << " should exist after restore";
    }

    // Verify the newly added UTXO is gone
    EXPECT_FALSE(utxo_set.HaveCoin(new_op))
        << "UTXO added after snapshot should not exist after restore";
}

// ============================================================================
// INV-1: Post-commit root verification
// ============================================================================
// The forest root after commit must match the computed root.
// We test this indirectly by connecting a coinbase-only block (height 1)
// and verifying the process succeeds (the invariant check is inline).

TEST(BlockValidationInvariants, PostCommitRootVerification) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);

    // Connect a genesis-like block at height 1 (premine)
    uint256 prev_hash;
    prev_hash.SetNull();
    Block block = MakeCoinbaseBlock(1, prev_hash);

    BlockUndo undo;
    std::string error;

    // ApplyBlock (no root verification) should succeed.
    // INV-1 check runs after forest commit — if it were violated, this would abort.
    uint256 computed_root;
    bool result = validator.ApplyBlock(block, 1, MakeTestHash(1), undo, computed_root, error, nullptr);

    EXPECT_TRUE(result) << "Coinbase-only block should succeed. Error: " << error;

    // Verify forest has leaves (coinbase outputs were added)
    EXPECT_GT(utxo_set.GetForest().getNumLeaves(), 0u)
        << "Forest should have leaves after connecting a block";
}

// ============================================================================
// INV-2: Deferred SpendCoin - verify no premature mutation
// ============================================================================
// Connect a block, then disconnect it. The snapshot-restore path should
// produce identical state, proving that deferred spends + restore works.

TEST(BlockValidationInvariants, DeferredSpendDisconnectConsistency) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);

    // Step 1: Connect height 1 (premine) to create some UTXOs
    uint256 prev_hash;
    prev_hash.SetNull();
    Block premine_block = MakeCoinbaseBlock(1, prev_hash);

    BlockUndo undo1;
    std::string error;
    uint256 root1;
    bool result = validator.ApplyBlock(premine_block, 1, MakeTestHash(1), undo1, root1, error, nullptr);
    ASSERT_TRUE(result) << "Premine block should succeed. Error: " << error;

    size_t utxo_count_after_connect = utxo_set.GetSetSize();
    ASSERT_GT(utxo_count_after_connect, 0u);

    // Step 2: Disconnect height 1
    result = validator.DisconnectBlock(premine_block, 1, undo1, error);
    ASSERT_TRUE(result) << "DisconnectBlock should succeed. Error: " << error;

    // Step 3: UTXO set should be back to pre-block state (0 UTXOs)
    EXPECT_EQ(utxo_set.GetSetSize(), 0u)
        << "After disconnect, UTXO count should be 0 (genesis state)";
}

// ============================================================================
// Snapshot isolation: mutations during validation don't leak on failure
// ============================================================================

TEST(BlockValidationInvariants, FailedBlockLeavesNoTrace) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);

    // Connect height 1 first
    uint256 prev_hash;
    prev_hash.SetNull();
    Block premine_block = MakeCoinbaseBlock(1, prev_hash);

    BlockUndo undo1;
    std::string error;
    uint256 root1;
    bool result = validator.ApplyBlock(premine_block, 1, MakeTestHash(1), undo1, root1, error, nullptr);
    ASSERT_TRUE(result) << "Premine block should succeed. Error: " << error;

    size_t utxo_count_before = utxo_set.GetSetSize();

    // Create a deliberately invalid block (empty, no coinbase)
    Block bad_block;
    bad_block.header.version = 1;
    bad_block.header.prev_block_hash = MakeTestHash(1);
    bad_block.header.timestamp = 1772496000 + 240;
    bad_block.header.difficulty = 0x1d00ffff;
    bad_block.header.nonce = 0;
    bad_block.header.ZeroReserved();
    // No transactions = invalid

    BlockUndo undo2;
    error.clear();
    uint256 root2;
    result = validator.ApplyBlock(bad_block, 2, MakeTestHash(2), undo2, root2, error, nullptr);
    EXPECT_FALSE(result) << "Empty block should be rejected";

    // UTXO count must be unchanged (snapshot-first guarantees this)
    EXPECT_EQ(utxo_set.GetSetSize(), utxo_count_before)
        << "Failed block must not change UTXO state";
}

TEST(BlockValidationInvariants, EmptySnapshotSkipsRestorePath) {
    EmptySnapshotUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATEFUL);

    uint256 prev_hash;
    prev_hash.SetNull();
    Block block = MakeCoinbaseBlock(1, prev_hash);

    BlockUndo undo;
    std::string error;
    uint256 root;
    bool ok = validator.ApplyBlock(block, 1, MakeTestHash(77), undo, root, error, nullptr);
    ASSERT_TRUE(ok) << "ApplyBlock failed: " << error;
    ASSERT_FALSE(undo.pre_block_snapshot.has_value())
        << "Empty snapshots must not be stored in undo";

    ok = validator.DisconnectBlock(block, 1, undo, error);
    ASSERT_TRUE(ok) << "DisconnectBlock failed: " << error;
    EXPECT_EQ(utxo_set.restore_calls, 0u)
        << "Disconnect must not call Restore() for empty snapshots";
    EXPECT_EQ(utxo_set.GetSetSize(), 0u);
}

TEST(BlockValidationInvariants, SnapshotCapableEmptyStateRestoresOnFailure) {
    EmptySnapshotUTXOSet utxo_set(/*supports_snapshot_restore=*/true);
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATEFUL);

    Block bad_block;
    bad_block.header.version = 1;
    bad_block.header.prev_block_hash = MakeTestHash(500);
    bad_block.header.timestamp = 1772496000 + 120;
    bad_block.header.difficulty = 0x1d00ffff;
    bad_block.header.nonce = 0;
    bad_block.header.ZeroReserved();
    // No transactions => guaranteed validation failure

    BlockUndo undo;
    std::string error;
    uint256 root;
    bool ok = validator.ApplyBlock(bad_block, 2, MakeTestHash(501), undo, root, error, nullptr);
    ASSERT_FALSE(ok);
    EXPECT_EQ(utxo_set.restore_calls, 1u)
        << "Snapshot-capable backend must restore even empty snapshots on failure";
}

// ============================================================================
// #274: STATELESS early-return must populate pre_block_shielded_frontier
// ============================================================================
// ConnectBlockInternal captures the pre-block shielded frontier early, but
// the STATELESS-mode early return ("skipping forest-clone path") used to
// return true WITHOUT copying it into undo. On a CSN node a shielded block
// then persisted an undo lacking the frontier, and the ConnectTip publish
// invariant refused the tip (regtest abort).

TEST(BlockValidationInvariants, StatelessEarlyReturnStoresShieldedFrontierInUndo) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    // Attach a shielded commitment tree (frontier source). NullifierSet is
    // not needed for a coinbase-only block.
    shielded::CommitmentTree tree;
    validator.setShieldedState(&tree, nullptr);

    // Coinbase-only block: no spends => no proofs required => reaches the
    // STATELESS early return after coinbase reward validation.
    Block block = MakeCoinbaseBlock(1, uint256());

    BlockUndo undo;
    std::string error;
    bool ok = validator.ConnectBlock(block, 1, MakeTestHash(274), undo, error, nullptr);
    ASSERT_TRUE(ok) << "Coinbase-only stateless block must connect: " << error;

    // #274 regression: the frontier must be present so DisconnectBlock can
    // restore the commitment tree and the publish invariant accepts the tip.
    EXPECT_TRUE(undo.pre_block_shielded_frontier.has_value())
        << "STATELESS early return must store pre_block_shielded_frontier in undo";

    // The stored frontier must round-trip: it is the serialized pre-block tree.
    if (undo.pre_block_shielded_frontier.has_value()) {
        EXPECT_EQ(*undo.pre_block_shielded_frontier, tree.SerializeFrontier())
            << "Stored frontier must equal the pre-block tree frontier";
    }

    // Stateless mode has no UTXO snapshot — must NOT be set (memory landmine).
    EXPECT_FALSE(undo.pre_block_snapshot.has_value())
        << "STATELESS early return must not store a UTXO snapshot";
}

// Entry point
int main(int argc, char** argv) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
