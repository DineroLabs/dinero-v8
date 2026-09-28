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
#include <filesystem>
namespace {
using namespace dinero;
class MempoolMaintenanceOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_maintenance_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
TEST_F(MempoolMaintenanceOwner, ConfirmationRetainsChildrenAndCoherentOverlay) {
    Mempool pool(&db,&coins);const auto funding=fund(1);const auto parent=spend(funding,1000000,1000);
    const auto child=spend({parent.GetTxid(),0},999000,1000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
    ASSERT_TRUE(coins.SpendCoin(funding));ASSERT_TRUE(coins.AddCoin({parent.GetTxid(),0},{parent.vout[0].value,script,110,false}));
    pool.removeConfirmedTransactions({parent.GetTxid().AsUint256()});
    EXPECT_FALSE(pool.hasTransaction(parent.GetTxid().AsUint256()));EXPECT_TRUE(pool.hasTransaction(child.GetTxid().AsUint256()));EXPECT_EQ(pool.size(),1U);
    EXPECT_TRUE(pool.isOutputSpentInMempool({parent.GetTxid(),0}));
    EXPECT_EQ(pool.getCoinsView().getCoin({parent.GetTxid(),0}).status(),Status::NotFound);
    EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
    const auto grandchild=spend({child.GetTxid(),0},998000,1000);EXPECT_TRUE(pool.submitTransaction(grandchild,"fixture",false).accepted());
    pool.removeConfirmedTransactions({parent.GetTxid().AsUint256()});EXPECT_EQ(pool.size(),2U);
}
TEST_F(MempoolMaintenanceOwner, ExpiryRebuildsAndAllowsFreshAdmission) {
    Mempool pool(&db,&coins);const auto funding=fund(2);const auto tx=spend(funding,1000000,1000);
    ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());pool.setMaxAge(std::chrono::hours(0));pool.removeExpiredTransactions();
    EXPECT_EQ(pool.size(),0U);EXPECT_FALSE(pool.isOutputSpentInMempool(funding));EXPECT_EQ(pool.getCoinsView().getCoin(funding).status(),Status::Ok);
    EXPECT_EQ(pool.getCoinsView().getCoin({tx.GetTxid(),0}).status(),Status::NotFound);
    pool.setMaxAge(std::chrono::hours(24));EXPECT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());pool.removeExpiredTransactions();EXPECT_EQ(pool.size(),1U);
}
TEST_F(MempoolMaintenanceOwner, SizeMaintenanceAndClearPermitCoherentRefill) {
    Mempool pool(&db,&coins);const auto funding=fund(3);const auto parent=spend(funding,1000000,1000);const auto child=spend({parent.GetTxid(),0},999000,5000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());pool.limitMempoolSize();EXPECT_EQ(pool.size(),2U);
    const auto child_entry=pool.getMempoolEntry(child.GetTxid().AsUint256());ASSERT_TRUE(child_entry.has_value());
    pool.setMaxSize(child_entry->tx_size);pool.limitMempoolSize();EXPECT_EQ(pool.size(),0U);EXPECT_FALSE(pool.isOutputSpentInMempool(funding));EXPECT_EQ(pool.getCoinsView().createdCount(),0U);
    pool.setMaxSize(1000000);ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
    pool.clear();EXPECT_EQ(pool.size(),0U);EXPECT_EQ(pool.getCoinsView().createdCount(),0U);EXPECT_EQ(pool.getCoinsView().spentCount(),0U);
    EXPECT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());EXPECT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
}
}
