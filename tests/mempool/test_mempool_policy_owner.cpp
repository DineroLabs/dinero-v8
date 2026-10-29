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
#include <thread>
#include <atomic>
#include <filesystem>
namespace {
using namespace dinero;
class MempoolPolicyOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_policy_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
struct JoinPolicyWorker {
    std::thread& worker;std::atomic<bool>& stop;
    ~JoinPolicyWorker() {stop.store(true);if(worker.joinable())worker.join();}
};
TEST_F(MempoolPolicyOwner, ConfigurationSnapshotsDoNotEscapeMutableState) {
    Mempool pool(&db,&coins);mining::CTSelectionConfig initial;initial.ct_min_fee_rate=7;initial.max_ct_per_block=31;pool.SetCTConfig(initial);
    auto captured=pool.GetCTConfig();captured.ct_min_fee_rate=99;EXPECT_EQ(pool.GetCTConfig().ct_min_fee_rate,7U);
    pool.SetCTMinFeeRate(8);pool.SetCTWeightMultiplier(2.5);pool.SetCTMaxPerBlock(32);pool.SetCTProofWeightFactor(6);
    const auto current=pool.GetCTConfig();EXPECT_EQ(current.ct_min_fee_rate,8U);EXPECT_EQ(current.ct_weight_multiplier,2.5);EXPECT_EQ(current.max_ct_per_block,32U);EXPECT_EQ(current.ct_proof_weight_factor,6U);
    EXPECT_EQ(captured.ct_min_fee_rate,99U);EXPECT_EQ(captured.max_ct_per_block,31U);EXPECT_EQ(current.max_ct_proof_data,initial.max_ct_proof_data);EXPECT_EQ(current.batch_size_threshold,initial.batch_size_threshold);
}
TEST_F(MempoolPolicyOwner, IndependentFieldUpdatesAndReadsPreserveConfiguration) {
    Mempool pool(&db,&coins);std::atomic<bool> start{false},stop{false};
    std::thread fee([&]{while(!start.load()&&!stop.load())std::this_thread::yield();if(stop.load())return;for(uint64_t i=1;i<=200;++i){pool.SetCTMinFeeRate(i);const auto snapshot=pool.GetCTConfig();EXPECT_GE(snapshot.ct_min_fee_rate,1U);}});
    JoinPolicyWorker fee_owner{fee,stop};
    std::thread count([&]{while(!start.load()&&!stop.load())std::this_thread::yield();if(stop.load())return;for(size_t i=1;i<=200;++i){pool.SetCTMaxPerBlock(i);const auto snapshot=pool.GetCTConfig();EXPECT_GE(snapshot.max_ct_per_block,1U);}});
    JoinPolicyWorker count_owner{count,stop};
    start.store(true);fee.join();count.join();const auto result=pool.GetCTConfig();EXPECT_EQ(result.ct_min_fee_rate,200U);EXPECT_EQ(result.max_ct_per_block,200U);EXPECT_EQ(result.ct_weight_multiplier,1.5);
}
TEST_F(MempoolPolicyOwner, AdmissionAndOwnedSettingsCompleteTogether) {
    Mempool pool(&db,&coins);std::atomic<bool> done{false};std::atomic<unsigned> updates{0};std::thread writer([&]{while(!done.load()){pool.setMaxSize(1000000);pool.setMaxAge(std::chrono::hours(24));pool.setMinFeeRate(1.0);pool.SetCTProofWeightFactor(4);EXPECT_EQ(pool.getMinFeeRate(),1.0);++updates;}});
    JoinPolicyWorker writer_owner{writer,done};while(updates.load()==0)std::this_thread::yield();
    for(uint8_t i=10;i<26;++i){const auto tx=spend(fund(i),1000000,2000);const auto result=pool.submitTransaction(tx,"fixture",false);EXPECT_TRUE(result.accepted())<<result.message;}
    done.store(true);writer.join();EXPECT_EQ(pool.size(),16U);EXPECT_EQ(pool.GetCTConfig().ct_proof_weight_factor,4U);
}
}
