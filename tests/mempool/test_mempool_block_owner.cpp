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
class MempoolBlockOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_block_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
TEST_F(MempoolBlockOwner, ConnectedConflictFailureRestoresPoolAndProofState) {
    PublicationLogger logger; Mempool pool(&db,&coins); pool.setLogger(&logger);
    const auto funding=fund(1); const auto parent=spend(funding,1000000,1000);
    const auto child=spend({parent.GetTxid(),0},999000,1000); const auto survivor=spend(fund(2),1000000,2000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());
    ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
    ASSERT_TRUE(pool.submitTransaction(survivor,"fixture",false).accepted());
    const auto survivor_id=survivor.GetTxid().AsUint256();
    ASSERT_TRUE(pool.refreshProof(survivor_id,{1,2,3},110));
    ASSERT_TRUE(pool.setCachedUtxoTxPayload(survivor_id,{7,8,9}));
    pool.excludeFromBlockTemplates(child.GetTxid().AsUint256(),"retained exclusion");
    const auto ids=idsFor(pool); const auto fees=pool.getTotalFees();
    const auto prior_height=pool.getStats().last_connected_height;
    ConnectedBlockEffects effects; effects.spent_transparent_inputs.push_back(funding);
    logger.fail_at="Block 111 connected:";
    EXPECT_THROW(pool.onBlockConnected(effects,111,{4,5,6}),std::runtime_error); logger.fail_at.clear();
    EXPECT_EQ(idsFor(pool),ids); EXPECT_EQ(pool.getTotalFees(),fees);
    EXPECT_EQ(pool.getStats().last_connected_height,prior_height); EXPECT_EQ(pool.getStaleCount(),0U);
    EXPECT_EQ(pool.getCachedUtxoTxPayload(survivor_id),std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{7,8,9}));
    EXPECT_TRUE(pool.isExcludedFromBlockTemplates(child.GetTxid().AsUint256()));
    EXPECT_TRUE(pool.isOutputSpentInMempool(funding));
    EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
    EXPECT_EQ(pool.onBlockConnected(effects,111,{4,5,6}),2U); EXPECT_EQ(pool.size(),1U);
    EXPECT_EQ(pool.getStats().last_connected_height,111U); EXPECT_EQ(pool.getStaleCount(),1U);
    EXPECT_FALSE(pool.getCachedUtxoTxPayload(survivor_id));
    EXPECT_FALSE(pool.isExcludedFromBlockTemplates(child.GetTxid().AsUint256()));
    EXPECT_EQ(pool.onBlockConnected(effects,111,{4,5,6}),0U); EXPECT_EQ(pool.size(),1U);
}
TEST_F(MempoolBlockOwner, ConfirmedParentFailurePreservesChildDependencyAndRetry) {
    PublicationLogger logger; Mempool pool(&db,&coins); pool.setLogger(&logger);
    const auto funding=fund(3); const auto parent=spend(funding,1000000,1000);
    const auto child=spend({parent.GetTxid(),0},999000,1000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());
    ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
    const auto before=pool.getMempoolEntry(child.GetTxid().AsUint256()); ASSERT_TRUE(before);
    ASSERT_TRUE(coins.SpendCoin(funding));
    ASSERT_TRUE(coins.AddCoin({parent.GetTxid(),0},{parent.vout[0].value,script,111,false}));
    ConnectedBlockEffects effects; effects.confirmed_txids.push_back(parent.GetTxid().AsUint256()); effects.spent_transparent_inputs.push_back(funding);
    logger.fail_at="Block 111 connected:"; EXPECT_THROW(pool.onBlockConnected(effects,111),std::runtime_error); logger.fail_at.clear();
    EXPECT_EQ(pool.size(),2U); const auto after=pool.getMempoolEntry(child.GetTxid().AsUint256()); ASSERT_TRUE(after);
    EXPECT_EQ(after->depends,before->depends); EXPECT_EQ(after->ancestor_fee,before->ancestor_fee); EXPECT_EQ(after->ancestor_size,before->ancestor_size);
    EXPECT_EQ(pool.onBlockConnected(effects,111),0U); EXPECT_EQ(pool.size(),1U);
    const auto retained=pool.getMempoolEntry(child.GetTxid().AsUint256()); ASSERT_TRUE(retained); EXPECT_TRUE(retained->depends.empty());
    EXPECT_TRUE(pool.isOutputSpentInMempool({parent.GetTxid(),0}));
    EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
    const auto grandchild=spend({child.GetTxid(),0},998000,1000); EXPECT_TRUE(pool.submitTransaction(grandchild,"fixture",false).accepted());
}
TEST_F(MempoolBlockOwner, DisconnectedFailureRestoresCacheBeforeRetry) {
    PublicationLogger logger; Mempool pool(&db,&coins); pool.setLogger(&logger);
    const auto tx=spend(fund(4),1000000,1000); ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    const auto id=tx.GetTxid().AsUint256(); ASSERT_TRUE(pool.refreshProof(id,{1,2,3},110)); ASSERT_TRUE(pool.setCachedUtxoTxPayload(id,{4,5,6}));
    logger.fail_at="Block 110 disconnected:"; EXPECT_THROW(pool.onBlockDisconnected(110),std::runtime_error); logger.fail_at.clear();
    EXPECT_EQ(pool.size(),1U); EXPECT_EQ(pool.getStaleCount(),0U);
    EXPECT_EQ(pool.getCachedUtxoTxPayload(id),std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{4,5,6}));
    const auto entry=pool.getMempoolEntry(id); ASSERT_TRUE(entry); EXPECT_EQ(entry->validated_at_height,110U); EXPECT_EQ(entry->validated_at_root,std::vector<uint8_t>({1,2,3}));
    pool.onBlockDisconnected(110); EXPECT_EQ(pool.getStaleCount(),1U); EXPECT_FALSE(pool.getCachedUtxoTxPayload(id));
    pool.onBlockDisconnected(110); EXPECT_EQ(pool.size(),1U); EXPECT_EQ(pool.getStaleCount(),1U);
}
TEST_F(MempoolBlockOwner, PreparedUpdatesAbandonOrPublishExactlyOnce) {
    PublicationLogger logger; Mempool pool(&db,&coins); pool.setLogger(&logger);
    const auto funding=fund(5); const auto tx=spend(funding,1000000,1000);
    ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    const auto id=tx.GetTxid().AsUint256(); ASSERT_TRUE(pool.refreshProof(id,{1,2,3},110));
    ASSERT_TRUE(pool.setCachedUtxoTxPayload(id,{4,5,6}));
    ConnectedBlockEffects effects; effects.spent_transparent_inputs.push_back(funding);
    { auto prepared=pool.prepareBlockConnected(effects,111,{7,8,9}); ASSERT_TRUE(prepared); EXPECT_EQ(prepared->EvictedCount(),1U); }
    EXPECT_EQ(pool.size(),1U); EXPECT_EQ(pool.getStats().last_connected_height,0U);
    EXPECT_EQ(pool.getStaleCount(),0U); EXPECT_TRUE(pool.getCachedUtxoTxPayload(id));
    { auto prepared=pool.prepareBlockDisconnected(110); ASSERT_TRUE(prepared); }
    EXPECT_EQ(pool.getStaleCount(),0U); EXPECT_TRUE(pool.getCachedUtxoTxPayload(id));
    { auto prepared=pool.prepareBlockDisconnected(110); ASSERT_TRUE(prepared); prepared->PublishAfterCommit(); }
    EXPECT_EQ(pool.getStaleCount(),1U); EXPECT_FALSE(pool.getCachedUtxoTxPayload(id));
    { auto prepared=pool.prepareBlockConnected(effects,111,{7,8,9}); ASSERT_TRUE(prepared); EXPECT_EQ(prepared->EvictedCount(),1U);
      logger.fail_at="Block 111 connected:"; prepared->PublishAfterCommit(); logger.fail_at.clear(); }
    EXPECT_EQ(pool.size(),0U); EXPECT_EQ(pool.getStats().last_connected_height,111U);
    EXPECT_FALSE(pool.isOutputSpentInMempool(funding));
}
}
