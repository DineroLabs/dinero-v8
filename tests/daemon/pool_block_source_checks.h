#pragma once
#include "pool/canonical_block.h"
#include "rpc/methods_pool.h"
#include "rpc/rpc_registry.h"
namespace dinero {
namespace {
struct PoolBlockRpcFixture {
    std::filesystem::path root;
    std::shared_ptr<pool::PoolManager> manager;
    std::shared_ptr<pool::PoolDB> observer;
    PoolBlockRpcFixture(){
        root=std::filesystem::temp_directory_path()/("pool_block_source_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!std::filesystem::create_directory(root))throw std::runtime_error("fixture directory");
        open();manager->getDatabase().getOrCreateWorker("miner","fixture-worker-address");
    }
    ~PoolBlockRpcFixture(){din::rpc::configurePoolRpc(nullptr,nullptr,false,"fixture complete");manager.reset();observer.reset();std::error_code ec;std::filesystem::remove_all(root,ec);}
    void open() {
        const auto name=(root/"pool.sqlite").string();manager=std::make_shared<pool::PoolManager>(name);observer=std::make_shared<pool::PoolDB>(name);
        if(!manager->initialize() || !observer->initialize())throw std::runtime_error("fixture pool RPC initialization");
        din::rpc::registerPoolMethods();din::rpc::configurePoolRpc(observer,manager,true,"");
    }
    sqlite3* raw(){return pool::PoolOrphanAccountingTestAccess::Database(manager->getDatabase());}
    auto state() {
        std::vector<pool_orphan_accounting_checks::Rows> result;
        for(const auto* table:{"workers","shares","blocks","rounds","round_shares","share_dedupe"}) {
            result.push_back(pool_orphan_accounting_checks::Read(raw(),std::string("SELECT * FROM ")+table+" ORDER BY rowid"));
        }
        return result;
    }
    din::Json request(bool block=false) {
        din::Json p;p["worker_id"]="miner";p["job_id"]="job";p["difficulty"]=64.0;p["is_valid"]=true;p["is_stale"]=false;p["is_block"]=block;p["share_uid"]="source-uid";
        if(block){p["block_hash"]=std::string(64,'a');p["block_height"]=1;p["block_reward"]=din::Json::UInt64(1);}return p;
    }
    din::Json call(const din::Json& p,DaemonContext* daemon=nullptr) {
        const auto* fn=g_rpcRegistry.lookup("pool.submitshare");if(!fn)throw std::runtime_error("actual submitshare RPC missing");
        const auto handler=*fn;ExecutionContext ctx;ctx.daemon=daemon;return handler(ctx,p);
    }
};
}
TEST(PoolBlockSource, MissingSelectedSourceCannotRecordFoundBlock) {
    PoolBlockRpcFixture f;const auto before=f.state();const auto p=f.request(true);
    EXPECT_TRUE(f.call(p).isMember("error"));EXPECT_EQ(f.state(),before);
    DaemonContext empty;EXPECT_TRUE(f.call(p,&empty).isMember("error"));EXPECT_EQ(f.state(),before);
    EXPECT_TRUE(f.call(f.request())["success"].asBool());EXPECT_EQ(f.manager->getDatabase().getPendingBlocks().size(),0u);
}
TEST(PoolBlockSource, NonBlockFractionalDifficultyBoundsAndDuplicates) {
    PoolBlockRpcFixture f;const auto before=f.state();auto p=f.request();
    for(double bad:{4294967296.0,1e30}) {p["difficulty"]=bad;EXPECT_TRUE(f.call(p).isMember("error"));EXPECT_EQ(f.state(),before);}
    for(double bad:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(f.manager->onShareSubmit("miner","job",bad,true,false,false).code,pool::PoolManager::ShareSubmitCode::REJECTED);EXPECT_EQ(f.state(),before);
    }
    p["difficulty"]=4294967295.5;ASSERT_TRUE(f.call(p)["success"].asBool());const auto accepted=f.state();
    EXPECT_TRUE(f.call(p)["duplicate"].asBool());EXPECT_EQ(f.state(),accepted);
    const auto rows=pool_orphan_accounting_checks::Read(f.raw(),"SELECT difficulty,difficulty_real FROM shares");
    ASSERT_EQ(rows.size(),1u);EXPECT_EQ(rows[0][0],"1:4294967295");
    auto check=[&] {
        const auto shares=f.manager->getDatabase().getLastNShares(1);
        ASSERT_EQ(shares.size(),1u);EXPECT_EQ(shares[0].difficulty,UINT32_MAX);
        EXPECT_DOUBLE_EQ(shares[0].difficulty_real,4294967295.5);
        const auto worker=f.manager->getDatabase().getWorkerShares("miner",UINT32_MAX);
        ASSERT_EQ(worker.size(),1u);EXPECT_EQ(worker[0].difficulty,UINT32_MAX);
        EXPECT_DOUBLE_EQ(worker[0].difficulty_real,4294967295.5);
    };
    check();din::rpc::configurePoolRpc(nullptr,nullptr,false,"fixture reopen");
    f.manager.reset();f.observer.reset();f.open();check();
    pool_orphan_accounting_checks::Sql(f.raw(),"UPDATE shares SET difficulty=-1");
    EXPECT_THROW(f.manager->getDatabase().getLastNShares(1),std::runtime_error);
    EXPECT_THROW(f.manager->getDatabase().getWorkerShares("miner",1),std::runtime_error);

}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct CanonicalPoolBlockRpcFixture : VaultObservationFixture {
    PoolBlockRpcFixture pool;
    din::Json request(uint32_t height,const Transaction& coinbase,const uint256& hash) {
        auto p=pool.request(true);p["block_hash"]=hash.GetHex();p["block_height"]=height;
        uint64_t reward=0;for(const auto& o:coinbase.vout)reward+=o.value.GetUna();p["block_reward"]=din::Json::UInt64(reward);return p;
    }
    din::Json historical() {
        ArchiveHistorical(1);const auto& block=f.blocks[1];
        OrchardAdmissionFixture::Require(f.db.putTxIndex(f.token,block.vtx.front().GetTxid().AsUint256(),block.GetHash(),0)==Status::Ok);
        return request(1,block.vtx.front(),block.GetHash());
    }
    std::pair<din::Json,std::shared_ptr<const RuntimeBlockBody>> mixed() {
        const auto [body,bundle]=Shield(Keys());const auto block=Mine(body);
        return {request(102,block->Orchard().Transactions().front().Historical(),block->Orchard().Header().GetHash()),block};
    }
};
}
TEST(PoolBlockSource, ActualHistoricalAndMixedRpcBindExactCoinbaseRewards) {
    CanonicalPoolBlockRpcFixture f;auto p=f.historical();ASSERT_TRUE(f.pool.call(p,&f.context)["success"].asBool());
    auto rows=f.pool.manager->getDatabase().getPendingBlocks();ASSERT_EQ(rows.size(),1u);EXPECT_EQ(rows[0].block_hash,p["block_hash"].asString());EXPECT_EQ(rows[0].height,1u);EXPECT_EQ(rows[0].total_reward,p["block_reward"].asUInt64());
    const auto before=f.pool.state();EXPECT_TRUE(f.pool.call(p,&f.context)["duplicate"].asBool());EXPECT_EQ(f.pool.state(),before);
    auto [mixed,body]=f.mixed();mixed["share_uid"]="mixed-uid";mixed["job_id"]="mixed-job";
    ASSERT_TRUE(f.pool.call(mixed,&f.context)["success"].asBool());rows=f.pool.manager->getDatabase().getPendingBlocks();ASSERT_EQ(rows.size(),2u);
    const auto found=std::find_if(rows.begin(),rows.end(),[](const auto& b){return b.height==102;});ASSERT_NE(found,rows.end());EXPECT_EQ(found->total_reward,mixed["block_reward"].asUInt64());EXPECT_EQ(found->block_hash,body->Orchard().Header().GetHash().GetHex());
}
TEST(PoolBlockSource, WrongCanonicalFieldsRefuseBeforePoolEffects) {
    CanonicalPoolBlockRpcFixture f;const auto valid=f.historical();const auto before=f.pool.state();
    for(int bad=0;bad<5;++bad) {
        auto p=valid;
        if(bad==0)p["block_hash"]=std::string(64,'b');
        if(bad==1)p["block_height"]=2;
        if(bad==2)p["block_reward"]=din::Json::UInt64(valid["block_reward"].asUInt64()+1);
        if(bad==3)p["block_reward"]=din::Json::UInt64(valid["block_reward"].asUInt64()-1);
        if(bad==4)p["block_reward"]=din::Json::UInt64(MAX_SUPPLY_UNA_CONST+1);
        EXPECT_TRUE(f.pool.call(p,&f.context).isMember("error"))<<bad;EXPECT_EQ(f.pool.state(),before);
    }
    ASSERT_TRUE(f.pool.call(valid,&f.context)["success"].asBool());
}
TEST(PoolBlockSource, DisconnectedAndUnavailableBodiesNeverRecordReports) {
    CanonicalPoolBlockRpcFixture f;auto [request,block]=f.mixed();const auto before=f.pool.state();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_TRUE(f.pool.call(request,&f.context).isMember("error"));EXPECT_EQ(f.pool.state(),before);
    const auto connected=f.Submit(block->Orchard().WireBytes());ASSERT_TRUE(connected.accepted()&&connected.connected);
    f.f.db.close();EXPECT_TRUE(f.pool.call(request,&f.context).isMember("error"));EXPECT_EQ(f.pool.state(),before);
    ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);ASSERT_TRUE(f.pool.call(request,&f.context)["success"].asBool());
}
TEST(PoolBlockSource, AccountingCommitFailureAndReopenKeepExactDeduplication) {
    CanonicalPoolBlockRpcFixture f;const auto request=f.historical();const auto before=f.pool.state();
    bool hit=false;sqlite3_commit_hook(f.pool.raw(),[](void* p){*static_cast<bool*>(p)=true;return 1;},&hit);
    EXPECT_TRUE(f.pool.call(request,&f.context).isMember("error"));sqlite3_commit_hook(f.pool.raw(),nullptr,nullptr);EXPECT_TRUE(hit);EXPECT_EQ(f.pool.state(),before);
    pool_orphan_accounting_checks::Sql(f.pool.raw(),"CREATE TRIGGER refuse_block BEFORE INSERT ON blocks BEGIN SELECT RAISE(ABORT,'fixture block refusal'); END");
    EXPECT_TRUE(f.pool.call(request,&f.context).isMember("error"));pool_orphan_accounting_checks::Sql(f.pool.raw(),"DROP TRIGGER refuse_block");EXPECT_EQ(f.pool.state(),before);
    ASSERT_TRUE(f.pool.call(request,&f.context)["success"].asBool());const auto recorded=f.pool.state();
    din::rpc::configurePoolRpc(nullptr,nullptr,false,"fixture reopen");f.pool.manager.reset();f.pool.observer.reset();f.pool.open();
    EXPECT_TRUE(f.pool.call(request,&f.context)["duplicate"].asBool());EXPECT_EQ(f.pool.state(),recorded);
}
#endif
} // namespace dinero
