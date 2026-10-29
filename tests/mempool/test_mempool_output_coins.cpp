// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include <gtest/gtest.h>
#include <chrono>
#include <algorithm>
#include <type_traits>
#ifdef DINERO_TEST_ORCHARD_BODY
#include "orchard_transaction.h"
#include <fstream>
#include <iterator>
#endif
#include <filesystem>
namespace {
using namespace dinero;
class MempoolOutputCoins : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_output_coins_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
        secret.back()=68;int parity=0;std::array<uint8_t,32> output{};
        ASSERT_TRUE(TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity));ASSERT_TRUE(TaprootKeys::ComputeTweakedPubkey(internal,output));
        script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
    }
    void TearDown() override {db.close();std::filesystem::remove_all(root);}
    OutPoint fund(uint8_t id) {
        uint256 hash;hash.data[0]=id;OutPoint out{TxId(hash),0};
        if(!coins.AddCoin(out,{AmountUna::Una(1000000),script,1,false}))throw std::runtime_error("fixture funding refused");return out;
    }
    Transaction spend(const OutPoint& out,uint64_t value,uint64_t fee) {
        Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=out.txid;tx.vin[0].prevout.vout=out.vout;tx.vin[0].sequence=0xfffffffd;
        tx.vout.emplace_back(AmountUna::Una(value-fee),script);
        CanonicalWalletUTXO coin;coin.txid=out.txid.AsUint256();coin.vout=out.vout;coin.value=AmountUna::Una(value);coin.spk=script;
        const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});if(bytes.size()!=32)throw std::runtime_error("fixture sighash refused");
        std::array<uint8_t,32> hash{};std::copy(bytes.begin(),bytes.end(),hash.begin());std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,hash,secret,internal))throw std::runtime_error("fixture signature refused");tx.vin[0].witness.emplace_back(signature.begin(),signature.end());return tx;
    }
};
TEST_F(MempoolOutputCoins, HistoricalOutputMetadataAndCopyRetention) {
    auto original = spend(fund(1), 1000000, 1000);
    TxOutput confidential(AmountUna::Una(0), std::vector<uint8_t>{0x51});
    confidential.is_confidential = true;
    confidential.commitment = std::vector<uint8_t>(33, 0x2a);
    original.vout.push_back(confidential);
    // Structural metadata fixture only: this modified body is not submitted.
    MempoolTransaction body(original);
    original.vout.clear();
    ASSERT_EQ(body.OutputCount(), 2U);
    auto ordinary = body.OutputCoin(0, 37);
    EXPECT_EQ(ordinary.value.GetUna(), 999000U);
    EXPECT_EQ(ordinary.scriptPubKey, script);
    EXPECT_EQ(ordinary.height, 37U);
    EXPECT_FALSE(ordinary.isCoinbase);
    EXPECT_FALSE(ordinary.is_confidential);
    EXPECT_TRUE(ordinary.commitment.empty());
    const auto hidden = body.OutputCoin(1, 38);
    EXPECT_EQ(hidden.value.GetUna(), 0U);
    EXPECT_EQ(hidden.scriptPubKey, std::vector<uint8_t>{0x51});
    EXPECT_EQ(hidden.height, 38U);
    EXPECT_FALSE(hidden.isCoinbase);
    EXPECT_TRUE(hidden.is_confidential);
    EXPECT_EQ(hidden.commitment, std::vector<uint8_t>(33, 0x2a));
    ordinary.scriptPubKey.clear();
    EXPECT_EQ(body.OutputCoin(0, 37).scriptPubKey, script);
    auto retained = body;
    body = MempoolTransaction{};
    EXPECT_EQ(retained.OutputCoin(1, 38).commitment, hidden.commitment);
}
TEST_F(MempoolOutputCoins, SignedParentChildReplacementAndBranchRemoval) {
    const auto funding = fund(2);
    const auto parent = spend(funding, 1000000, 1000);
    const OutPoint parent_output{parent.GetTxid(), 0};
    const auto child = spend(parent_output, 999000, 10000);
    const auto replacement = spend(parent_output, 999000, 20000);
    const OutPoint child_output{child.GetTxid(), 0};
    const OutPoint replacement_output{replacement.GetTxid(), 0};
    Mempool pool(&db, &coins);
    pool.setRBFEnabled(true);
    ASSERT_TRUE(pool.submitTransaction(parent, "fixture", false).accepted());
    ASSERT_FALSE(coins.GetCoin(parent_output));
    const auto parent_coin = pool.getCoinsView().getCoin(parent_output);
    ASSERT_EQ(parent_coin.status(), Status::Ok);
    EXPECT_EQ(parent_coin.value().value.GetUna(), 999000U);
    EXPECT_EQ(parent_coin.value().scriptPubKey, script);
    EXPECT_EQ(parent_coin.value().height, 110U);
    EXPECT_FALSE(parent_coin.value().isCoinbase);
    ASSERT_TRUE(pool.submitTransaction(child, "fixture", false).accepted());
    EXPECT_EQ(pool.getCoinsView().getCoin(parent_output).status(), Status::NotFound);
    const auto old_child = pool.getMempoolEntry(child.GetTxid().AsUint256());
    ASSERT_TRUE(old_child);
    // Preflight and replacement must recover the parent output hidden by the
    // old child, even though it is absent from the selected chain coin set.
    ASSERT_TRUE(pool.submitTransactionTestOnly(replacement, "fixture").accepted());
    pool.setMinFeeRate(100000);
    EXPECT_EQ(pool.submitTransaction(replacement, "fixture", false).code, TxRejectCode::INSUFFICIENT_FEE);
    EXPECT_EQ(pool.getCoinsView().getCoin(child_output).status(), Status::Ok);
    EXPECT_EQ(pool.getCoinsView().getCoin(replacement_output).status(), Status::NotFound);
    pool.setMinFeeRate(1);
    ASSERT_TRUE(pool.submitTransaction(replacement, "fixture", false).accepted());
    EXPECT_EQ(pool.getCoinsView().getCoin(child_output).status(), Status::NotFound);
    const auto new_coin = pool.getCoinsView().getCoin(replacement_output);
    ASSERT_EQ(new_coin.status(), Status::Ok);
    EXPECT_EQ(new_coin.value().value.GetUna(), 979000U);
    EXPECT_EQ(new_coin.value().scriptPubKey, script);
    EXPECT_EQ(old_child->tx.OutputCoin(0, old_child->height).value.GetUna(), 989000U);
    ASSERT_TRUE(pool.removeTransaction(replacement.GetTxid().AsUint256()));
    EXPECT_EQ(pool.getCoinsView().getCoin(parent_output).status(), Status::Ok);
    ASSERT_TRUE(pool.submitTransaction(replacement, "fixture", false).accepted());
    const auto replacement_entry = pool.getMempoolEntry(replacement.GetTxid().AsUint256());
    ASSERT_TRUE(replacement_entry);
    pool.setMaxSize(replacement_entry->tx_size);
    pool.limitMempoolSize();
    EXPECT_EQ(pool.size(), 0U);
    EXPECT_EQ(pool.getCoinsView().createdCount(), 0U);
    EXPECT_EQ(pool.getCoinsView().spentCount(), 0U);
    EXPECT_EQ(pool.getCoinsView().getCoin(funding).status(), Status::Ok);
    EXPECT_EQ(pool.getCoinsView().getCoin(parent_output).status(), Status::NotFound);
}
TEST_F(MempoolOutputCoins, EmptyAndOutOfRangeRefuse) {
    MempoolTransaction empty;
    EXPECT_THROW((void)empty.OutputCount(), std::logic_error);
    EXPECT_THROW((void)empty.OutputCoin(0, 0), std::logic_error);
    MempoolTransaction body(spend(fund(3), 1000000, 1000));
    EXPECT_THROW((void)body.OutputCoin(1, 110), std::out_of_range);
    EXPECT_THROW((void)body.OutputCoin(static_cast<size_t>(-1), 110), std::out_of_range);
    EXPECT_EQ(body.OutputCoin(0, 110).value.GetUna(), 999000U);
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolOutputCoins, OrchardOutputsRetainCanonicalMetadata) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    auto body = MempoolTransaction::FromOrchard(envelope);
    ASSERT_FALSE(envelope.Outputs().empty());
    ASSERT_EQ(body.OutputCount(), envelope.Outputs().size());
    for (size_t i = 0; i < body.OutputCount(); ++i) {
        const auto coin = body.OutputCoin(i, 111);
        EXPECT_EQ(coin.value.GetUna(), envelope.Outputs()[i].amount_una);
        EXPECT_EQ(coin.scriptPubKey, envelope.Outputs()[i].script_pub_key);
        EXPECT_EQ(coin.height, 111U);
        EXPECT_FALSE(coin.isCoinbase);
        EXPECT_FALSE(coin.is_confidential);
        EXPECT_TRUE(coin.commitment.empty());
    }
    auto retained = body;
    body = MempoolTransaction{};
    EXPECT_EQ(retained.OutputCoin(0, 111).value.GetUna(), 10000U);
    EXPECT_THROW((void)retained.OutputCoin(retained.OutputCount(), 111), std::out_of_range);
    EXPECT_THROW((void)retained.Historical(), std::logic_error);
    // No Orchard insertion/admission or proof-verification claim.
}
#endif
}
