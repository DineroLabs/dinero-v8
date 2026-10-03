// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "daemon/prepared_pool_tip.h"
#include "daemon/services/mempool_service.h"
#include <future>
#include <atomic>
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
namespace dinero {
class MempoolServiceOwnerTestPeer {
public:
    static std::shared_ptr<MempoolService> Published(ChainDB& db,consensus::ConsensusUTXOSet& coins,ILogger* logger) {
        auto service=std::make_shared<MempoolService>();
        service->mempool_=std::make_unique<Mempool>(&db,&coins);
        service->mempool_->setLogger(logger);service->logger_interface_=logger;
        service->accepting_=true;service->started_=true;return service;
    }
    static bool WaitForStopping(const MempoolService& service) {
        std::unique_lock<std::mutex> lock(service.operation_mutex_);
        return service.operation_changed_.wait_for(lock,std::chrono::seconds(5),[&]{return service.stopping_;});
    }
};
}
namespace {
using namespace dinero;
class MempoolServiceOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_service_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    std::function<void()> on_log;
    void setLogLevel(LogLevel) override {} void setLogFile(const std::string&) override {} void shutdown() override {}
    void log(LogLevel,const std::string& text) override {if(on_log){auto fn=std::move(on_log);fn();}if(!fail_at.empty()&&text.find(fail_at)!=std::string::npos)throw std::runtime_error("fixture publication exception");}
    void debug(const std::string& text) override {log(LogLevel::DEBUG,text);} void info(const std::string& text) override {log(LogLevel::INFO,text);}
    void warning(const std::string& text) override {log(LogLevel::WARNING,text);} void error(const std::string& text) override {log(LogLevel::ERROR,text);}
};

// All concurrent scenarios execute the patched lifetime guard, never an unsafe
// original or a synchronization-removal control. No production datadir is used.
TEST_F(MempoolServiceOwner, PreparedOperationRetainsOwnerAndDrainsBeforeStop) {
    PublicationLogger logger;auto service=MempoolServiceOwnerTestPeer::Published(db,coins,&logger);
    const auto tx=spend(fund(1),1000000,1000);ASSERT_TRUE(service->Submit(tx,TxOrigin::INTERNAL).accepted());
    std::promise<void> ready,release;auto released=release.get_future().share();
    std::atomic<bool> stopped{false},published{false};
    auto job=std::async(std::launch::async,[service,&ready,released,&published] {
        auto use=MempoolService::AcquirePoolUse(service);
        auto prepared=PreparedPoolTip::Connect(use->Pool(),{}, {},Block{},111,{2});
        ready.set_value();released.wait();prepared->PublishAfterCommit();prepared->RequestRefresh();
        EXPECT_EQ(use->Pool().size(),1U);EXPECT_THROW(service->Stop(),std::logic_error);published=true;
    });
    ready.get_future().wait();
    auto stopping=std::async(std::launch::async,[service,&stopped]{service->Stop();stopped=true;});
    EXPECT_TRUE(MempoolServiceOwnerTestPeer::WaitForStopping(*service));EXPECT_FALSE(stopped.load());
    EXPECT_THROW(MempoolService::AcquirePoolUse(service),std::runtime_error);
    EXPECT_FALSE(service->Test(tx,TxOrigin::INTERNAL));
    release.set_value();job.get();stopping.get();EXPECT_TRUE(published.load());EXPECT_TRUE(stopped.load());
    EXPECT_FALSE(service->isInitialized());EXPECT_FALSE(service->IsHealthy());EXPECT_THROW(service->size(),std::runtime_error);
    EXPECT_NO_THROW(service->Stop());
    // A retained long-operation owner is not just a borrowed pool pointer.
    auto second=MempoolServiceOwnerTestPeer::Published(db,coins,&logger);std::weak_ptr<MempoolService> weak=second;
    auto use=MempoolService::AcquirePoolUse(second);second.reset();EXPECT_FALSE(weak.expired());EXPECT_EQ(use->Pool().size(),0U);
    use.reset();EXPECT_TRUE(weak.expired());
}
TEST_F(MempoolServiceOwner, SubmissionCallbackFinishesNestedReadsDuringDrain) {
    PublicationLogger logger;auto service=MempoolServiceOwnerTestPeer::Published(db,coins,&logger);
    const auto tx=spend(fund(2),1000000,1000);const auto id=tx.GetTxid().AsUint256();
    std::promise<void> callback_entered,release;auto released=release.get_future().share();std::atomic<bool> nested{false},stopped{false};
    service->mempool().setTxBroadcastCallback([&](const uint256& observed){
        callback_entered.set_value();released.wait();EXPECT_EQ(observed,id);
        EXPECT_TRUE(service->HasTransaction(id));EXPECT_THROW(service->Stop(),std::logic_error);nested=true;
    });
    auto submission=std::async(std::launch::async,[&]{return service->Submit(tx,TxOrigin::WALLET);});
    callback_entered.get_future().wait();
    auto stopping=std::async(std::launch::async,[&]{service->Stop();stopped=true;});
    EXPECT_TRUE(MempoolServiceOwnerTestPeer::WaitForStopping(*service));EXPECT_FALSE(stopped.load());
    EXPECT_THROW(service->HasTransaction(id),std::runtime_error);release.set_value();
    EXPECT_TRUE(submission.get().accepted());stopping.get();EXPECT_TRUE(nested.load());EXPECT_TRUE(stopped.load());
}
TEST_F(MempoolServiceOwner, ShutdownLoggingCannotStrandClosedService) {
    PublicationLogger logger;auto service=MempoolServiceOwnerTestPeer::Published(db,coins,&logger);
    ASSERT_TRUE(service->Submit(spend(fund(3),1000000,1000),TxOrigin::INTERNAL).accepted());
    logger.fail_at="[MempoolService] Shutting down";EXPECT_NO_THROW(service->Stop());logger.fail_at.clear();
    EXPECT_FALSE(service->isInitialized());EXPECT_FALSE(service->IsHealthy());EXPECT_FALSE(service->Start());
    EXPECT_THROW(service->Submit(Transaction{},TxOrigin::INTERNAL),std::runtime_error);
    EXPECT_THROW(service->GetTransaction(uint256{}),std::runtime_error);
    EXPECT_THROW(service->SelectTransactionsForBlock(),std::runtime_error);
    EXPECT_FALSE(service->Test(Transaction{},TxOrigin::INTERNAL));
    EXPECT_EQ(service->GetMetrics(),R"({"status":"not_initialized"})");EXPECT_NO_THROW(service->Stop());
    auto reentrant=MempoolServiceOwnerTestPeer::Published(db,coins,&logger);
    logger.on_log=[&]{EXPECT_THROW(reentrant->Stop(),std::logic_error);};
    EXPECT_NO_THROW(reentrant->Stop());EXPECT_FALSE(reentrant->isInitialized());
}
}
