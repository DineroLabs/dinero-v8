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
class MempoolReplacementPolicy : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_replacement_policy_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
TEST_F(MempoolReplacementPolicy, FeePolicyRefusalPreservesExistingPool) {
    const auto out=fund(1);Mempool pool(&db,&coins);pool.setRBFEnabled(true);const auto first=spend(out,1000000,10000);ASSERT_TRUE(pool.submitTransaction(first,"fixture",false).accepted());
    const auto replacement=spend(out,1000000,20000);int callbacks=0;pool.setTxAcceptedCallback([&](const Transaction&){++callbacks;});pool.setMinFeeRate(100000);
    const auto before=pool.getTransactionIds();const auto preflight=pool.submitTransactionTestOnly(replacement,"fixture");EXPECT_EQ(preflight.code,TxRejectCode::INSUFFICIENT_FEE);
    const auto result=pool.submitTransaction(replacement,"fixture",false);EXPECT_EQ(result.code,TxRejectCode::INSUFFICIENT_FEE);EXPECT_EQ(pool.getTransactionIds(),before);EXPECT_TRUE(pool.isOutputSpentInMempool(out));EXPECT_EQ(callbacks,0);
    auto retained=pool.getTransaction(first.GetTxid().AsUint256());ASSERT_TRUE(retained);EXPECT_EQ(retained->Serialize(),first.Serialize());
    pool.setMinFeeRate(1);ASSERT_TRUE(pool.submitTransaction(replacement,"fixture",false).accepted());EXPECT_FALSE(pool.hasTransaction(first.GetTxid().AsUint256()));EXPECT_TRUE(pool.hasTransaction(replacement.GetTxid().AsUint256()));EXPECT_EQ(callbacks,1);
}
TEST_F(MempoolReplacementPolicy, PreflightAndAdmissionUseSurvivingDescendants) {
    Mempool pool(&db,&coins);pool.setRBFEnabled(true);const auto parent=spend(fund(2),1000000,1000);ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());
    Transaction last=parent;uint64_t value=999000;std::vector<uint256> removed;
    for(unsigned i=0;i<25;++i){auto child=spend({last.GetTxid(),0},value,1000);const auto result=pool.submitTransaction(child,"fixture",false);ASSERT_TRUE(result.accepted())<<i<<": "<<result.message;removed.push_back(child.GetTxid().AsUint256());last=std::move(child);value-=1000;}
    const auto unrelated=spend(fund(3),1000000,1000);ASSERT_TRUE(pool.submitTransaction(unrelated,"fixture",false).accepted());
    const auto replacement=spend({parent.GetTxid(),0},999000,50000);const auto before=pool.getTransactionIds();
    const auto preflight=pool.submitTransactionTestOnly(replacement,"fixture");ASSERT_TRUE(preflight.accepted())<<preflight.message;EXPECT_EQ(pool.getTransactionIds(),before);
    const auto result=pool.submitTransaction(replacement,"fixture",false);ASSERT_TRUE(result.accepted())<<result.message;EXPECT_EQ(pool.size(),3U);EXPECT_TRUE(pool.hasTransaction(parent.GetTxid().AsUint256()));EXPECT_TRUE(pool.hasTransaction(unrelated.GetTxid().AsUint256()));EXPECT_TRUE(pool.hasTransaction(replacement.GetTxid().AsUint256()));for(const auto& id:removed)EXPECT_FALSE(pool.hasTransaction(id));
    const auto child=spend({replacement.GetTxid(),0},949000,1000);ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
}
TEST_F(MempoolReplacementPolicy, ExistingRbfFeeRulesRemainRequired) {
    const auto out=fund(4);Mempool pool(&db,&coins);pool.setRBFEnabled(true);const auto first=spend(out,1000000,10000);ASSERT_TRUE(pool.submitTransaction(first,"fixture",false).accepted());
    const auto cheaper=spend(out,1000000,9000);const auto before=pool.getTransactionIds();EXPECT_EQ(pool.submitTransactionTestOnly(cheaper,"fixture").code,TxRejectCode::RBF_REJECTED);EXPECT_EQ(pool.submitTransaction(cheaper,"fixture",false).code,TxRejectCode::RBF_REJECTED);EXPECT_EQ(pool.getTransactionIds(),before);EXPECT_TRUE(pool.isOutputSpentInMempool(out));
}
}
