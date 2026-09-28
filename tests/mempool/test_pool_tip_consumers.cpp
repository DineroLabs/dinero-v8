// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "daemon/prepared_pool_tip.h"
#include <thread>
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include <gtest/gtest.h>
#include "common/ilogger.h"
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
namespace {
using namespace dinero;
class PoolTipConsumers : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("pool_tip_consumers_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    static std::vector<uint256> idsFor(const Mempool& pool) {
        auto ids=pool.getTransactionIds();std::sort(ids.begin(),ids.end(),[](const auto& a,const auto& b){return a.GetHex()<b.GetHex();});return ids;
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
class PublicationLogger final:public ILogger {
public:
    std::string fail_at;
    void setLogLevel(LogLevel) override {} void setLogFile(const std::string&) override {} void shutdown() override {}
    void log(LogLevel,const std::string& text) override {if(!fail_at.empty()&&text.find(fail_at)!=std::string::npos)throw std::runtime_error("fixture publication exception");}
    void debug(const std::string& text) override {log(LogLevel::DEBUG,text);} void info(const std::string& text) override {log(LogLevel::INFO,text);}
    void warning(const std::string& text) override {log(LogLevel::WARNING,text);} void error(const std::string& text) override {log(LogLevel::ERROR,text);}
};
std::shared_ptr<network::BridgeNode> MakeBridge(consensus::ConsensusUTXOSet& coins) {
    auto provider=std::shared_ptr<consensus::IUTXOProvider>(std::shared_ptr<void>{},&coins);
    return std::make_shared<network::BridgeNode>(provider,&coins.GetForest(),nullptr,nullptr,nullptr,&coins);
}
void Cooldown() { std::this_thread::sleep_for(std::chrono::milliseconds(550)); }
TEST_F(PoolTipConsumers, AbandonAndPublishPreserveAllConsumerOwners) {
    Mempool pool(&db,&coins);const auto funding=fund(1);const auto tx=spend(funding,1000000,1000);const auto id=tx.GetTxid().AsUint256();
    coins.MutateForestGuarded([&](consensus::UtreexoForest& forest){forest.add(consensus::HashUTXOForCreationHeight(funding.txid.AsUint256(),0,1000000,script,1,false));});
    ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());ASSERT_TRUE(pool.refreshProof(id,{1},110));
    auto bridge=MakeBridge(coins);ASSERT_TRUE(bridge->GenerateProofsForTransaction(tx));
    auto relay=std::make_shared<TxRelayManager>(nullptr);relay->SetCsnMode(true);size_t sent=0;
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;});relay->RequestProofRefresh({id});ASSERT_EQ(sent,1U);
    Block block;
    {auto prepared=PreparedPoolTip::Connect(pool,bridge,relay,block,111,{2});ASSERT_TRUE(prepared);EXPECT_THROW(prepared->RequestRefresh(),std::logic_error);}
    EXPECT_EQ(pool.getStaleCount(),0U);EXPECT_EQ(pool.getStats().refresh_attempted_total,0U);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);
    Cooldown();relay->RequestProofRefresh({id});EXPECT_EQ(sent,1U);
    Cooldown();auto prepared=PreparedPoolTip::Connect(pool,bridge,relay,block,111,{2});prepared->PublishAfterCommit();
    EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(pool.getStats().refresh_attempted_total,1U);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);relay->CompleteRefresh(id);});
    prepared->RequestRefresh();EXPECT_EQ(sent,2U);EXPECT_THROW(prepared->RequestRefresh(),std::logic_error);
}
TEST_F(PoolTipConsumers, PreparationFailureAndAbsentConsumersRetainExistingPolicy) {
    PublicationLogger logger;Mempool pool(&db,&coins);pool.setLogger(&logger);const auto funding=fund(2);const auto tx=spend(funding,1000000,1000);const auto id=tx.GetTxid().AsUint256();
    coins.MutateForestGuarded([&](consensus::UtreexoForest& forest){forest.add(consensus::HashUTXOForCreationHeight(funding.txid.AsUint256(),0,1000000,script,1,false));});
    ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());ASSERT_TRUE(pool.refreshProof(id,{1},1));
    auto bridge=MakeBridge(coins);ASSERT_TRUE(bridge->GenerateProofsForTransaction(tx));auto relay=std::make_shared<TxRelayManager>(nullptr);relay->SetCsnMode(true);
    size_t sent=0;relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;});relay->RequestProofRefresh({id});
    logger.fail_at="[ProofChurnGuard] Evicted ";EXPECT_THROW(PreparedPoolTip::Connect(pool,bridge,relay,Block{},111,{2}),std::runtime_error);logger.fail_at.clear();
    EXPECT_EQ(pool.size(),1U);EXPECT_EQ(pool.getStaleCount(),0U);EXPECT_EQ(pool.getStats().stale_evicted_total,0U);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);
    Cooldown();relay->RequestProofRefresh({id});EXPECT_EQ(sent,1U);
    // Relay absence preserves the existing no-refresh-policy behavior.
    {auto prepared=PreparedPoolTip::Connect(pool,{}, {},Block{},111,{2});prepared->PublishAfterCommit();prepared->RequestRefresh();}
    EXPECT_EQ(pool.size(),1U);EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(pool.getStats().stale_evicted_total,0U);EXPECT_EQ(pool.getStats().refresh_attempted_total,0U);
}
TEST_F(PoolTipConsumers, DisconnectPublishesBeforeAmbiguousExternalRefresh) {
    Mempool pool(&db,&coins);const auto funding=fund(3);const auto tx=spend(funding,1000000,1000);const auto id=tx.GetTxid().AsUint256();
    coins.MutateForestGuarded([&](consensus::UtreexoForest& forest){forest.add(consensus::HashUTXOForCreationHeight(funding.txid.AsUint256(),0,1000000,script,1,false));});
    ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());ASSERT_TRUE(pool.refreshProof(id,{1},109));
    auto bridge=MakeBridge(coins);ASSERT_TRUE(bridge->GenerateProofsForTransaction(tx));auto relay=std::make_shared<TxRelayManager>(nullptr);relay->SetCsnMode(true);size_t attempts=0;
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++attempts;EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);throw std::runtime_error("fixture ambiguous send");});
    auto prepared=PreparedPoolTip::Disconnect(pool,bridge,relay,110);prepared->PublishAfterCommit();EXPECT_THROW(prepared->RequestRefresh(),std::runtime_error);
    EXPECT_EQ(attempts,1U);EXPECT_EQ(pool.size(),1U);EXPECT_EQ(pool.getStats().refresh_attempted_total,1U);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++attempts;});Cooldown();relay->RequestProofRefresh({id});EXPECT_EQ(attempts,1U);
}
}
