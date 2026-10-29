// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "daemon/block_relay_manager.h"
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
class BlockRelayPoolOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("block_relay_pool_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
MempoolAccessFactory factoryFor(const std::shared_ptr<MempoolService>& service) {
    std::weak_ptr<MempoolService> weak = service;
    return [weak]() -> std::unique_ptr<MempoolAccess> {
        return MempoolService::AcquirePoolUse(weak.lock());
    };
}
Block blockWith(const Transaction& tx, uint32_t nonce = 1) {
    Block block{}; block.header.version = 1; block.header.nonce = nonce;
    Transaction coinbase; coinbase.version = 2;
    coinbase.vout.emplace_back(AmountUna::Una(1),std::vector<uint8_t>{0x51});
    block.vtx = {coinbase,tx}; return block;
}

// Only safe, patched ownership paths execute. These are component reconstruction
// tests with a validation observer, not consensus admission or a network peer.
TEST_F(BlockRelayPoolOwner, ReconstructionRetainsServiceThroughValidationAndStop) {
    auto service=MempoolServiceOwnerTestPeer::Published(db,coins,nullptr);
    const auto tx=spend(fund(1),1000000,1000);ASSERT_TRUE(service->Submit(tx,TxOrigin::INTERNAL).accepted());
    const auto block=blockWith(tx);const auto compact=CompactBlockCodec::CreateCompactBlock(block);
    ASSERT_EQ(compact.short_txids.size(),1U);
    BlockRelayManager relay(nullptr);relay.SetMempoolAccessFactory(factoryFor(service));
    std::promise<void> entered,release;auto released=release.get_future().share();std::atomic<bool> stopped{false};
    relay.SetValidateBlockCallback([&](const Block& reconstructed,const std::string&) {
        EXPECT_EQ(reconstructed.vtx.size(),2U);
        if(reconstructed.vtx.size()==2)EXPECT_EQ(reconstructed.vtx[1].GetTxid(),tx.GetTxid());
        entered.set_value();released.wait();
        EXPECT_TRUE(service->HasTransaction(tx.GetTxid().AsUint256()));
        EXPECT_THROW(service->Stop(),std::logic_error);
        return BlockRelayManager::BlockValidationOutcome::Accepted;
    });
    auto job=std::async(std::launch::async,[&]{relay.HandleCompactBlock("fixture",compact);});
    auto ready=entered.get_future();EXPECT_EQ(ready.wait_for(std::chrono::seconds(5)),std::future_status::ready);
    auto stopping=std::async(std::launch::async,[&]{service->Stop();stopped=true;});
    EXPECT_TRUE(MempoolServiceOwnerTestPeer::WaitForStopping(*service));EXPECT_FALSE(stopped.load());
    EXPECT_THROW(MempoolService::AcquirePoolUse(service),std::runtime_error);
    release.set_value();job.get();stopping.get();EXPECT_TRUE(stopped.load());
    EXPECT_TRUE(relay.IsBlockSeen(block.header.GetHash()));EXPECT_EQ(relay.GetStats().blocks_validated,1U);
}

TEST_F(BlockRelayPoolOwner, FactoryReplacementRetainsCapturedOwner) {
    auto first=MempoolServiceOwnerTestPeer::Published(db,coins,nullptr);
    auto second=MempoolServiceOwnerTestPeer::Published(db,coins,nullptr);
    const auto tx=spend(fund(2),1000000,1000);ASSERT_TRUE(first->Submit(tx,TxOrigin::INTERNAL).accepted());
    BlockRelayManager relay(nullptr);std::weak_ptr<MempoolService> weak=first;std::atomic<int> replacements{0};
    auto next=factoryFor(second);
    relay.SetMempoolAccessFactory([&,weak,next]() -> std::unique_ptr<MempoolAccess> {
        auto owner=MempoolService::AcquirePoolUse(weak.lock());
        // The factory executes outside the relay configuration mutex.
        relay.SetMempoolAccessFactory([&,next]{++replacements;return next();});
        return owner;
    });
    std::promise<void> entered,release;auto released=release.get_future().share();
    relay.SetValidateBlockCallback([&](const Block& block,const std::string&) {
        EXPECT_EQ(block.vtx.size(),2U);entered.set_value();released.wait();
        return BlockRelayManager::BlockValidationOutcome::Accepted;
    });
    auto compact=CompactBlockCodec::CreateCompactBlock(blockWith(tx,2));
    auto job=std::async(std::launch::async,[&]{relay.HandleCompactBlock("fixture",compact);});
    auto ready=entered.get_future();EXPECT_EQ(ready.wait_for(std::chrono::seconds(5)),std::future_status::ready);
    first.reset();EXPECT_FALSE(weak.expired());release.set_value();job.get();EXPECT_TRUE(weak.expired());
    int requests=0;relay.SetSendMessageCallback([&](const std::string&,const std::string& command,const std::vector<uint8_t>&){EXPECT_EQ(command,"getblocktxn");++requests;});
    relay.HandleCompactBlock("fixture",CompactBlockCodec::CreateCompactBlock(blockWith(tx,3)));
    EXPECT_EQ(replacements.load(),1);EXPECT_EQ(requests,1);EXPECT_EQ(relay.GetStats().blocks_validated,1U);
    // Configuration replacement must also destroy arbitrary captured owners
    // outside its mutex. The destructor safely reenters configuration.
    bool destroyed=false;
    auto probe=std::shared_ptr<int>(new int(1),[&](int* p){delete p;destroyed=true;relay.SetMempoolAccessFactory(next);});
    relay.SetMempoolAccessFactory([probe,next]{return next();});probe.reset();
    relay.SetMempoolAccessFactory(next);EXPECT_TRUE(destroyed);
}

TEST_F(BlockRelayPoolOwner, UnavailableOwnerHasNoRelayEffectsAndRetryWorks) {
    auto service=MempoolServiceOwnerTestPeer::Published(db,coins,nullptr);
    const auto tx=spend(fund(3),1000000,1000);ASSERT_TRUE(service->Submit(tx,TxOrigin::INTERNAL).accepted());
    const auto block=blockWith(tx,4);const auto compact=CompactBlockCodec::CreateCompactBlock(block);
    BlockRelayManager relay(nullptr);int sent=0,validated=0;
    relay.SetSendMessageCallback([&](const std::string&,const std::string&,const std::vector<uint8_t>&){++sent;});
    relay.SetValidateBlockCallback([&](const Block&,const std::string&){++validated;return BlockRelayManager::BlockValidationOutcome::Accepted;});
    relay.SetMempoolAccessFactory([]{return std::unique_ptr<MempoolAccess>{};});
    EXPECT_THROW(relay.HandleCompactBlock("fixture",compact),std::runtime_error);
    EXPECT_THROW(relay.AnnounceBlock(block.header.GetHash()),std::runtime_error);
    EXPECT_EQ(sent,0);EXPECT_EQ(validated,0);EXPECT_FALSE(relay.IsBlockSeen(block.header.GetHash()));
    EXPECT_EQ(relay.GetStats().blocks_seen,0U);EXPECT_EQ(relay.GetStats().compact_txns_requested,0U);
    auto stopped=MempoolServiceOwnerTestPeer::Published(db,coins,nullptr);stopped->Stop();
    relay.SetMempoolAccessFactory(factoryFor(stopped));EXPECT_THROW(relay.HandleCompactBlock("fixture",compact),std::runtime_error);
    EXPECT_THROW(relay.SetMempoolAccessFactory({}),std::invalid_argument);
    relay.SetMempoolAccessFactory(factoryFor(service));relay.HandleCompactBlock("fixture",compact);
    EXPECT_EQ(validated,1);EXPECT_EQ(sent,0);EXPECT_TRUE(relay.IsBlockSeen(block.header.GetHash()));
    // Explicitly unconfigured legacy null access still requests missing data.
    relay.SetMempool(nullptr);relay.HandleCompactBlock("fixture",CompactBlockCodec::CreateCompactBlock(blockWith(tx,5)));
    EXPECT_EQ(sent,1);EXPECT_EQ(validated,1);
}
}
