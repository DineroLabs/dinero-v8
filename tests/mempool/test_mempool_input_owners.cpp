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
#include <filesystem>
namespace {
using namespace dinero;
class MempoolInputOwners : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_input_owners_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
        secret.back()=67;int parity=0;std::array<uint8_t,32> output{};
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
TEST_F(MempoolInputOwners, AdmissionAndReplacementPreserveExactOwners) {
    Mempool pool(&db, &coins);
    pool.setRBFEnabled(true);
    const auto funding = fund(1);
    const auto first = spend(funding, 1000000, 10000);
    const auto replacement = spend(funding, 1000000, 20000);
    const auto first_id = first.GetTxid().AsUint256();
    const auto replacement_id = replacement.GetTxid().AsUint256();
    EXPECT_TRUE(pool.getInputSpenders(funding).empty());
    ASSERT_TRUE(pool.submitTransaction(first, "fixture", false).accepted());
    const auto captured = pool.getInputSpenders(funding);
    EXPECT_EQ(captured, std::vector<uint256>{first_id});
    ASSERT_TRUE(pool.submitTransactionTestOnly(replacement, "fixture").accepted());
    EXPECT_EQ(pool.getInputSpenders(funding), captured);
    pool.setMinFeeRate(100000);
    EXPECT_EQ(pool.submitTransaction(replacement, "fixture", false).code, TxRejectCode::INSUFFICIENT_FEE);
    EXPECT_EQ(pool.getInputSpenders(funding), captured);
    pool.setMinFeeRate(1);
    ASSERT_TRUE(pool.submitTransaction(replacement, "fixture", false).accepted());
    EXPECT_EQ(pool.getInputSpenders(funding), std::vector<uint256>{replacement_id});
    EXPECT_EQ(captured, std::vector<uint256>{first_id});
    EXPECT_FALSE(pool.removeTransaction(first_id));
    EXPECT_EQ(pool.getInputSpenders(funding), std::vector<uint256>{replacement_id});
    ASSERT_TRUE(pool.removeTransaction(replacement_id));
    EXPECT_TRUE(pool.getInputSpenders(funding).empty());
    EXPECT_FALSE(pool.isOutputSpentInMempool(funding));
}
TEST_F(MempoolInputOwners, PreparedPublicationAndConfirmationRetainChildOwner) {
    Mempool pool(&db, &coins);
    const auto funding = fund(2);
    const auto parent = spend(funding, 1000000, 1000);
    const OutPoint output{parent.GetTxid(), 0};
    const auto child = spend(output, 999000, 1000);
    const auto parent_id = parent.GetTxid().AsUint256();
    const auto child_id = child.GetTxid().AsUint256();
    ASSERT_TRUE(pool.submitTransaction(parent, "fixture", false).accepted());
    ASSERT_TRUE(pool.submitTransaction(child, "fixture", false).accepted());
    ConnectedBlockEffects conflict;
    conflict.spent_transparent_inputs.push_back(funding);
    { auto prepared = pool.prepareBlockConnected(conflict, 111);
      ASSERT_TRUE(prepared); EXPECT_EQ(prepared->EvictedCount(), 2U); }
    EXPECT_EQ(pool.getInputSpenders(funding), std::vector<uint256>{parent_id});
    EXPECT_EQ(pool.getInputSpenders(output), std::vector<uint256>{child_id});
    ASSERT_TRUE(coins.SpendCoin(funding));
    ASSERT_TRUE(coins.AddCoin(output, {parent.vout[0].value, script, 111, false}));
    pool.removeConfirmedTransactions({parent_id});
    EXPECT_TRUE(pool.getInputSpenders(funding).empty());
    EXPECT_EQ(pool.getInputSpenders(output), std::vector<uint256>{child_id});
    ConnectedBlockEffects confirmed;
    confirmed.confirmed_txids.push_back(child_id);
    confirmed.spent_transparent_inputs.push_back(output);
    { auto prepared = pool.prepareBlockConnected(confirmed, 112);
      ASSERT_TRUE(prepared); prepared->PublishAfterCommit(); }
    EXPECT_TRUE(pool.getInputSpenders(output).empty());
    EXPECT_EQ(pool.size(), 0U);
}
TEST_F(MempoolInputOwners, MaintenanceAndClearReleaseOwners) {
    Mempool pool(&db, &coins);
    const auto funding = fund(3);
    const auto tx = spend(funding, 1000000, 1000);
    const auto id = tx.GetTxid().AsUint256();
    ASSERT_TRUE(pool.submitTransaction(tx, "fixture", false).accepted());
    pool.setMaxAge(std::chrono::hours(0));
    pool.removeExpiredTransactions();
    EXPECT_TRUE(pool.getInputSpenders(funding).empty());
    pool.setMaxAge(std::chrono::hours(24));
    ASSERT_TRUE(pool.submitTransaction(tx, "fixture", false).accepted());
    pool.setMaxSize(1);
    pool.limitMempoolSize();
    EXPECT_TRUE(pool.getInputSpenders(funding).empty());
    pool.setMaxSize(1000000);
    ASSERT_TRUE(pool.submitTransaction(tx, "fixture", false).accepted());
    EXPECT_EQ(pool.getInputSpenders(funding), std::vector<uint256>{id});
    pool.clear();
    EXPECT_TRUE(pool.getInputSpenders(funding).empty());
    ASSERT_TRUE(pool.submitTransaction(tx, "fixture", false).accepted());
    EXPECT_EQ(pool.getInputSpenders(funding), std::vector<uint256>{id});
}
TEST_F(MempoolInputOwners, SyntheticSharedInputRemovalRetainsRemainingOwner) {
    Mempool pool(&db, &coins);
    const auto funding = fund(4);
    const auto first = spend(funding, 1000000, 1000);
    const auto second = spend(funding, 1000000, 2000);
    const auto first_id = first.GetTxid().AsUint256();
    const auto second_id = second.GetTxid().AsUint256();
    // Explicit synthetic state: canonical ingress still enforces conflict policy.
    pool.addUnchecked(first);
    pool.addUnchecked(second);
    pool.addUnchecked(first);
    std::vector<uint256> both{first_id, second_id};
    std::sort(both.begin(), both.end());
    EXPECT_EQ(pool.getInputSpenders(funding), both);
    ASSERT_TRUE(pool.removeTransaction(first_id));
    EXPECT_TRUE(pool.isOutputSpentInMempool(funding));
    EXPECT_EQ(pool.getInputSpenders(funding), std::vector<uint256>{second_id});
    pool.removeConfirmedTransactions({second_id});
    EXPECT_FALSE(pool.isOutputSpentInMempool(funding));
    EXPECT_TRUE(pool.getInputSpenders(funding).empty());
}
}
