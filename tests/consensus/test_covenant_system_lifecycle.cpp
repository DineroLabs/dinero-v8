#include "consensus/block_validation.h"
#include "consensus/chainparams.h"
#include "consensus/covenants.h"
#include "consensus/script.h"
#include "consensus/script_verify.h"
#include "consensus/transaction_validator.h"
#include "consensus/utreexo_accumulator.h"
#include "daemon/mempool.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using dinero::AmountUna;
using dinero::Block;
using dinero::Chain;
using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::Coin;
using dinero::Mempool;
using dinero::OutPoint;
using dinero::SelectParams;
using dinero::Status;
using dinero::Transaction;
using dinero::TxId;
using dinero::TxOutput;
using dinero::arith_uint256;
using dinero::consensus::BlockUndo;
using dinero::consensus::BlockValidator;
using dinero::consensus::ContractState;
using dinero::consensus::IConsensusUTXOSet;
using dinero::consensus::UTXOEntry;
using dinero::consensus::UTXOSnapshot;
using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;

class InMemoryConsensusUTXOSet final : public IConsensusUTXOSet {
public:
    bool AddCoin(const OutPoint& outpoint, const UTXOEntry& coin) override {
        return coins_.emplace(outpoint, coin).second;
    }

    std::unique_ptr<UTXOEntry> SpendCoin(
        const OutPoint& outpoint) override {
        auto it = coins_.find(outpoint);
        if (it == coins_.end()) return nullptr;
        auto coin = std::make_unique<UTXOEntry>(it->second);
        coins_.erase(it);
        return coin;
    }

    const UTXOEntry* GetCoin(const OutPoint& outpoint) const override {
        const auto it = coins_.find(outpoint);
        return it == coins_.end() ? nullptr : &it->second;
    }

    bool HaveCoin(const OutPoint& outpoint) const override {
        return coins_.count(outpoint) != 0;
    }

    bool DeleteCoin(const OutPoint& outpoint) override {
        return coins_.erase(outpoint) != 0;
    }

    bool ApplyBlock(
        const Block&, uint32_t, const dinero::uint256&,
        BlockUndo&, UtreexoHash&, std::string& error) override {
        error = "not used by covenant lifecycle test";
        return false;
    }

    bool UndoBlock(
        const Block&, uint32_t, const BlockUndo&,
        std::string& error) override {
        error = "not used by covenant lifecycle test";
        return false;
    }

    bool SupportsSnapshotRestore() const override { return false; }
    UTXOSnapshot Snapshot() const override { return {}; }
    void Restore(const UTXOSnapshot&) override {}
    uint32_t GetHeight() const override { return height_; }
    const dinero::uint256& GetBestBlock() const override {
        return best_block_;
    }
    void SetBestBlock(
        const dinero::uint256& hash, uint32_t height) override {
        best_block_ = hash;
        height_ = height;
    }
    UtreexoHash GetUtreexoRoot() const override {
        return forest_.getCommitment();
    }
    UtreexoForest& GetForest() override { return forest_; }
    const UtreexoForest& GetForest() const override { return forest_; }
    size_t GetSetSize() const override { return coins_.size(); }
    size_t GetMemoryUsage() const override { return sizeof(*this); }
    void Clear() override {
        coins_.clear();
        forest_ = UtreexoForest();
    }

    std::optional<UTXOEntry> GetUTXO(
        const OutPoint& outpoint) const override {
        const auto* coin = GetCoin(outpoint);
        return coin ? std::optional<UTXOEntry>(*coin) : std::nullopt;
    }
    bool AddUTXO(
        const OutPoint& outpoint, const UTXOEntry& entry) override {
        return AddCoin(outpoint, entry);
    }
    bool SpendUTXO(const OutPoint& outpoint, uint32_t) override {
        return coins_.erase(outpoint) != 0;
    }
    bool DeleteUTXO(const OutPoint& outpoint) override {
        return DeleteCoin(outpoint);
    }
    bool HasUTXO(const OutPoint& outpoint) const override {
        return HaveCoin(outpoint);
    }

private:
    std::unordered_map<OutPoint, UTXOEntry> coins_;
    UtreexoForest forest_;
    dinero::uint256 best_block_;
    uint32_t height_ = 0;
};

struct CovenantSpend {
    Transaction tx;
    OutPoint funding_outpoint;
    UTXOEntry funding_coin;
};

void CommitWitnessScriptToFundingCoin(CovenantSpend& spend) {
    ASSERT_EQ(spend.tx.vin.size(), 1U);
    ASSERT_FALSE(spend.tx.vin[0].witness.empty());
    const auto& script = spend.tx.vin[0].witness[0];

    const auto leaf_hash =
        dinero::consensus::TapLeafHash(0xc0, script);
    std::array<uint8_t, 32> merkle_root{};
    std::copy(leaf_hash.begin(), leaf_hash.end(), merkle_root.begin());

    ContractState key_seed{};
    key_seed.stateHash[31] = 1;
    std::array<uint8_t, 32> internal_key{};
    ASSERT_TRUE(dinero::consensus::DeriveContractInternalKey(
        key_seed, internal_key));

    std::vector<uint8_t> spent_script;
    uint8_t parity = 0;
    ASSERT_TRUE(dinero::consensus::ComputeContractOutputScript(
        key_seed, merkle_root, spent_script, &parity));

    std::vector<uint8_t> control_block{
        static_cast<uint8_t>(0xc0 | parity)};
    control_block.insert(
        control_block.end(), internal_key.begin(), internal_key.end());
    spend.tx.vin[0].witness = {script, control_block};
    spend.funding_coin = UTXOEntry(
        AmountUna::Una(100'000), spent_script,
        1, false);
}

CovenantSpend BuildCTVSpend() {
    CovenantSpend spend;
    spend.funding_outpoint = OutPoint{
        TxId(dinero::uint256::FromHexUnsafe(
            "000000000000000000000000000000000000000000000000000000000000c0de")),
        0};

    spend.tx.version = 2;
    spend.tx.vin.emplace_back();
    spend.tx.vin[0].prevout.txid = spend.funding_outpoint.txid;
    spend.tx.vin[0].prevout.vout = spend.funding_outpoint.vout;
    spend.tx.vin[0].sequence = 0xfffffffeU;
    spend.tx.vout.emplace_back(
        AmountUna::Una(99'000),
        std::vector<uint8_t>{dinero::consensus::OP_TRUE});

    const auto template_hash =
        dinero::consensus::ComputeCTVHash(spend.tx, 0);
    std::vector<uint8_t> script{32};
    script.insert(script.end(), template_hash.begin(), template_hash.end());
    script.push_back(
        static_cast<uint8_t>(
            dinero::consensus::OP_CHECKTEMPLATEVERIFY));
    spend.tx.vin[0].witness = {script};
    CommitWitnessScriptToFundingCoin(spend);
    return spend;
}

std::filesystem::path UniqueTestRoot() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("dinero_covenant_lifecycle_" + std::to_string(stamp));
}

void SetTip(ChainDB& db, uint32_t height, uint8_t discriminator) {
    std::string hash_hex(64, '0');
    hash_hex[62] = "0123456789abcdef"[discriminator >> 4];
    hash_hex[63] = "0123456789abcdef"[discriminator & 0x0f];
    const auto token = ChainWriteToken::CreateForTesting();
    ASSERT_EQ(
        db.setTip(
            token,
            dinero::uint256::FromHexUnsafe(hash_hex),
            static_cast<int>(height),
            arith_uint256(height)),
        Status::Ok);
}

void PutFundingCoin(
    ChainDB& db, const CovenantSpend& spend) {
    Coin coin;
    coin.amount = spend.funding_coin.value.GetUna();
    coin.script_pubkey =
        dinero::TransactionSerializer::ToHex(
            spend.funding_coin.scriptPubKey);
    coin.height = static_cast<int>(spend.funding_coin.height);
    coin.coinbase = spend.funding_coin.isCoinbase;
    const auto token = ChainWriteToken::CreateForTesting();
    ASSERT_EQ(
        db.putCoin(
            token,
            spend.funding_outpoint.txid.AsUint256(),
            spend.funding_outpoint.vout,
            coin),
        Status::Ok);
}

class CovenantSystemLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        SelectParams(Chain::REGTEST);
        root_ = UniqueTestRoot();
        ASSERT_TRUE(std::filesystem::create_directories(root_));
        ASSERT_EQ(db_.init(root_ / "chaindb"), Status::Ok);
        spend_ = BuildCTVSpend();
        PutFundingCoin(db_, spend_);
    }

    void TearDown() override {
        db_.close();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    std::filesystem::path root_;
    ChainDB db_;
    CovenantSpend spend_;
};

TEST_F(
    CovenantSystemLifecycleTest,
    BlockValidationEnforcesActivationAndRejectsTemplateMutation) {
    InMemoryConsensusUTXOSet utxos;
    ASSERT_TRUE(utxos.AddCoin(
        spend_.funding_outpoint, spend_.funding_coin));
    BlockValidator validator(&utxos);

    uint64_t input_value = 0;
    std::string error;
    EXPECT_FALSE(validator.ValidateTransaction(
        spend_.tx, 19, false, input_value, error));

    error.clear();
    EXPECT_TRUE(validator.ValidateTransaction(
        spend_.tx, 20, false, input_value, error))
        << error;
    EXPECT_EQ(input_value, 100'000U);

    Transaction mutated = spend_.tx;
    mutated.vout[0].value = AmountUna::Una(98'999);
    error.clear();
    EXPECT_FALSE(validator.ValidateTransaction(
        mutated, 20, false, input_value, error));

    Transaction bad_control_block = spend_.tx;
    ASSERT_GT(bad_control_block.vin[0].witness[1].size(), 1U);
    bad_control_block.vin[0].witness[1][1] ^= 0x01;
    error.clear();
    EXPECT_FALSE(validator.ValidateTransaction(
        bad_control_block, 20, false, input_value, error));

    CovenantSpend wrong_template = spend_;
    ASSERT_EQ(wrong_template.tx.vin[0].witness[0].size(), 34U);
    wrong_template.tx.vin[0].witness[0][1] ^= 0x01;
    CommitWitnessScriptToFundingCoin(wrong_template);
    InMemoryConsensusUTXOSet wrong_template_utxos;
    ASSERT_TRUE(wrong_template_utxos.AddCoin(
        wrong_template.funding_outpoint,
        wrong_template.funding_coin));
    BlockValidator wrong_template_validator(&wrong_template_utxos);
    error.clear();
    EXPECT_FALSE(wrong_template_validator.ValidateTransaction(
        wrong_template.tx, 20, false, input_value, error));
}

TEST_F(
    CovenantSystemLifecycleTest,
    AdversarialWitnessesNeverEnterMempoolOrMiningTemplate) {
    SetTip(db_, 19, 0x19);

    Transaction bad_control_block = spend_.tx;
    ASSERT_GT(bad_control_block.vin[0].witness[1].size(), 1U);
    bad_control_block.vin[0].witness[1][1] ^= 0x01;

    Mempool pool(&db_);
    pool.setMinFeeRate(0);
    const auto bad_control_result = pool.submitTransaction(
        bad_control_block, "covenant-adversarial-test", false);
    EXPECT_FALSE(bad_control_result.accepted());
    EXPECT_EQ(pool.size(), 0U);
    EXPECT_TRUE(pool.selectTransactionsForBlock(
        1'000'000, 4'000'000, 20).empty());

    CovenantSpend wrong_template = spend_;
    ASSERT_EQ(wrong_template.tx.vin[0].witness[0].size(), 34U);
    wrong_template.tx.vin[0].witness[0][1] ^= 0x01;
    CommitWitnessScriptToFundingCoin(wrong_template);
    PutFundingCoin(db_, wrong_template);

    const auto wrong_template_result = pool.submitTransaction(
        wrong_template.tx, "covenant-adversarial-test", false);
    EXPECT_FALSE(wrong_template_result.accepted());
    EXPECT_EQ(pool.size(), 0U);
    EXPECT_TRUE(pool.selectTransactionsForBlock(
        1'000'000, 4'000'000, 20).empty());
}

TEST_F(
    CovenantSystemLifecycleTest,
    MempoolMiningReorgAndRestartRespectActivationState) {
    auto& params = dinero::MutableParams();
    const uint32_t saved_scriptpath =
        params.taproot_scriptpath_activation_height;
    params.taproot_scriptpath_activation_height = 1;
    struct RestoreScriptPath {
        dinero::ChainParams& params;
        uint32_t height;
        ~RestoreScriptPath() {
            params.taproot_scriptpath_activation_height = height;
        }
    } restore{params, saved_scriptpath};

    const auto persistence_file = root_ / "mempool.dat";

    SetTip(db_, 18, 0x18);
    {
        Mempool pool(&db_);
        pool.setMinFeeRate(0);
        const auto pre_activation =
            pool.submitTransaction(spend_.tx, "covenant-system-test", false);
        EXPECT_FALSE(pre_activation.accepted());
        EXPECT_NE(
            pre_activation.message.find(
                "premature revealed OP_CHECKTEMPLATEVERIFY"),
            std::string::npos)
            << pre_activation.message;
        EXPECT_EQ(pool.size(), 0U);

        SetTip(db_, 19, 0x19);
        const auto active =
            pool.submitTransaction(spend_.tx, "covenant-system-test", false);
        ASSERT_TRUE(active.accepted()) << active.message;
        ASSERT_EQ(pool.size(), 1U);

        const auto selected_active =
            pool.selectTransactionsForBlock(1'000'000, 4'000'000, 20);
        ASSERT_EQ(selected_active.size(), 1U);
        EXPECT_EQ(
            selected_active[0].GetTxid(),
            spend_.tx.GetTxid());
        ASSERT_TRUE(pool.saveToDisk(persistence_file.string()));

        SetTip(db_, 18, 0x28);
        pool.onBlockDisconnected(Block{}, 19);
        EXPECT_TRUE(
            pool.selectTransactionsForBlock(
                1'000'000, 4'000'000, 19).empty());
    }

    {
        Mempool restarted_pre_activation(&db_);
        restarted_pre_activation.setMinFeeRate(0);
        EXPECT_TRUE(restarted_pre_activation.loadFromDisk(
            persistence_file.string()));
        EXPECT_EQ(restarted_pre_activation.size(), 0U);
    }

    SetTip(db_, 19, 0x29);
    {
        Mempool restarted_active(&db_);
        restarted_active.setMinFeeRate(0);
        EXPECT_TRUE(restarted_active.loadFromDisk(
            persistence_file.string()));
        EXPECT_EQ(restarted_active.size(), 1U);
    }
}

TEST_F(
    CovenantSystemLifecycleTest,
    Mainnet99998Through100001MempoolRestartAndReorgBoundary) {
    SelectParams(Chain::MAINNET);
    const auto persistence_file = root_ / "mainnet-boundary-mempool.dat";

    // Tip 99,998 admits transactions for candidate block 99,999, where the
    // revealed CTV opcode is still premature policy even though consensus
    // retains historical NOP4 behavior.
    SetTip(db_, 99'998, 0x91);
    {
        Mempool pool(&db_);
        pool.setMinFeeRate(0);
        const auto premature =
            pool.submitTransaction(spend_.tx, "mainnet-boundary", false);
        EXPECT_FALSE(premature.accepted());
        EXPECT_NE(
            premature.message.find(
                "premature revealed OP_CHECKTEMPLATEVERIFY"),
            std::string::npos)
            << premature.message;

        // Tip 99,999 admits for activation block 100,000 and selects the same
        // transaction for both activation and post-activation templates.
        SetTip(db_, 99'999, 0x92);
        const auto active =
            pool.submitTransaction(spend_.tx, "mainnet-boundary", false);
        ASSERT_TRUE(active.accepted()) << active.message;
        ASSERT_EQ(
            pool.selectTransactionsForBlock(1'000'000, 4'000'000, 100'000)
                .size(),
            1U);
        ASSERT_EQ(
            pool.selectTransactionsForBlock(1'000'000, 4'000'000, 100'001)
                .size(),
            1U);
        ASSERT_TRUE(pool.saveToDisk(persistence_file.string()));

        // A reorg back below the boundary makes the persisted transaction
        // unselectable and triggers height-gated revalidation.
        SetTip(db_, 99'998, 0x93);
        pool.onBlockDisconnected(Block{}, 99'999);
        EXPECT_TRUE(
            pool.selectTransactionsForBlock(
                1'000'000, 4'000'000, 99'999).empty());
    }

    {
        Mempool restarted_preactivation(&db_);
        restarted_preactivation.setMinFeeRate(0);
        EXPECT_TRUE(restarted_preactivation.loadFromDisk(
            persistence_file.string()));
        EXPECT_EQ(restarted_preactivation.size(), 0U);
    }

    SetTip(db_, 99'999, 0x94);
    {
        Mempool restarted_active(&db_);
        restarted_active.setMinFeeRate(0);
        EXPECT_TRUE(restarted_active.loadFromDisk(
            persistence_file.string()));
        EXPECT_EQ(restarted_active.size(), 1U);
    }
}

} // namespace
