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
class MempoolPackageBounds : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_package_bounds_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    void sign(Transaction& tx,const OutPoint& out,uint64_t value) {
        CanonicalWalletUTXO coin;coin.txid=out.txid.AsUint256();coin.vout=out.vout;coin.value=AmountUna::Una(value);coin.spk=script;
        const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});
        if(bytes.size()!=32)throw std::runtime_error("fixture sighash refused");
        std::array<uint8_t,32> hash{};std::copy(bytes.begin(),bytes.end(),hash.begin());std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,hash,secret,internal))throw std::runtime_error("fixture signature refused");
        tx.vin[0].witness={std::vector<uint8_t>(signature.begin(),signature.end())};
    }
    Transaction split(const OutPoint& out) {
        auto tx=spend(out,1000000,1000);tx.vout[0].value=AmountUna::Una(499500);
        tx.vout.emplace_back(AmountUna::Una(499500),script);sign(tx,out,1000000);return tx;
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
TEST_F(MempoolPackageBounds, NonDirectAncestorBoundPreservesPool) {
    Mempool pool(&db,&coins);const auto parent=split(fund(1));
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());
    Transaction last=parent;uint64_t value=499500;
    for(unsigned i=0;i<24;++i){auto tx=spend({last.GetTxid(),0},value,1000);const auto result=pool.submitTransaction(tx,"fixture",false);ASSERT_TRUE(result.accepted())<<i<<": "<<result.message;last=std::move(tx);value-=1000;}
    const auto branch=spend({parent.GetTxid(),1},499500,1000);ASSERT_TRUE(pool.submitTransaction(branch,"fixture",false).accepted());
    const auto candidate=spend({branch.GetTxid(),0},498500,1000);const auto before=pool.getTransactionIds();int callbacks=0;pool.setTxAcceptedCallback([&](const Transaction&){++callbacks;});
    EXPECT_EQ(pool.submitTransactionTestOnly(candidate,"fixture").code,TxRejectCode::TOO_MANY_DESCENDANTS);
    EXPECT_EQ(pool.submitTransaction(candidate,"fixture",false).code,TxRejectCode::TOO_MANY_DESCENDANTS);
    EXPECT_EQ(pool.getTransactionIds(),before);EXPECT_EQ(callbacks,0);
    EXPECT_FALSE(pool.isOutputSpentInMempool({branch.GetTxid(),0}));
    const auto unrelated=spend(fund(2),1000000,1000);EXPECT_TRUE(pool.submitTransaction(unrelated,"fixture",false).accepted());
}
TEST_F(MempoolPackageBounds, ValidReplacementRestoresAncestorCapacity) {
    Mempool pool(&db,&coins);pool.setRBFEnabled(true);const auto parent=split(fund(3));ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());
    Transaction last=parent;uint64_t value=499500;std::vector<uint256> removed;
    for(unsigned i=0;i<24;++i){auto tx=spend({last.GetTxid(),0},value,1000);const auto result=pool.submitTransaction(tx,"fixture",false);ASSERT_TRUE(result.accepted())<<i<<": "<<result.message;removed.push_back(tx.GetTxid().AsUint256());last=std::move(tx);value-=1000;}
    const auto branch=spend({parent.GetTxid(),1},499500,1000);ASSERT_TRUE(pool.submitTransaction(branch,"fixture",false).accepted());
    const auto replacement=spend({parent.GetTxid(),0},499500,50000);const auto before=pool.getTransactionIds();
    auto result=pool.submitTransactionTestOnly(replacement,"fixture");ASSERT_TRUE(result.accepted())<<result.message;EXPECT_EQ(pool.getTransactionIds(),before);
    result=pool.submitTransaction(replacement,"fixture",false);ASSERT_TRUE(result.accepted())<<result.message;EXPECT_EQ(pool.size(),3U);
    for(const auto& id:removed)EXPECT_FALSE(pool.hasTransaction(id));
    const auto candidate=spend({branch.GetTxid(),0},498500,1000);EXPECT_TRUE(pool.submitTransactionTestOnly(candidate,"fixture").accepted());EXPECT_TRUE(pool.submitTransaction(candidate,"fixture",false).accepted());EXPECT_EQ(pool.size(),4U);
}
}
