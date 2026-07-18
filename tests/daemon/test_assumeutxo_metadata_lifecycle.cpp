#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "daemon/services/assumeutxo_state.h"
#include "daemon/snapshot_bootstrap_policy.h"
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "wallet/utxo_index.h"

namespace fs = std::filesystem;

namespace dinero {

class AssumeUTXOMetadataLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto unique_id = std::to_string(
            static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        temp_dir_ = fs::temp_directory_path() / ("dinero_assumeutxo_meta_" + unique_id);
        fs::create_directories(temp_dir_);

        utxo_index_ = std::make_unique<UTXOIndex>((temp_dir_ / "wallet.db").string());
        ASSERT_TRUE(utxo_index_->Initialize());
    }

    void TearDown() override {
        utxo_index_.reset();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    std::optional<std::string> ReadMetadata(const std::string& key) {
        return utxo_index_->GetMetadata(key);
    }

    bool active_ = false;
    uint256 base_block_;
    uint32_t base_height_ = 0;
    std::unique_ptr<UTXOIndex> utxo_index_;
    fs::path temp_dir_;
};

TEST_F(AssumeUTXOMetadataLifecycleTest, PersistedStateIsClearedWhenExitingAssumeUTXO) {
    const uint256 base_block = uint256::FromHexUnsafe(
        "00000000790bbd5855d9062cfb783a418f8f639b5052c9a542c9358b979f43e5");

    assumeutxo::SetState({active_, base_block_, base_height_, utxo_index_.get()},
                         base_block,
                         27727,
                         /*persist_metadata=*/true);

    ASSERT_TRUE(active_);
    ASSERT_EQ(base_block_, base_block);
    ASSERT_EQ(base_height_, 27727u);
    ASSERT_EQ(ReadMetadata(assumeutxo::kActiveKey).value_or(""), "true");
    ASSERT_EQ(ReadMetadata(assumeutxo::kBaseBlockKey).value_or(""), base_block.GetHex());
    ASSERT_EQ(ReadMetadata(assumeutxo::kBaseHeightKey).value_or(""), "27727");

    assumeutxo::ClearState({active_, base_block_, base_height_, utxo_index_.get()},
                           /*clear_persisted_metadata=*/true);

    EXPECT_FALSE(active_);
    EXPECT_TRUE(base_block_.IsNull());
    EXPECT_EQ(base_height_, 0u);
    EXPECT_FALSE(ReadMetadata(assumeutxo::kActiveKey).has_value());
    EXPECT_FALSE(ReadMetadata(assumeutxo::kBaseBlockKey).has_value());
    EXPECT_FALSE(ReadMetadata(assumeutxo::kBaseHeightKey).has_value());
}

TEST_F(AssumeUTXOMetadataLifecycleTest, InMemoryRestoreDoesNotRewritePersistedMetadata) {
    const uint256 persisted_block = uint256::FromHexUnsafe(
        "00000000790bbd5855d9062cfb783a418f8f639b5052c9a542c9358b979f43e5");
    const uint256 restored_block = uint256::FromHexUnsafe(
        "000000009070ff6d1cbacc466f8fe5fd1b6dddd1256eeee2adf73190a1a2a4a4");

    assumeutxo::SetState({active_, base_block_, base_height_, utxo_index_.get()},
                         persisted_block,
                         27727,
                         /*persist_metadata=*/true);
    assumeutxo::SetState({active_, base_block_, base_height_, utxo_index_.get()},
                         restored_block,
                         28551,
                         /*persist_metadata=*/false);

    EXPECT_TRUE(active_);
    EXPECT_EQ(base_block_, restored_block);
    EXPECT_EQ(base_height_, 28551u);

    EXPECT_EQ(ReadMetadata(assumeutxo::kActiveKey).value_or(""), "true");
    EXPECT_EQ(ReadMetadata(assumeutxo::kBaseBlockKey).value_or(""), persisted_block.GetHex());
    EXPECT_EQ(ReadMetadata(assumeutxo::kBaseHeightKey).value_or(""), "27727");
}

TEST_F(AssumeUTXOMetadataLifecycleTest, InMemoryClearDoesNotDeletePersistedMetadata) {
    const uint256 persisted_block = uint256::FromHexUnsafe(
        "00000000790bbd5855d9062cfb783a418f8f639b5052c9a542c9358b979f43e5");

    assumeutxo::SetState({active_, base_block_, base_height_, utxo_index_.get()},
                         persisted_block,
                         27727,
                         /*persist_metadata=*/true);

    assumeutxo::ClearState({active_, base_block_, base_height_, utxo_index_.get()},
                           /*clear_persisted_metadata=*/false);

    EXPECT_FALSE(active_);
    EXPECT_TRUE(base_block_.IsNull());
    EXPECT_EQ(base_height_, 0u);

    EXPECT_EQ(ReadMetadata(assumeutxo::kActiveKey).value_or(""), "true");
    EXPECT_EQ(ReadMetadata(assumeutxo::kBaseBlockKey).value_or(""), persisted_block.GetHex());
    EXPECT_EQ(ReadMetadata(assumeutxo::kBaseHeightKey).value_or(""), "27727");
}

TEST(SnapshotBootstrapPolicyTest, AcceptsOnlyExactGenesisCoinbaseState) {
    SelectParams(Chain::MAINNET);
    Transaction genesis_coinbase;
    ASSERT_TRUE(TransactionSerializer::Deserialize(
        genesis_coinbase, Params().genesis.genesisCoinbaseHex));
    ASSERT_TRUE(genesis_coinbase.IsCoinbase());

    consensus::ConsensusUTXOSet utxo_set;
    const TxId genesis_txid = genesis_coinbase.GetTxid();
    for (uint32_t vout = 0; vout < genesis_coinbase.vout.size(); ++vout) {
        const auto& output = genesis_coinbase.vout[vout];
        ASSERT_TRUE(utxo_set.AddCoin(
            OutPoint(genesis_txid, vout),
            consensus::UTXOEntry(
                output.value, output.scriptPubKey, 0, true,
                output.is_confidential, output.commitment)));
    }

    EXPECT_TRUE(assumeutxo::IsGenesisOnlyUtxoSet(
        utxo_set, genesis_coinbase));

    const OutPoint first_genesis(genesis_txid, 0);
    ASSERT_TRUE(utxo_set.DeleteCoin(first_genesis));
    const uint256 impostor_hash = uint256::FromHexUnsafe(
        "0100000000000000000000000000000000000000000000000000000000000000");
    ASSERT_TRUE(utxo_set.AddCoin(
        OutPoint(TxId(impostor_hash), 0),
        consensus::UTXOEntry(AmountUna::Una(1), {0x51}, 1, false)));

    // Same cardinality is not enough: real post-genesis state must never be
    // cleared for snapshot import.
    EXPECT_FALSE(assumeutxo::IsGenesisOnlyUtxoSet(
        utxo_set, genesis_coinbase));
}

} // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
