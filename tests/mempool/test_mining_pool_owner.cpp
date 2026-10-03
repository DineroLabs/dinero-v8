// Benign operation-lifetime checks on the patched implementation only.
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/block_validation.h"
#include "daemon/services/mempool_service.h"
#include "mining/block_assembler.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <gtest/gtest.h>
#include <future>
#include <atomic>
#include <filesystem>
#include <chrono>
namespace dinero {
class MempoolServiceOwnerTestPeer {
public:
    static std::shared_ptr<MempoolService> Published(ChainDB& db,consensus::ConsensusUTXOSet& coins) {
        auto service=std::make_shared<MempoolService>();
        service->mempool_=std::make_unique<Mempool>(&db,&coins);
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
class MiningPoolOwner : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::unique_ptr<consensus::BlockValidator> validator;
    static constexpr const char* address="din1pmvnrlwkk87phdekfs65gfxv69qgjcnupanyyzw894rwd8e76n66q6cey44";
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mining_pool_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);
        const auto genesis=BuildCanonicalGenesis(Params());Block block{};block.header=genesis.header;
        const auto hash=block.GetHash();auto token=ChainWriteToken::CreateForTesting();
        ASSERT_EQ(db.putBlock(token,hash,block),Status::Ok);
        ASSERT_EQ(db.putHeader(token,hash,genesis.header,0,arith_uint256(0)),Status::Ok);
        ASSERT_EQ(db.setTip(token,hash,0,arith_uint256(0)),Status::Ok);
        coins.SetBestBlock(hash,0);validator=std::make_unique<consensus::BlockValidator>(&coins);
    }
    void TearDown() override {validator.reset();db.close();std::filesystem::remove_all(root);}
    void wire(BlockAssembler& assembler) {
        assembler.SetConsensusUTXOSet(&coins);assembler.SetBlockValidator(validator.get());
        assembler.SetUTXOProvider(std::shared_ptr<consensus::IUTXOProvider>(&coins,[](auto*){}));
        assembler.SetMiningAddress(address);
    }
    static MempoolAccessFactory factoryFor(const std::shared_ptr<MempoolService>& service) {
        std::weak_ptr<MempoolService> weak=service;
        return [weak]() -> std::unique_ptr<MempoolAccess> {return MempoolService::AcquirePoolUse(weak.lock());};
    }
};
class PausedAccess final : public MempoolAccess {
public:
    PausedAccess(std::unique_ptr<MempoolAccess> owner,std::promise<void>& entered,std::shared_future<void> release)
        :owner_(std::move(owner)),entered_(entered),release_(std::move(release)) {}
    Mempool& Pool() const override {entered_.set_value();release_.wait();return owner_->Pool();}
private:
    std::unique_ptr<MempoolAccess> owner_;std::promise<void>& entered_;std::shared_future<void> release_;
};
TEST_F(MiningPoolOwner, RpcTemplateRetainsPoolThroughStop) {
    auto service=MempoolServiceOwnerTestPeer::Published(db,coins);
    BlockAssembler assembler(&db);wire(assembler);
    std::promise<void> entered,release;auto released=release.get_future().share();
    auto factory=factoryFor(service);
    assembler.SetMempoolAccessFactory([&]() -> std::unique_ptr<MempoolAccess> {return std::make_unique<PausedAccess>(factory(),entered,released);});
    auto job=std::async(std::launch::async,[&]{return assembler.CreateNewBlock(address);});
    auto ready=entered.get_future();EXPECT_EQ(ready.wait_for(std::chrono::seconds(5)),std::future_status::ready);
    std::atomic<bool> stopped{false};auto stopping=std::async(std::launch::async,[&]{service->Stop();stopped=true;});
    EXPECT_TRUE(MempoolServiceOwnerTestPeer::WaitForStopping(*service));EXPECT_FALSE(stopped.load());
    release.set_value();auto block=job.get();stopping.get();EXPECT_TRUE(stopped.load());
    ASSERT_NE(block,nullptr);ASSERT_EQ(block->vtx.size(),1U);EXPECT_TRUE(block->vtx[0].IsCoinbase());
    EXPECT_FALSE(block->header.utreexo_root.IsNull());EXPECT_EQ(assembler.getBlockTemplateStats().height,1U);
}
TEST_F(MiningPoolOwner, StratumJobUsesOneCapturedPool) {
    auto service=MempoolServiceOwnerTestPeer::Published(db,coins);std::weak_ptr<MempoolService> weak=service;
    BlockAssembler assembler(&db);wire(assembler);
    std::promise<void> entered,release;auto released=release.get_future().share();int acquisitions=0;
    auto factory=factoryFor(service);
    assembler.SetMempoolAccessFactory([&]() -> std::unique_ptr<MempoolAccess> {
        ++acquisitions;auto owner=factory();
        assembler.SetMempoolAccessFactory([]{return std::unique_ptr<MempoolAccess>{};});
        return std::make_unique<PausedAccess>(std::move(owner),entered,released);
    });
    auto job=std::async(std::launch::async,[&]{return assembler.CreateJob();});
    auto ready=entered.get_future();EXPECT_EQ(ready.wait_for(std::chrono::seconds(5)),std::future_status::ready);
    service.reset();EXPECT_FALSE(weak.expired());release.set_value();auto result=job.get();EXPECT_TRUE(weak.expired());
    ASSERT_NE(result,nullptr);EXPECT_EQ(result->height,1U);EXPECT_EQ(result->transactions.size(),1U);EXPECT_EQ(acquisitions,1);
    EXPECT_THROW(assembler.CreateJob(),std::runtime_error);
}
TEST_F(MiningPoolOwner, UnavailableOwnerPreservesTemplateStateAndBorrowedAccess) {
    auto service=MempoolServiceOwnerTestPeer::Published(db,coins);
    BlockAssembler assembler(&db);wire(assembler);assembler.SetMempoolAccessFactory(factoryFor(service));
    ASSERT_NE(assembler.CreateNewBlock(address),nullptr);
    const auto before=assembler.getBlockTemplateStats();const auto error=assembler.getLastTemplateError();
    assembler.SetMempoolAccessFactory([]{return std::unique_ptr<MempoolAccess>{};});
    EXPECT_THROW(assembler.CreateNewBlock(address),std::runtime_error);
    EXPECT_THROW(assembler.CreateJob(),std::runtime_error);
    EXPECT_EQ(assembler.getBlockTemplateStats().determinism_hash,before.determinism_hash);
    EXPECT_EQ(assembler.getBlockTemplateStats().total_fees,before.total_fees);EXPECT_EQ(assembler.getLastTemplateError(),error);
    EXPECT_THROW(assembler.getMempool(),std::logic_error);
    EXPECT_THROW(assembler.SetMempoolAccessFactory({}),std::invalid_argument);
    assembler.setMempool(&service->mempool());EXPECT_EQ(assembler.getMempool(),&service->mempool());
    EXPECT_NE(assembler.CreateNewBlock(address),nullptr);
}
}
