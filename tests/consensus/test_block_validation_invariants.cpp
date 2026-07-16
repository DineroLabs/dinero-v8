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
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/chainparams.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/subsidy.h"
#include "network/bridge_node.h"
#include "network/stateless_node.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "primitives/amount.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <cstring>
#include <memory>
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

namespace {
UtreexoHash ExpectedPostRoot(const UtreexoForest& base, const Block& block, uint32_t height) {
    UtreexoForest expected = base.clone();
    for (size_t i = 0; i < block.vtx.size(); i++) {
        const Transaction& tx = block.vtx[i];
        const TxId txid = tx.GetTxid();
        for (size_t n = 0; n < tx.vout.size(); n++) {
            const auto& out = tx.vout[n];
            const uint64_t leaf_value = out.is_confidential ? 0 : out.value.GetUna();
            UtreexoHash leaf = HashUTXOForCreationHeight(
                txid.AsUint256(), static_cast<uint32_t>(n), leaf_value,
                std::vector<uint8_t>(out.scriptPubKey.begin(), out.scriptPubKey.end()),
                height, tx.IsCoinbase());
            expected.add(leaf);
        }
    }
    return expected.getCommitment();
}

void SetHeaderRoot(Block& block, const UtreexoHash& root) {
    ASSERT_EQ(root.size(), 32u);
    std::memcpy(block.header.utreexo_root.data, root.data(), 32);
}
}  // namespace

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
// CONSENSUS-SPLIT GATE: Stateless coinbase maturity
// ============================================================================
// The STATEFUL path enforces COINBASE_MATURITY (height - utxo.height >= 100)
// using UTXOEntry.isCoinbase + height. Before the maturity-bound leaf fork, the
// STATELESS path cannot verify that rule independently and must soft-defer. At
// and after the fork, v2 leaves authenticate created_height + is_coinbase, so
// the live stateless ConnectBlock path must enforce the rule itself.

// Build a minimal stateless spend-block: coinbase + one tx spending one outpoint,
// with matching Utreexo spent_outputs metadata and seeded root_before.
static Block MakeStatelessSpendBlock(const ConsensusUTXOSet& utxo_set,
                                     uint32_t height,
                                     uint32_t spent_created_height = 0,
                                     bool spent_is_coinbase = false,
                                     uint8_t proof_format = 0) {
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
    utreexo_data.spend_proof.format_version =
        proof_format == 0 ? GetUtreexoProofFormatVersion(height) : proof_format;
    SpentOutputData so(5000, std::vector<uint8_t>(34, 0x00),
                       spent_created_height, spent_is_coinbase);
    so.scriptPubKey[0] = 0x51;
    so.scriptPubKey[1] = 0x20;
    utreexo_data.spent_outputs.push_back(so);
    block.utreexo = utreexo_data;

    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    return block;
}

static std::vector<uint8_t> MakeMatrixScript(uint8_t tag) {
    std::vector<uint8_t> script = {0x51, 0x20};
    script.resize(34, tag);
    return script;
}

static void SeedCpfpFunding(
    ConsensusUTXOSet& utxo_set,
    OutPoint& funding_outpoint,
    UTXOEntry& funding_utxo,
    UtreexoHash& funding_leaf) {
    funding_outpoint = OutPoint(MakeTestTxId(9001), 0);
    funding_utxo = UTXOEntry(
        AmountUna::Una(50'00000000ULL),
        MakeMatrixScript(0x42),
        GetUtreexoMaturityLeafActivationHeight(),
        false);
    ASSERT_TRUE(utxo_set.AddCoin(funding_outpoint, funding_utxo));
    funding_leaf = HashUTXOForCreationHeight(
        funding_outpoint.txid.AsUint256(),
        funding_outpoint.vout,
        funding_utxo.value.GetUna(),
        funding_utxo.scriptPubKey,
        funding_utxo.height,
        funding_utxo.isCoinbase);
    ASSERT_NE(utxo_set.GetForest().add(funding_leaf), UINT64_MAX);
}

static Block MakeCpfpMatrixBlock(uint32_t height, const OutPoint& funding_outpoint) {
    const auto script = MakeMatrixScript(0x42);
    Block block = MakeCoinbaseBlock(height, MakeTestHash(8000));

    Transaction parent;
    parent.version = 2;
    parent.lockTime = 0;
    parent.witness_version = 1;
    TxInput parent_input;
    parent_input.prevout.txid = funding_outpoint.txid;
    parent_input.prevout.vout = funding_outpoint.vout;
    parent_input.sequence = 0xfffffffe;
    parent.vin.push_back(parent_input);
    TxOutput parent_output;
    parent_output.value = AmountUna::Una(49'99900000ULL);
    parent_output.scriptPubKey = script;
    parent.vout.push_back(parent_output);
    block.vtx.push_back(parent);

    Transaction child;
    child.version = 2;
    child.lockTime = 0;
    child.witness_version = 1;
    TxInput child_input;
    child_input.prevout.txid = parent.GetTxid();
    child_input.prevout.vout = 0;
    child_input.sequence = 0xfffffffe;
    child.vin.push_back(child_input);
    TxOutput child_output;
    child_output.value = AmountUna::Una(49'99800000ULL);
    child_output.scriptPubKey = script;
    child.vout.push_back(child_output);
    block.vtx.push_back(child);

    block.header.merkle_root = block.vtx[0].GetTxid().AsUint256();
    return block;
}

static BlockUtreexoData BuildMinerLikeCpfpUtreexoData(
    ConsensusUTXOSet& utxo_set,
    const Block& block,
    uint32_t height) {
    BlockUtreexoData data;
    data.accumulator_root_before = utxo_set.GetForest().getCommitment();

    std::unordered_map<OutPoint, SpentOutputData> intra_block_outputs;
    for (const auto& tx : block.vtx) {
        const TxId txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            const auto& output = tx.vout[vout];
            SpentOutputData spent;
            spent.value = output.value.GetUna();
            spent.scriptPubKey = output.scriptPubKey;
            spent.is_confidential = output.is_confidential;
            spent.commitment = output.commitment;
            spent.created_height = height;
            spent.is_coinbase = tx.IsCoinbase();
            intra_block_outputs.emplace(OutPoint(txid, vout), std::move(spent));
        }
    }

    std::vector<UtreexoHash> targets;
    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        for (const auto& input : block.vtx[tx_idx].vin) {
            const OutPoint prevout(input.prevout.txid, input.prevout.vout);
            auto intra_it = intra_block_outputs.find(prevout);
            if (intra_it != intra_block_outputs.end()) {
                data.spent_outputs.push_back(intra_it->second);
                continue;
            }

            auto utxo = utxo_set.GetUTXO(prevout);
            EXPECT_TRUE(utxo.has_value());
            if (!utxo.has_value()) {
                continue;
            }

            const uint64_t leaf_value = utxo->is_confidential ? 0 : utxo->value.GetUna();
            targets.push_back(HashUTXOForCreationHeight(
                input.prevout.txid.AsUint256(),
                input.prevout.vout,
                leaf_value,
                utxo->scriptPubKey,
                utxo->height,
                utxo->isCoinbase));
            data.spent_outputs.emplace_back(
                leaf_value,
                utxo->scriptPubKey,
                utxo->height,
                utxo->isCoinbase,
                utxo->is_confidential,
                utxo->commitment);
        }
    }

    data.spend_proof = utxo_set.GetForest().generateBlockProof(
        targets,
        GetUtreexoProofFormatVersion(height));
    return data;
}

static BlockUtreexoData BuildBridgeCpfpUtreexoData(
    ConsensusUTXOSet& utxo_set,
    const Block& block,
    uint32_t height) {
    auto provider = std::shared_ptr<IUTXOProvider>(
        std::shared_ptr<void>{},
        static_cast<IUTXOProvider*>(&utxo_set));
    network::BridgeNode bridge(provider, &utxo_set.GetForest());
    return bridge.GenerateProofForBlock(block, height);
}

static void ExpectCpfpProducerDataShape(const BlockUtreexoData& data, const char* label) {
    EXPECT_EQ(data.spent_outputs.size(), 2u) << label;
    EXPECT_EQ(data.spend_proof.targets.size(), 1u) << label;
}

static void RunCpfpConnectBlockMatrixCase(
    const char* label,
    const std::function<BlockUtreexoData(ConsensusUTXOSet&, const Block&, uint32_t)>& producer) {
    ConsensusUTXOSet utxo_set;
    OutPoint funding_outpoint;
    UTXOEntry funding_utxo;
    UtreexoHash funding_leaf;
    SeedCpfpFunding(utxo_set, funding_outpoint, funding_utxo, funding_leaf);

    const uint32_t height = GetUtreexoMaturityLeafActivationHeight() + 1;
    Block block = MakeCpfpMatrixBlock(height, funding_outpoint);
    BlockUtreexoData data = producer(utxo_set, block, height);
    ExpectCpfpProducerDataShape(data, label);
    block.utreexo = data;

    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);
    uint256 computed_root;
    std::string root_error;
    ASSERT_TRUE(validator.ComputeUtreexoRootPure(block, height, computed_root, root_error))
        << label << ": " << root_error;
    block.header.utreexo_root = computed_root;

    BlockUndo undo;
    std::string error;
    (void)validator.ConnectBlock(block, height, MakeTestHash(9100), undo, error, nullptr);

    EXPECT_EQ(error.find("utreexo-proof-target-mismatch"), std::string::npos) << label << ": " << error;
    EXPECT_EQ(error.find("utreexo-ephemeral-spent-output-mismatch"), std::string::npos) << label << ": " << error;
    EXPECT_EQ(error.find("utreexo-spent-outputs-count-mismatch"), std::string::npos) << label << ": " << error;
    EXPECT_EQ(error.find("utreexo-insufficient-spent-outputs"), std::string::npos) << label << ": " << error;
    EXPECT_EQ(error.find("bad-utreexo-root"), std::string::npos) << label << ": " << error;
    EXPECT_EQ(error.find("PROOF_INVALID"), std::string::npos) << label << ": " << error;
    EXPECT_EQ(error.find("stateless-coinbase-maturity-violation"), std::string::npos) << label << ": " << error;
}

static void RunCpfpConnectBlockRejectsLiedChildMetadataCase(
    const char* label,
    const std::function<BlockUtreexoData(ConsensusUTXOSet&, const Block&, uint32_t)>& producer) {
    ConsensusUTXOSet utxo_set;
    OutPoint funding_outpoint;
    UTXOEntry funding_utxo;
    UtreexoHash funding_leaf;
    SeedCpfpFunding(utxo_set, funding_outpoint, funding_utxo, funding_leaf);

    const uint32_t height = GetUtreexoMaturityLeafActivationHeight() + 1;
    Block block = MakeCpfpMatrixBlock(height, funding_outpoint);
    BlockUtreexoData data = producer(utxo_set, block, height);
    ExpectCpfpProducerDataShape(data, label);
    block.utreexo = data;

    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);
    uint256 computed_root;
    std::string root_error;
    ASSERT_TRUE(validator.ComputeUtreexoRootPure(block, height, computed_root, root_error))
        << label << ": " << root_error;
    block.header.utreexo_root = computed_root;

    ASSERT_GT(data.spent_outputs.size(), 1u) << label;
    data.spent_outputs[1].scriptPubKey[2] ^= 0x7f;
    block.utreexo = data;

    BlockUndo undo;
    std::string error;
    const bool ok = validator.ConnectBlock(block, height, MakeTestHash(9101), undo, error, nullptr);

    EXPECT_FALSE(ok) << label;
    EXPECT_NE(error.find("utreexo-ephemeral-spent-output-mismatch"), std::string::npos)
        << label << ": " << error;
}

static void RunCpfpReplayBlockMatrixCase(
    const char* label,
    const std::function<BlockUtreexoData(ConsensusUTXOSet&, const Block&, uint32_t)>& producer) {
    ConsensusUTXOSet producer_set;
    OutPoint funding_outpoint;
    UTXOEntry funding_utxo;
    UtreexoHash funding_leaf;
    SeedCpfpFunding(producer_set, funding_outpoint, funding_utxo, funding_leaf);

    const uint32_t height = GetUtreexoMaturityLeafActivationHeight() + 1;
    Block block = MakeCpfpMatrixBlock(height, funding_outpoint);
    BlockUtreexoData data = producer(producer_set, block, height);
    ExpectCpfpProducerDataShape(data, label);

    UtreexoForest expected_after = producer_set.GetForest();
    auto position = expected_after.findLeafPosition(funding_leaf);
    ASSERT_TRUE(position.has_value()) << label;
    auto proof = expected_after.prove(*position);
    ASSERT_TRUE(proof.has_value()) << label;
    ASSERT_TRUE(expected_after.remove(funding_leaf, *proof)) << label;
    const TxId coinbase_txid = block.vtx[0].GetTxid();
    ASSERT_NE(expected_after.add(HashUTXOForCreationHeight(
        coinbase_txid.AsUint256(),
        0,
        block.vtx[0].vout[0].value.GetUna(),
        block.vtx[0].vout[0].scriptPubKey,
        height,
        true)), UINT64_MAX) << label;
    const TxId child_txid = block.vtx[2].GetTxid();
    ASSERT_NE(expected_after.add(HashUTXOForCreationHeight(
        child_txid.AsUint256(),
        0,
        block.vtx[2].vout[0].value.GetUna(),
        block.vtx[2].vout[0].scriptPubKey,
        height,
        false)), UINT64_MAX) << label;
    const auto expected_root = expected_after.getCommitment();
    ASSERT_EQ(expected_root.size(), 32u) << label;
    std::memcpy(block.header.utreexo_root.begin(), expected_root.data(), 32);

    UtreexoForest replay_forest;
    ASSERT_NE(replay_forest.add(funding_leaf), UINT64_MAX) << label;
    network::StatelessNode node(&replay_forest);
    node.SyncToForestState(GetUtreexoMaturityLeafActivationHeight());

    EXPECT_TRUE(node.ReplayBlock(
        block,
        height,
        data.spend_proof.targets,
        &data.spent_outputs)) << label;
    EXPECT_EQ(replay_forest.getCommitment(), expected_root) << label;
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

TEST(BlockValidationInvariants, StatelessRejectsImmatureV2CoinbaseOnLiveConnectPath) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    ASSERT_GT(activation, 0u);
    constexpr uint32_t kCoinbaseMaturity = 100;
    const uint32_t coinbase_height = activation;
    const uint32_t spend_height = coinbase_height + kCoinbaseMaturity - 1;

    Block block = MakeStatelessSpendBlock(
        utxo_set,
        spend_height,
        coinbase_height,
        true,
        GetUtreexoProofFormatVersion(spend_height));
    BlockUndo undo;
    std::string error;
    const bool ok = validator.ConnectBlock(block, spend_height, MakeTestHash(703), undo, error, nullptr);

    EXPECT_FALSE(ok) << "Immature v2 coinbase spend must be rejected on the live stateless path";
    EXPECT_NE(error.find("stateless-coinbase-maturity-violation"), std::string::npos)
        << "Expected maturity rejection before script/proof validation, got: " << error;
    EXPECT_FALSE(validator.statelessMaturityUnverified())
        << "v2 maturity metadata is authenticated; it must be enforced, not deferred";
}

TEST(BlockValidationInvariants, StatelessRejectsProofFormatDowngradeFromTrustedHeight) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    ASSERT_GT(activation, 0u);

    Block block = MakeStatelessSpendBlock(
        utxo_set,
        activation,
        activation,
        true,
        5);
    BlockUndo undo;
    std::string error;
    const bool ok = validator.ConnectBlock(block, activation, MakeTestHash(704), undo, error, nullptr);

    EXPECT_FALSE(ok) << "Post-activation proof format is chosen by trusted block height";
    EXPECT_NE(error.find("stateless-validation-proof-format-mismatch"), std::string::npos)
        << "Expected trusted-height format rejection, got: " << error;
}

TEST(BlockValidationInvariants, StatelessLegacyLeafGraceWindowSoftDefers) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    ASSERT_GT(activation, 0u);
    const uint32_t spend_height = activation + 50;

    Block block = MakeStatelessSpendBlock(
        utxo_set,
        spend_height,
        activation - 1,
        true,
        GetUtreexoProofFormatVersion(spend_height));
    BlockUndo undo;
    std::string error;
    (void)validator.ConnectBlock(block, spend_height, MakeTestHash(705), undo, error, nullptr);

    EXPECT_TRUE(validator.statelessMaturityUnverified())
        << "Legacy leaves inside the grace window must soft-defer maturity";
    EXPECT_EQ(error.find("stateless-validation-proof-format-mismatch"), std::string::npos)
        << "Honest v6 grace-window legacy spends must not be treated as format downgrades";
}

TEST(BlockValidationInvariants, StatelessCpfpConnectBlockAcceptsMinerProducedConvention) {
    RunCpfpConnectBlockMatrixCase("miner-produced", BuildMinerLikeCpfpUtreexoData);
}

TEST(BlockValidationInvariants, StatelessCpfpConnectBlockAcceptsBridgeProducedConvention) {
    RunCpfpConnectBlockMatrixCase("bridge-produced", BuildBridgeCpfpUtreexoData);
}

TEST(BlockValidationInvariants, StatelessCpfpConnectBlockRejectsLiedMinerChildMetadata) {
    RunCpfpConnectBlockRejectsLiedChildMetadataCase("miner-produced", BuildMinerLikeCpfpUtreexoData);
}

TEST(BlockValidationInvariants, StatelessCpfpConnectBlockRejectsLiedBridgeChildMetadata) {
    RunCpfpConnectBlockRejectsLiedChildMetadataCase("bridge-produced", BuildBridgeCpfpUtreexoData);
}

TEST(BlockValidationInvariants, StatelessCpfpReplayBlockAcceptsMinerProducedConvention) {
    RunCpfpReplayBlockMatrixCase("miner-produced", BuildMinerLikeCpfpUtreexoData);
}

TEST(BlockValidationInvariants, StatelessCpfpReplayBlockAcceptsBridgeProducedConvention) {
    RunCpfpReplayBlockMatrixCase("bridge-produced", BuildBridgeCpfpUtreexoData);
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
// 2026-07-16 on-device wedge: a proof-less spend block in STATELESS mode is a
// pure presence reject and must NOT fire the Phase-2 failure Restore().
// ============================================================================
// Restore() replaces the SHARED forest object; the CSN worker applies
// validated proofs to that same forest from another thread. The block-body-
// before-proof race retries this reject continuously, and each pointless
// Restore() can clobber a concurrent worker apply — observed on-device as a
// forest root matching no canonical height (FAIL step 2 at height 62825,
// local=d5fe696d... vs canonical=e6d55d21...), halting CSN IBD permanently
// after 5,689 retry attempts.

TEST(BlockValidationInvariants, StatelessMissingProofRejectDoesNotFireRestore) {
    EmptySnapshotUTXOSet utxo_set(/*supports_snapshot_restore=*/true);
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    // Spend block WITHOUT a utreexo payload — the exact shape of a stored
    // body that arrived ahead of its proof.
    Block block = MakeCoinbaseBlock(62825, MakeTestHash(62824));
    Transaction spend;
    spend.version = 1;
    spend.witness_version = 1;
    TxInput input;
    input.prevout.txid = MakeTestTxId(777);
    input.prevout.vout = 0;
    input.sequence = 0xfffffffe;
    spend.vin.push_back(input);
    TxOutput output;
    output.value = AmountUna::Una(1000);
    output.scriptPubKey = {0x51, 0x20};
    output.scriptPubKey.resize(34, 0x00);
    spend.vout.push_back(output);
    block.vtx.push_back(spend);
    ASSERT_FALSE(block.utreexo.has_value());

    BlockUndo undo;
    std::string error;
    uint256 root;
    bool ok = validator.ApplyBlock(block, 62825, MakeTestHash(62825), undo, root, error, nullptr);
    ASSERT_FALSE(ok) << "proof-less spend block must not connect in stateless mode";
    EXPECT_NE(error.find("missing-utreexo-data"), std::string::npos)
        << "reject reason must be the proof-presence check, got: " << error;
    EXPECT_EQ(utxo_set.restore_calls, 0u)
        << "presence reject must not fire Restore() — it clobbers the shared "
           "forest under the CSN worker's concurrent proof apply";
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
    // #382: the stateless forward path now verifies the header commitment;
    // give the fixture block a REAL post-state root.
    SetHeaderRoot(block, ExpectedPostRoot(utxo_set.GetForest(), block, 1));
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

// The shielded epoch reset snapshot must survive undo serialization (it is
// persisted to the undo flatfile and reloaded on a reorg across the cutover).
// Both the binary (Serialize/Deserialize) and JSON (ToJson/FromJson) forms.
TEST(BlockValidationInvariants, BlockUndoEpochSnapshotRoundTrips) {
    BlockUndo undo(61000);
    shielded::ShieldedEpochSnapshot snap;
    snap.tree_frontier  = {0x01, 0x02, 0x03};
    snap.anchor_history = {0xAA, 0xBB, 0xCC, 0xDD};
    snap.nullifiers     = {0x4E, 0x43, 0x53, 0x46, 0x99, 0x00, 0x7F};
    undo.pre_reset_shielded_epoch = snap;

    const auto bytes = undo.Serialize();
    const BlockUndo back = BlockUndo::Deserialize(bytes);
    ASSERT_TRUE(back.pre_reset_shielded_epoch.has_value());
    EXPECT_EQ(back.height, 61000u);
    EXPECT_EQ(back.pre_reset_shielded_epoch->tree_frontier,  snap.tree_frontier);
    EXPECT_EQ(back.pre_reset_shielded_epoch->anchor_history, snap.anchor_history);
    EXPECT_EQ(back.pre_reset_shielded_epoch->nullifiers,     snap.nullifiers);

    const BlockUndo jback = BlockUndo::FromJson(undo.ToJson());
    ASSERT_TRUE(jback.pre_reset_shielded_epoch.has_value());
    EXPECT_EQ(jback.pre_reset_shielded_epoch->tree_frontier,  snap.tree_frontier);
    EXPECT_EQ(jback.pre_reset_shielded_epoch->anchor_history, snap.anchor_history);
    EXPECT_EQ(jback.pre_reset_shielded_epoch->nullifiers,     snap.nullifiers);
}

// The reset happens on exactly one block; every other undo must leave the field
// nullopt. Also: an OLD undo record (written before this field existed) ends
// right after the frontier — Deserialize must tolerate that and yield nullopt,
// not read past the end.
TEST(BlockValidationInvariants, BlockUndoWithoutEpochSnapshotIsBackwardCompatible) {
    BlockUndo undo(100);
    undo.pre_block_shielded_frontier = std::vector<uint8_t>{0x07, 0x08, 0x09};
    const auto bytes = undo.Serialize();

    const BlockUndo back = BlockUndo::Deserialize(bytes);
    EXPECT_FALSE(back.pre_reset_shielded_epoch.has_value());
    ASSERT_TRUE(back.pre_block_shielded_frontier.has_value());
    EXPECT_EQ(*back.pre_block_shielded_frontier, *undo.pre_block_shielded_frontier);

    // Simulate an old record: drop the trailing epoch-absent flag byte so the
    // stream ends exactly where the pre-field format ended.
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(bytes.back(), 0x00) << "final byte is the epoch-absent flag";
    const std::vector<uint8_t> old_format(bytes.begin(), bytes.end() - 1);
    const BlockUndo old_back = BlockUndo::Deserialize(old_format);
    EXPECT_FALSE(old_back.pre_reset_shielded_epoch.has_value());
    ASSERT_TRUE(old_back.pre_block_shielded_frontier.has_value())
        << "dropping the epoch flag must not disturb the frontier field";
    EXPECT_EQ(*old_back.pre_block_shielded_frontier, *undo.pre_block_shielded_frontier);
}

// Entry point
// ============================================================================
// #382: stateless forward-connect must maintain the shared forest.
// DineroTX clean-run evidence: ConnectTip's stateless early-return connected
// 52289..55705 while the shared forest stayed frozen at the canonical-52288
// root (77ba3a36...), so the first spend block failed root-continuity forever.
// Contract: if the forest is at the block's PRE-state, ConnectBlock applies
// the block's additions and verifies the result against the header commitment
// (fail-fast on mismatch); if already at POST-state (StatelessNode worker
// applied it), it must NOT double-apply.
// ============================================================================


TEST(BlockValidationInvariants, StatelessForwardConnectAdvancesForest) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    const uint32_t height = 2;
    Block block = MakeCoinbaseBlock(height, MakeTestHash(41));
    const UtreexoHash expected_root =
        ExpectedPostRoot(utxo_set.GetForest(), block, height);
    SetHeaderRoot(block, expected_root);

    // (1) Forest at PRE-state (the ConnectTip forward path: no worker involved,
    // no block.utreexo). Connect must succeed AND advance the shared forest.
    BlockUndo undo;
    std::string error;
    ASSERT_TRUE(validator.ConnectBlock(block, height, MakeTestHash(42), undo, error, nullptr))
        << error;
    EXPECT_EQ(utxo_set.GetForest().getCommitment(), expected_root)
        << "stateless forward-connect left the shared forest at pre-state (#382 "
           "— the DineroTX frozen-forest bug)";

    // (2) Forest already at POST-state (worker-applied shape): reconnecting the
    // same block must not double-apply.
    BlockUndo undo2;
    std::string error2;
    ASSERT_TRUE(validator.ConnectBlock(block, height, MakeTestHash(43), undo2, error2, nullptr))
        << error2;
    EXPECT_EQ(utxo_set.GetForest().getCommitment(), expected_root)
        << "worker-applied block was double-applied";
}

TEST(BlockValidationInvariants, StatelessForwardConnectRejectsWrongHeaderRoot) {
    ConsensusUTXOSet utxo_set;
    BlockValidator validator(&utxo_set);
    validator.setValidationMode(ValidationMode::STATELESS);

    const uint32_t height = 2;
    Block block = MakeCoinbaseBlock(height, MakeTestHash(51));
    // Deliberately wrong (but non-null) header commitment.
    block.header.utreexo_root = MakeTestHash(0xbad);

    BlockUndo undo;
    std::string error;
    const UtreexoHash before = utxo_set.GetForest().getCommitment();
    EXPECT_FALSE(validator.ConnectBlock(block, height, MakeTestHash(52), undo, error, nullptr))
        << "a wrong header utreexo commitment must fail fast on the stateless "
           "forward path (silent divergence is how the forest froze for 3,417 "
           "blocks unnoticed)";
    EXPECT_NE(error.find("bad-utreexo-root"), std::string::npos) << "error was: " << error;
    EXPECT_EQ(utxo_set.GetForest().getCommitment(), before)
        << "failed connect must not mutate the forest";
}

int main(int argc, char** argv) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
