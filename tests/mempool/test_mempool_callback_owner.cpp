// Patched-path ownership invariants only. Never run this fixture against an
// unsafe original or a control that removes synchronization/lifetime ownership.
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
#include <memory>
namespace {
using namespace dinero;
class MempoolCallbackOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_callback_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
    }
    void TearDown() override {db.close();std::filesystem::remove_all(root);}
    Transaction fund(uint8_t id) {
        uint256 hash;hash.data[0]=id;OutPoint out{TxId(hash),0};
        std::array<uint8_t,32> secret{},internal{},output{};secret.back()=67;int parity=0;
        if(!TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity) || !TaprootKeys::ComputeTweakedPubkey(internal,output))throw std::runtime_error("fixture key derivation refused");
        std::vector<uint8_t> script{0x51,0x20};script.insert(script.end(),output.begin(),output.end());
        if(!coins.AddCoin(out,{AmountUna::Una(100000),script,1,false}))throw std::runtime_error("fixture funding refused");
        Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=out.txid;tx.vin[0].prevout.vout=0;
        tx.vout.emplace_back(AmountUna::Una(90000),script);
        CanonicalWalletUTXO coin;coin.txid=hash;coin.vout=0;coin.value=AmountUna::Una(100000);coin.spk=script;
        const auto hash_bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});
        if(hash_bytes.size()!=32)throw std::runtime_error("fixture sighash refused");
        std::array<uint8_t,32> sighash{};std::copy(hash_bytes.begin(),hash_bytes.end(),sighash.begin());
        std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,sighash,secret,internal))throw std::runtime_error("fixture signature refused");
        tx.vin[0].witness.emplace_back(signature.begin(),signature.end());
        return tx;
    }

    struct Guard final:Mempool::ChainstateReadGuard {
        bool& held;explicit Guard(bool& state):held(state){EXPECT_FALSE(held);held=true;}~Guard() override{held=false;}
    };
};
TEST_F(MempoolCallbackOwner, OwnedCallbacksAfterLocksAndReplacementSnapshot) {
    const auto tx=fund(1);Mempool pool(&db,&coins);bool held=false;pool.setChainstateReadGuardFactory([&]{return std::make_unique<Guard>(held);});
    int accepted=0,old_broadcast=0,new_broadcast=0;auto owner=std::make_shared<int>(17);std::weak_ptr<int> weak=owner;
    pool.setTxBroadcastCallback([&,owner](const uint256& id){EXPECT_FALSE(held);if(held)return;EXPECT_EQ(*owner,17);EXPECT_EQ(id,tx.GetTxid().AsUint256());EXPECT_TRUE(pool.hasTransaction(id));++old_broadcast;});owner.reset();
    pool.setTxAcceptedCallback([&](const Transaction& body){EXPECT_FALSE(held);if(held)return;++accepted;EXPECT_EQ(body.GetTxid(),tx.GetTxid());EXPECT_TRUE(pool.hasTransaction(tx.GetTxid().AsUint256()));pool.setTxAcceptedCallback({});pool.setTxBroadcastCallback([&](const uint256&){++new_broadcast;});EXPECT_FALSE(weak.expired());});
    const auto result=pool.submitTransaction(tx,"fixture",true);ASSERT_TRUE(result.accepted())<<result.message;EXPECT_FALSE(held);EXPECT_EQ(accepted,1);EXPECT_EQ(old_broadcast,1);EXPECT_EQ(new_broadcast,0);EXPECT_TRUE(weak.expired());pool.broadcastTransaction(tx.GetTxid().AsUint256());EXPECT_EQ(new_broadcast,1);
}
TEST_F(MempoolCallbackOwner, PreflightDoesNotPublishOrNotify) {
    const auto tx=fund(2);Mempool pool(&db,&coins);bool held=false;pool.setChainstateReadGuardFactory([&]{return std::make_unique<Guard>(held);});int calls=0;
    pool.setTxAcceptedCallback([&](const Transaction&){++calls;});pool.setTxBroadcastCallback([&](const uint256&){++calls;});
    const auto result=pool.submitTransactionTestOnly(tx,"fixture");ASSERT_TRUE(result.accepted())<<result.message;EXPECT_FALSE(held);EXPECT_EQ(calls,0);EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));EXPECT_FALSE(pool.isOutputSpentInMempool({tx.vin[0].prevout.txid,tx.vin[0].prevout.vout}));
}
TEST_F(MempoolCallbackOwner, CallbackExceptionPreservesAdmissionAndReleasesOwners) {
    const auto tx=fund(3);Mempool pool(&db,&coins);bool held=false;pool.setChainstateReadGuardFactory([&]{return std::make_unique<Guard>(held);});int broadcasts=0;
    pool.setTxAcceptedCallback([&](const Transaction&){EXPECT_FALSE(held);throw std::runtime_error("fixture observer failed after publication");});pool.setTxBroadcastCallback([&](const uint256&){++broadcasts;});
    EXPECT_THROW(pool.submitTransaction(tx,"fixture",true),std::runtime_error);EXPECT_FALSE(held);EXPECT_EQ(broadcasts,0);EXPECT_TRUE(pool.hasTransaction(tx.GetTxid().AsUint256()));EXPECT_TRUE(pool.isOutputSpentInMempool({tx.vin[0].prevout.txid,tx.vin[0].prevout.vout}));pool.setTxAcceptedCallback({});EXPECT_TRUE(pool.removeTransaction(tx.GetTxid().AsUint256()));
}
TEST_F(MempoolCallbackOwner, CleanupCompletesBeforeExternalObservers) {
    const auto tx=fund(4);Mempool pool(&db,&coins);pool.setMaxSize(1);bool held=false;pool.setChainstateReadGuardFactory([&]{return std::make_unique<Guard>(held);});int calls=0;
    pool.setTxAcceptedCallback([&](const Transaction&){EXPECT_FALSE(held);if(held)return;++calls;EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));});pool.setTxBroadcastCallback([&](const uint256&){EXPECT_FALSE(held);if(held)return;++calls;EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));});
    const auto result=pool.submitTransaction(tx,"fixture",true);ASSERT_TRUE(result.accepted())<<result.message;EXPECT_EQ(calls,2);EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));EXPECT_FALSE(held);
}
}
