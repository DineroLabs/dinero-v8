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
class MempoolAdmissionOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_admission_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
TEST_F(MempoolAdmissionOwner, InsertionExceptionRestoresEntriesIndexesAndOverlay) {
    PublicationLogger logger;Mempool pool(&db,&coins);pool.setLogger(&logger);const auto existing=spend(fund(1),1000000,1000);ASSERT_TRUE(pool.submitTransaction(existing,"fixture",false).accepted());
    const auto input=fund(2);const auto candidate=spend(input,1000000,2000);const auto ids=idsFor(pool);const auto size=pool.getTotalSize();const auto fees=pool.getTotalFees();int calls=0;pool.setTxAcceptedCallback([&](const Transaction&){++calls;});
    logger.fail_at="Added transaction to mempool:";EXPECT_THROW(pool.submitTransaction(candidate,"fixture",false),std::runtime_error);logger.fail_at.clear();
    EXPECT_EQ(idsFor(pool),ids);EXPECT_EQ(pool.getTotalSize(),size);EXPECT_EQ(pool.getTotalFees(),fees);EXPECT_EQ(calls,0);
    EXPECT_FALSE(pool.isOutputSpentInMempool(input));EXPECT_EQ(pool.getCoinsView().getCoin(input).status(),Status::Ok);EXPECT_EQ(pool.getCoinsView().getCoin({candidate.GetTxid(),0}).status(),Status::NotFound);
    EXPECT_TRUE(pool.submitTransaction(candidate,"fixture",false).accepted());EXPECT_EQ(calls,1);EXPECT_EQ(pool.getTransactionsByFeeRate().size(),2U);
}
TEST_F(MempoolAdmissionOwner, ReplacementExceptionRestoresDescendantsAndExclusions) {
    PublicationLogger logger;Mempool pool(&db,&coins);pool.setLogger(&logger);pool.setRBFEnabled(true);const auto input=fund(3);const auto parent=spend(input,1000000,1000);const auto child=spend({parent.GetTxid(),0},999000,1000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());ASSERT_TRUE(pool.submitTransaction(child,"fixture",false).accepted());
    pool.excludeFromBlockTemplates(child.GetTxid().AsUint256(),"fixture exclusion");const auto ids=idsFor(pool);const auto prior=pool.getMempoolEntry(child.GetTxid().AsUint256());ASSERT_TRUE(prior.has_value());const auto replacement=spend(input,1000000,50000);
    logger.fail_at="Added transaction to mempool:";EXPECT_THROW(pool.submitTransaction(replacement,"fixture",false),std::runtime_error);logger.fail_at.clear();EXPECT_EQ(idsFor(pool),ids);
    const auto after=pool.getMempoolEntry(child.GetTxid().AsUint256());ASSERT_TRUE(after.has_value());EXPECT_EQ(after->depends,prior->depends);EXPECT_EQ(after->ancestor_fee,prior->ancestor_fee);EXPECT_EQ(after->ancestor_size,prior->ancestor_size);EXPECT_TRUE(pool.isExcludedFromBlockTemplates(child.GetTxid().AsUint256()));
    EXPECT_TRUE(pool.isOutputSpentInMempool(input));EXPECT_TRUE(pool.isOutputSpentInMempool({parent.GetTxid(),0}));EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
    EXPECT_TRUE(pool.submitTransaction(replacement,"fixture",false).accepted());EXPECT_EQ(pool.size(),1U);EXPECT_FALSE(pool.isExcludedFromBlockTemplates(child.GetTxid().AsUint256()));
}
TEST_F(MempoolAdmissionOwner, MaintenanceExceptionRollsBackBeforeObservers) {
    PublicationLogger logger;Mempool pool(&db,&coins);pool.setLogger(&logger);const auto input=fund(4);const auto candidate=spend(input,1000000,1000);pool.setMaxSize(1);int calls=0;pool.setTxAcceptedCallback([&](const Transaction&){++calls;});
    logger.fail_at="Evicting low-fee transaction:";EXPECT_THROW(pool.submitTransaction(candidate,"fixture",false),std::runtime_error);logger.fail_at.clear();EXPECT_EQ(pool.size(),0U);EXPECT_EQ(calls,0);EXPECT_FALSE(pool.isOutputSpentInMempool(input));EXPECT_EQ(pool.getCoinsView().createdCount(),0U);
    pool.setMaxSize(1000000);EXPECT_TRUE(pool.submitTransaction(candidate,"fixture",false).accepted());EXPECT_EQ(calls,1);
}
}
