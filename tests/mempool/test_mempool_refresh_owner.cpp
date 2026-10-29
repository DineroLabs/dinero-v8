// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
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
class MempoolRefreshOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_refresh_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
TEST_F(MempoolRefreshOwner, AgeEvictionRemovesDescendantsBeforeSelectingSurvivors) {
    Mempool pool(&db,&coins); const auto funding=fund(1); const auto parent=spend(funding,1000000,1000);
    const auto child=spend({parent.GetTxid(),0},999000,1000); const auto independent=spend(fund(2),1000000,2000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted()); ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted()); ASSERT_TRUE(pool.submitTransaction(independent,"fixture",false).accepted());
    ASSERT_TRUE(pool.refreshProof(parent.GetTxid().AsUint256(),{1},1)); ASSERT_TRUE(pool.refreshProof(child.GetTxid().AsUint256(),{1},110)); ASSERT_TRUE(pool.refreshProof(independent.GetTxid().AsUint256(),{1},110));
    pool.onBlockDisconnected(110); const auto candidates=pool.selectStaleForRefresh(110,20,2,1,256);
    EXPECT_EQ(candidates,std::vector<uint256>({independent.GetTxid().AsUint256()})); EXPECT_EQ(pool.size(),1U);
    EXPECT_FALSE(pool.hasTransaction(parent.GetTxid().AsUint256())); EXPECT_FALSE(pool.hasTransaction(child.GetTxid().AsUint256()));
    EXPECT_FALSE(pool.isOutputSpentInMempool(funding)); EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::NotFound);
    const auto stats=pool.getStats(); EXPECT_EQ(stats.stale_evicted_total,2U); EXPECT_EQ(stats.refresh_attempted_total,1U);
    const auto remaining=pool.getMempoolEntry(independent.GetTxid().AsUint256()); ASSERT_TRUE(remaining); EXPECT_EQ(remaining->proof_refresh_attempts,1U);
    EXPECT_TRUE(pool.selectStaleForRefresh(110,20,2,1,256).empty()); EXPECT_EQ(pool.size(),0U); EXPECT_EQ(pool.getStats().stale_evicted_total,3U);
}
TEST_F(MempoolRefreshOwner, OverloadEvictsChildrenWithoutProofMetadata) {
    Mempool pool(&db,&coins); const auto funding=fund(3); const auto parent=spend(funding,1000000,1000);
    const auto child=spend({parent.GetTxid(),0},999000,1000); const auto independent=spend(fund(4),1000000,2000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted()); ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted()); ASSERT_TRUE(pool.submitTransaction(independent,"fixture",false).accepted());
    ASSERT_TRUE(pool.refreshProof(parent.GetTxid().AsUint256(),{1},110)); pool.onBlockDisconnected(110); ASSERT_EQ(pool.getStaleCount(),1U);
    pool.excludeFromBlockTemplates(child.GetTxid().AsUint256(),"child exclusion"); EXPECT_TRUE(pool.selectStaleForRefresh(110,20,2,1,1).empty());
    EXPECT_EQ(pool.size(),1U); EXPECT_TRUE(pool.hasTransaction(independent.GetTxid().AsUint256())); EXPECT_FALSE(pool.hasTransaction(child.GetTxid().AsUint256()));
    EXPECT_FALSE(pool.isExcludedFromBlockTemplates(child.GetTxid().AsUint256())); EXPECT_FALSE(pool.isOutputSpentInMempool(funding));
    EXPECT_EQ(pool.getCoinsView().getCoin({independent.GetTxid(),0}).status(),Status::Ok);
    const auto stats=pool.getStats(); EXPECT_EQ(stats.stale_evicted_total,2U); EXPECT_EQ(stats.refresh_dropped_budget_total,2U); EXPECT_EQ(stats.refresh_attempted_total,0U);
}
TEST_F(MempoolRefreshOwner, SelectionExceptionRestoresEntriesAttemptsAndCounters) {
    PublicationLogger logger; Mempool pool(&db,&coins); pool.setLogger(&logger);
    const auto parent=spend(fund(5),1000000,1000); const auto child=spend({parent.GetTxid(),0},999000,1000); const auto independent=spend(fund(6),1000000,2000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted()); ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted()); ASSERT_TRUE(pool.submitTransaction(independent,"fixture",false).accepted());
    ASSERT_TRUE(pool.refreshProof(parent.GetTxid().AsUint256(),{1},1)); ASSERT_TRUE(pool.refreshProof(child.GetTxid().AsUint256(),{1},110)); ASSERT_TRUE(pool.refreshProof(independent.GetTxid().AsUint256(),{1},110));
    pool.onBlockDisconnected(110); const auto ids=idsFor(pool); const auto before=pool.getStats();
    logger.fail_at="[ProofChurnGuard] Evicted "; EXPECT_THROW(pool.selectStaleForRefresh(110,20,2,1,256),std::runtime_error); logger.fail_at.clear();
    EXPECT_EQ(idsFor(pool),ids); EXPECT_EQ(pool.getStats().total_fees,before.total_fees); EXPECT_EQ(pool.getStats().stale_evicted_total,before.stale_evicted_total); EXPECT_EQ(pool.getStats().refresh_attempted_total,before.refresh_attempted_total);
    for (const auto& id:ids) { const auto entry=pool.getMempoolEntry(id); ASSERT_TRUE(entry); EXPECT_EQ(entry->proof_refresh_attempts,0U); }
    EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
    EXPECT_EQ(pool.selectStaleForRefresh(110,20,2,1,256),std::vector<uint256>({independent.GetTxid().AsUint256()})); EXPECT_EQ(pool.size(),1U);
}
}
