// Benign operation-lifetime checks on the patched implementation only.
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/block_validation.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/mempool.h"
#include "mining/block_assembler.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <gtest/gtest.h>
#include <future>
#include <atomic>
#include <filesystem>
#include <chrono>
namespace {
using namespace dinero;
class MiningChainGuard : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::unique_ptr<consensus::BlockValidator> validator;
    static constexpr const char* address="din1pmvnrlwkk87phdekfs65gfxv69qgjcnupanyyzw894rwd8e76n66q6cey44";
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mining_chain_guard_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    struct Guard final : BlockAssembler::ChainstateReadGuard {
        std::shared_ptr<ChainstateService> owner;
        std::unique_lock<AnnotatedRecursiveMutex> lock;
        unsigned& held;
        explicit Guard(std::shared_ptr<ChainstateService> service,unsigned& count)
            :owner(std::move(service)),lock(owner->AcquireBlockIngressActivationLock()),held(count){++held;}
        ~Guard() override {--held;}
    };
    static BlockAssembler::ChainstateReadGuardFactory factoryFor(
            const std::shared_ptr<ChainstateService>& service,unsigned& held,unsigned& acquired) {
        std::weak_ptr<ChainstateService> weak=service;
        return [weak,&held,&acquired]() -> std::unique_ptr<BlockAssembler::ChainstateReadGuard> {
            auto owner=weak.lock();if(!owner)return nullptr;
            ++acquired;return std::make_unique<Guard>(std::move(owner),held);
        };
    }
};
class ObservedPool final : public MempoolAccess {
public:
    ObservedPool(Mempool& pool,std::function<void()> observe):pool_(pool),observe_(std::move(observe)){}
    ~ObservedPool() override {observe_();}
    Mempool& Pool() const override {observe_();return pool_;}
private:
    Mempool& pool_;std::function<void()> observe_;
};
TEST_F(MiningChainGuard, ChainPrecedesPoolAndOutlivesPoolRelease) {
    auto service=std::make_shared<ChainstateService>();unsigned held=0,acquired=0,observed=0;
    Mempool pool(&db,&coins);BlockAssembler assembler(&db);wire(assembler);
    assembler.SetChainstateReadGuardFactory(factoryFor(service,held,acquired));
    assembler.SetMempoolAccessFactory([&]() -> std::unique_ptr<MempoolAccess> {
        service->AssertActivationLockHeld("mining pool acquisition");EXPECT_EQ(held,1U);
        return std::make_unique<ObservedPool>(pool,[&]{
            service->AssertActivationLockHeld("mining pool access or release");EXPECT_EQ(held,1U);++observed;
        });
    });
    struct CaptureGuard final : Mempool::ChainstateReadGuard {};
    pool.setChainstateReadGuardFactory([&]() -> std::unique_ptr<Mempool::ChainstateReadGuard> {
        service->AssertActivationLockHeld("mining selected capture");EXPECT_EQ(held,1U);
        return std::make_unique<CaptureGuard>();
    });
    auto block=assembler.CreateNewBlock(address);ASSERT_NE(block,nullptr);EXPECT_EQ(held,0U);EXPECT_EQ(acquired,1U);EXPECT_EQ(observed,2U);
    auto job=assembler.CreateJob();ASSERT_NE(job,nullptr);EXPECT_EQ(held,0U);EXPECT_EQ(acquired,2U);EXPECT_EQ(observed,4U);
    EXPECT_EQ(block->header.prev_block_hash,job->header.prev_block_hash);EXPECT_EQ(job->height,1U);
}
TEST_F(MiningChainGuard, CapturedFactorySurvivesReplacementAndUnavailableRefusesEarly) {
    auto service=std::make_shared<ChainstateService>();std::weak_ptr<ChainstateService> weak=service;
    unsigned held=0,acquired=0,pool_acquired=0;Mempool pool(&db,&coins);BlockAssembler assembler(&db);wire(assembler);
    assembler.SetChainstateReadGuardFactory(factoryFor(service,held,acquired));
    assembler.SetMempoolAccessFactory([&]() -> std::unique_ptr<MempoolAccess> {
        ++pool_acquired;EXPECT_EQ(held,1U);
        assembler.SetChainstateReadGuardFactory([]{return std::unique_ptr<BlockAssembler::ChainstateReadGuard>{};});
        service.reset();EXPECT_FALSE(weak.expired());
        return std::make_unique<ObservedPool>(pool,[&]{EXPECT_FALSE(weak.expired());EXPECT_EQ(held,1U);});
    });
    std::unordered_map<uint256,uint64_t> fees;
    ASSERT_NE(assembler.CreateNewBlock(address,{},&fees),nullptr);EXPECT_TRUE(weak.expired());EXPECT_EQ(held,0U);EXPECT_EQ(acquired,1U);
    const auto stats=assembler.getBlockTemplateStats();const auto error=assembler.getLastTemplateError();fees.emplace(uint256{},543);const auto before=fees;
    EXPECT_THROW(assembler.CreateNewBlock(address,{},&fees),std::runtime_error);
    EXPECT_THROW(assembler.CreateJob(),std::runtime_error);
    EXPECT_THROW(assembler.SetChainstateReadGuardFactory({}),std::invalid_argument);
    EXPECT_EQ(pool_acquired,1U);EXPECT_EQ(fees,before);EXPECT_EQ(assembler.getLastTemplateError(),error);
    EXPECT_EQ(assembler.getBlockTemplateStats().determinism_hash,stats.determinism_hash);
}
TEST_F(MiningChainGuard, ExplicitParentMustEqualCurrentDurableTip) {
    auto service=std::make_shared<ChainstateService>();unsigned held=0,acquired=0;
    Mempool pool(&db,&coins);BlockAssembler assembler(&db);wire(assembler);assembler.setMempool(&pool);
    assembler.SetChainstateReadGuardFactory(factoryFor(service,held,acquired));
    const auto tip=db.getTip();ASSERT_TRUE(tip.ok());const auto selected=tip->hash;
    // A separately recorded header is not selected merely because its height is known.
    auto other=BuildCanonicalGenesis(Params()).header;other.nonce+=1;Block block{};block.header=other;const auto unselected=block.GetHash();
    ASSERT_NE(unselected,selected);
    ASSERT_EQ(db.putHeader(ChainWriteToken::CreateForTesting(),unselected,other,0,arith_uint256(0)),Status::Ok);
    EXPECT_EQ(assembler.CreateJob(&unselected),nullptr);EXPECT_EQ(held,0U);
    auto retry=assembler.CreateJob(&selected);ASSERT_NE(retry,nullptr);EXPECT_EQ(retry->header.prev_block_hash,selected);EXPECT_EQ(retry->height,1U);
    EXPECT_EQ(held,0U);EXPECT_EQ(acquired,2U);EXPECT_EQ(db.getTip()->hash,selected);
}
}
