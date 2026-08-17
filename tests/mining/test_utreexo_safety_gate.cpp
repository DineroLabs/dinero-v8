// ============================================================================
// UTREEXO SAFETY GATE TEST — BlockAssembler Hardening
// ============================================================================
//
// Regression test for the "null utreexo root" bug: BlockAssembler must REFUSE
// to produce a block template when BlockValidator is not wired, preventing
// miners from committing invalid utreexo roots that would later be rejected
// by validation.
//
// Tests:
//   1. CreateNewBlock returns nullptr when BlockValidator is missing
//   2. CreateNewBlock succeeds when BlockValidator is wired
//   3. CreateJob returns nullptr when BlockValidator is missing
//
// ============================================================================

#include <gtest/gtest.h>
#include "mining/block_assembler.h"
#include "consensus/block_validation.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "consensus/subsidy.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "primitives/amount.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "daemon/mempool.h"
#include <string>
#include <iostream>
#include <filesystem>
#include <cstring>

using namespace dinero;
using namespace dinero::consensus;

// ============================================================================
// Minimal mock UTXO set for testing
// ============================================================================

class SafetyGateUTXOSet : public IConsensusUTXOSet {
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
        return (it != utxos_.end()) ? &it->second : nullptr;
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

    // IUTXOProvider interface
    std::optional<UTXOEntry> GetUTXO(const OutPoint& outpoint) const override {
        auto it = utxos_.find(outpoint);
        if (it != utxos_.end()) return it->second;
        return std::nullopt;
    }
    bool AddUTXO(const OutPoint& outpoint, const UTXOEntry& entry) override {
        return utxos_.emplace(outpoint, entry).second;
    }
    bool SpendUTXO(const OutPoint& outpoint, uint32_t) override {
        return utxos_.erase(outpoint) > 0;
    }
    bool DeleteUTXO(const OutPoint& outpoint) override {
        utxos_.erase(outpoint);
        return true;
    }
    bool HasUTXO(const OutPoint& outpoint) const override {
        return utxos_.count(outpoint) > 0;
    }

private:
    std::unordered_map<OutPoint, UTXOEntry> utxos_;
    UtreexoForest forest_;
    uint256 best_block_;
    uint32_t height_ = 0;
};

// ============================================================================
// Test fixture — uses real genesis block and ChainDB
// ============================================================================

class UtreexoSafetyGateTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use a temp directory for ChainDB
        db_path_ = std::filesystem::temp_directory_path() / "test_safety_gate_db";
        std::filesystem::remove_all(db_path_);
        std::filesystem::create_directories(db_path_);

        chain_db_ = std::make_unique<ChainDB>();
        auto status = chain_db_->init(db_path_.string());
        ASSERT_TRUE(status == Status::Ok) << "ChainDB init failed";

        // Build canonical genesis and store it in ChainDB
        auto genesis = dinero::BuildCanonicalGenesis(dinero::Params());
        genesis_hash_ = genesis.header.GetHash();

        Block genesis_block;
        genesis_block.header = genesis.header;

        auto token = ChainWriteToken::CreateForTesting();
        chain_db_->putBlock(token, genesis_hash_, genesis_block);
        chain_db_->putHeader(token, genesis_hash_, genesis.header, 0, arith_uint256(0));
        chain_db_->setTip(token, genesis_hash_, 0, arith_uint256(0));

        utxo_set_ = std::make_unique<SafetyGateUTXOSet>();
        validator_ = std::make_unique<BlockValidator>(utxo_set_.get());
        mempool_ = std::make_unique<Mempool>(chain_db_.get());
    }

    void TearDown() override {
        mempool_.reset();
        validator_.reset();
        utxo_set_.reset();
        chain_db_.reset();
        std::filesystem::remove_all(db_path_);
    }

    // Valid taproot address for mining (from Mac mining wallet)
    static constexpr const char* MINING_ADDRESS =
        "din1pmvnrlwkk87phdekfs65gfxv69qgjcnupanyyzw894rwd8e76n66q6cey44";

    std::filesystem::path db_path_;
    std::unique_ptr<ChainDB> chain_db_;
    std::unique_ptr<SafetyGateUTXOSet> utxo_set_;
    std::unique_ptr<BlockValidator> validator_;
    std::unique_ptr<Mempool> mempool_;
    uint256 genesis_hash_;
};

static TxId MakeTestTxId(uint32_t seed) {
    TxId id;
    std::memset(id.v.data, 0, 32);
    std::memcpy(id.v.data, &seed, sizeof(seed));
    return id;
}

static Transaction MakeUncheckedTransparentTx(uint32_t input_seed) {
    Transaction tx;
    tx.version = 2;
    tx.witness_version = 0xFF;

    TxInput input;
    input.prevout.txid = MakeTestTxId(input_seed);
    input.prevout.vout = 0;
    tx.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(1000);
    output.scriptPubKey = {0x51};  // OP_TRUE / anyone-can-spend is sufficient for this regression
    tx.vout.push_back(output);

    return tx;
}

// ============================================================================
// Test 1: CreateNewBlock MUST fail when BlockValidator is not wired
// ============================================================================

TEST_F(UtreexoSafetyGateTest, CreateNewBlockFailsWithoutValidator) {
    // Create assembler WITHOUT wiring BlockValidator
    BlockAssembler assembler(chain_db_.get());
    assembler.SetConsensusUTXOSet(utxo_set_.get());

    // Attempt to create block — MUST return nullptr (not a block with null root)
    auto block = assembler.CreateNewBlock(MINING_ADDRESS);

    EXPECT_EQ(block, nullptr)
        << "CRITICAL: BlockAssembler produced a block without BlockValidator! "
           "This would commit a null utreexo root.";
}

// ============================================================================
// Test 2: CreateNewBlock succeeds when BlockValidator IS wired
// ============================================================================

TEST_F(UtreexoSafetyGateTest, CreateNewBlockSucceedsWithValidator) {
    BlockAssembler assembler(chain_db_.get());
    assembler.SetConsensusUTXOSet(utxo_set_.get());
    assembler.SetBlockValidator(validator_.get());
    assembler.SetUTXOProvider(std::shared_ptr<consensus::IUTXOProvider>(
        utxo_set_.get(), [](auto*){}));  // non-owning shared_ptr
    assembler.setMempool(mempool_.get());

    auto block = assembler.CreateNewBlock(MINING_ADDRESS);

    ASSERT_NE(block, nullptr)
        << "BlockAssembler failed to produce a block with validator wired";
    EXPECT_FALSE(block->header.utreexo_root.IsNull())
        << "Block was produced but utreexo_root is null — oracle didn't compute";
}

// ============================================================================
// Test 3: CreateJob MUST fail when BlockValidator is not wired
// ============================================================================

TEST_F(UtreexoSafetyGateTest, CreateJobFailsWithoutValidator) {
    BlockAssembler assembler(chain_db_.get());
    assembler.SetConsensusUTXOSet(utxo_set_.get());
    assembler.SetMiningAddress(MINING_ADDRESS);

    // Attempt to create job — MUST return nullptr
    auto job = assembler.CreateJob();

    EXPECT_EQ(job, nullptr)
        << "CRITICAL: BlockAssembler produced a mining job without BlockValidator! "
           "This would commit a null utreexo root.";
}

// ============================================================================
// Test 4: Bad relayed tx is quarantined from templates instead of poisoning mining
// ============================================================================

TEST_F(UtreexoSafetyGateTest, CreateNewBlockQuarantinesTemplatePoisoningTx) {
    Transaction bad_tx = MakeUncheckedTransparentTx(/*input_seed=*/42);
    const uint256 bad_txid = bad_tx.GetTxid().AsUint256();
    mempool_->addUnchecked(bad_tx);

    BlockAssembler assembler(chain_db_.get());
    assembler.SetConsensusUTXOSet(utxo_set_.get());
    assembler.SetBlockValidator(validator_.get());
    assembler.SetUTXOProvider(std::shared_ptr<consensus::IUTXOProvider>(
        utxo_set_.get(), [](auto*){}));
    assembler.setMempool(mempool_.get());

    auto block = assembler.CreateNewBlock(MINING_ADDRESS);
    ASSERT_NE(block, nullptr)
        << "Template hardening should salvage a coinbase-only block instead of failing";
    EXPECT_EQ(block->vtx.size(), 1u)
        << "Bad unchecked tx should be excluded, leaving only coinbase";
    EXPECT_TRUE(mempool_->hasTransaction(bad_txid))
        << "Template rescue should quarantine, not delete, the offending mempool tx";

    std::string exclusion_reason;
    EXPECT_TRUE(mempool_->isExcludedFromBlockTemplates(bad_txid, &exclusion_reason))
        << "Offending tx should be marked as template-excluded for future retries";
    EXPECT_FALSE(exclusion_reason.empty());

    auto block_again = assembler.CreateNewBlock(MINING_ADDRESS);
    ASSERT_NE(block_again, nullptr)
        << "Subsequent template creation should keep working with the exclusion in place";
    EXPECT_EQ(block_again->vtx.size(), 1u);
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
