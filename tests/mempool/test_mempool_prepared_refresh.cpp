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
class MempoolPreparedRefresh : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_prepared_refresh_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
TEST_F(MempoolPreparedRefresh, ConnectAbandonsOrPublishesBlockAndRefreshTogether) {
    Mempool pool(&db,&coins);const auto parent=spend(fund(1),1000000,1000);
    const auto child=spend({parent.GetTxid(),0},999000,1000);const auto independent=spend(fund(2),1000000,2000);
    for(const auto& tx:{parent,child,independent})ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    ASSERT_TRUE(pool.refreshProof(parent.GetTxid().AsUint256(),{1},1));ASSERT_TRUE(pool.refreshProof(child.GetTxid().AsUint256(),{1},110));ASSERT_TRUE(pool.refreshProof(independent.GetTxid().AsUint256(),{1},110));
    const auto ids=idsFor(pool);const auto before=pool.getStats();Mempool::ProofRefreshPolicy policy;
    {auto prepared=pool.prepareBlockConnected({},111,{2},policy);ASSERT_TRUE(prepared);EXPECT_EQ(prepared->RefreshCandidates(),std::vector<uint256>({independent.GetTxid().AsUint256()}));}
    EXPECT_EQ(idsFor(pool),ids);EXPECT_EQ(pool.getStaleCount(),0U);EXPECT_EQ(pool.getStats().last_connected_height,before.last_connected_height);EXPECT_EQ(pool.getStats().refresh_attempted_total,before.refresh_attempted_total);
    {auto prepared=pool.prepareBlockConnected({},111,{2},policy);ASSERT_TRUE(prepared);prepared->PublishAfterCommit();}
    EXPECT_EQ(pool.size(),1U);EXPECT_EQ(pool.getStats().stale_evicted_total,2U);EXPECT_EQ(pool.getStats().refresh_attempted_total,1U);EXPECT_EQ(pool.getStats().last_connected_height,111U);
    const auto entry=pool.getMempoolEntry(independent.GetTxid().AsUint256());ASSERT_TRUE(entry);EXPECT_EQ(entry->proof_refresh_attempts,1U);EXPECT_TRUE(entry->is_proof_stale);
}
TEST_F(MempoolPreparedRefresh, RefreshPreparationFailureRestoresCompleteBlockTransition) {
    PublicationLogger logger;Mempool pool(&db,&coins);pool.setLogger(&logger);const auto conflict_funding=fund(3);
    const auto conflict=spend(conflict_funding,1000000,1000);const auto old=spend(fund(4),1000000,1000);const auto survivor=spend(fund(5),1000000,2000);
    for(const auto& tx:{conflict,old,survivor})ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    ASSERT_TRUE(pool.refreshProof(old.GetTxid().AsUint256(),{1},1));ASSERT_TRUE(pool.refreshProof(survivor.GetTxid().AsUint256(),{1},110));ASSERT_TRUE(pool.setCachedUtxoTxPayload(survivor.GetTxid().AsUint256(),{8}));
    const auto ids=idsFor(pool);const auto stats=pool.getStats();ConnectedBlockEffects effects;effects.spent_transparent_inputs.push_back(conflict_funding);Mempool::ProofRefreshPolicy policy;
    logger.fail_at="[ProofChurnGuard] Evicted ";EXPECT_THROW(pool.prepareBlockConnected(effects,111,{2},policy),std::runtime_error);logger.fail_at.clear();
    EXPECT_EQ(idsFor(pool),ids);EXPECT_EQ(pool.getStats().total_fees,stats.total_fees);EXPECT_EQ(pool.getStats().last_connected_height,stats.last_connected_height);EXPECT_EQ(pool.getStats().stale_evicted_total,stats.stale_evicted_total);EXPECT_EQ(pool.getStats().refresh_attempted_total,stats.refresh_attempted_total);
    EXPECT_EQ(pool.getStaleCount(),0U);EXPECT_TRUE(pool.getCachedUtxoTxPayload(survivor.GetTxid().AsUint256()));EXPECT_TRUE(pool.isOutputSpentInMempool(conflict_funding));
    {auto prepared=pool.prepareBlockConnected(effects,111,{2},policy);ASSERT_TRUE(prepared);EXPECT_EQ(prepared->EvictedCount(),1U);EXPECT_EQ(prepared->RefreshCandidates(),std::vector<uint256>({survivor.GetTxid().AsUint256()}));prepared->PublishAfterCommit();}
    EXPECT_EQ(pool.size(),1U);EXPECT_EQ(pool.getStats().stale_evicted_total,1U);
}
TEST_F(MempoolPreparedRefresh, DisconnectUsesParentHeightAndPreservesAbandonedAttempts) {
    Mempool pool(&db,&coins);const auto tx=spend(fund(6),1000000,1000);ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());const auto id=tx.GetTxid().AsUint256();
    ASSERT_TRUE(pool.refreshProof(id,{1},109));Mempool::ProofRefreshPolicy policy;policy.max_age_blocks=0;
    {auto prepared=pool.prepareBlockDisconnected(110,policy);ASSERT_TRUE(prepared);EXPECT_EQ(prepared->RefreshCandidates(),std::vector<uint256>({id}));}
    EXPECT_EQ(pool.getStaleCount(),0U);EXPECT_EQ(pool.getStats().refresh_attempted_total,0U);
    {auto prepared=pool.prepareBlockDisconnected(110,policy);ASSERT_TRUE(prepared);EXPECT_EQ(prepared->RefreshCandidates(),std::vector<uint256>({id}));prepared->PublishAfterCommit();}
    EXPECT_EQ(pool.size(),1U);EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(pool.getStats().stale_evicted_total,0U);EXPECT_EQ(pool.getStats().refresh_attempted_total,1U);
}
TEST_F(MempoolPreparedRefresh, ConfirmationPreparedBeforeBaseCommitRetainsChildOverlay) {
    Mempool pool(&db,&coins);const auto funding=fund(7);const auto parent=spend(funding,1000000,1000);const auto child=spend({parent.GetTxid(),0},999000,1000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
    ASSERT_TRUE(pool.refreshProof(child.GetTxid().AsUint256(),{1},110));
    ConnectedBlockEffects effects;effects.confirmed_txids.push_back(parent.GetTxid().AsUint256());effects.spent_transparent_inputs.push_back(funding);Mempool::ProofRefreshPolicy policy;
    auto prepared=pool.prepareBlockConnected(effects,111,{2},policy);ASSERT_TRUE(prepared);EXPECT_EQ(prepared->EvictedCount(),0U);EXPECT_EQ(prepared->RefreshCandidates(),std::vector<uint256>({child.GetTxid().AsUint256()}));
    // The real isolated base is advanced only AFTER preparation, matching the
    // intended canonical ordering. This fixture is not a full chain commit.
    ASSERT_TRUE(coins.SpendCoin(funding));ASSERT_TRUE(coins.AddCoin({parent.GetTxid(),0},{parent.vout[0].value,script,111,false}));
    prepared->PublishAfterCommit();prepared.reset();
    EXPECT_EQ(pool.size(),1U);const auto retained=pool.getMempoolEntry(child.GetTxid().AsUint256());ASSERT_TRUE(retained);EXPECT_TRUE(retained->depends.empty());EXPECT_EQ(retained->proof_refresh_attempts,1U);
    EXPECT_TRUE(pool.isOutputSpentInMempool({parent.GetTxid(),0}));EXPECT_EQ(pool.getCoinsView().getCoin({parent.GetTxid(),0}).status(),Status::NotFound);EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
    const auto grandchild=spend({child.GetTxid(),0},998000,1000);EXPECT_TRUE(pool.submitTransaction(grandchild,"fixture",false).accepted());
}
}
