#pragma once
#include "pool_allocation_owner_checks.h"
namespace pool_calculation_inputs_checks {
using namespace dinero::pool;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
struct Fixture : pool_allocation_owner_checks::Fixture {
    Fixture() {
        config.payout_mode=PayoutMode::PPLNS;config.pplns_window=2;config.pps_rate=100;
        Sql(raw(),"DELETE FROM shares");
        Sql(raw(),"INSERT INTO shares(share_id,worker_id,wallet_address,job_id,difficulty,difficulty_real,status,block_hash,block_height,block_reward,submitted_at) VALUES(3,'a','address-a','job-a',1,1,0,NULL,0,0,20),(9,'b','address-b','job-b',4294967295,3,0,'',0,0,30),(20,'c','address-c','job-c',1,2,4,'block-c',7,10000,40),(25,'d','address-d','job-d',1,9,1,NULL,0,0,70)");
    }
    Rows state() {auto s=pool_allocation_owner_checks::Fixture::state();auto shares=Read(raw(),"SELECT * FROM shares ORDER BY share_id");s.insert(s.end(),shares.begin(),shares.end());return s;}
};
TEST(PoolCalculationInputs, ExactSelectionAndActualCalculatorsPreserveRows) {
    Fixture f;auto& db=f.manager->getDatabase();const auto before=f.state();
    const auto last=db.getLastNShares(2);ASSERT_EQ(last.size(),2u);EXPECT_EQ(last[0].share_id,9u);EXPECT_EQ(last[1].share_id,3u);
    EXPECT_EQ(last[0].difficulty,UINT32_MAX);EXPECT_TRUE(last[1].block_hash.empty());
    const auto range=db.getSharesInRange(20,40);ASSERT_EQ(range.size(),3u);EXPECT_EQ(range[0].share_id,20u);EXPECT_EQ(range[1].share_id,9u);EXPECT_EQ(range[2].share_id,3u);
    EXPECT_EQ(range[0].status,ShareStatus::BLOCK);EXPECT_TRUE(db.getLastNShares(0).empty());EXPECT_TRUE(db.getSharesInRange(80,90).empty());EXPECT_TRUE(db.getRecentBlocks(0).empty());
    PayoutCalculator calc(db,f.config);const auto window=calc.calculatePPLNS(f.block());ASSERT_EQ(window.size(),2u);
    EXPECT_EQ(window[0].worker_id,"a");EXPECT_EQ(window[0].amount,2500u);EXPECT_EQ(window[1].worker_id,"b");EXPECT_EQ(window[1].amount,7500u);
    const auto pps=calc.calculatePPS(f.block());ASSERT_EQ(pps.size(),3u);EXPECT_EQ(pps[0].amount,100u);EXPECT_EQ(pps[1].amount,300u);EXPECT_EQ(pps[2].amount,200u);
    EXPECT_EQ(f.state(),before);f.manager.reset();f.open();EXPECT_EQ(f.state(),before);
    const auto reopened=f.manager->getDatabase().getLastNShares(2);ASSERT_EQ(reopened.size(),2u);EXPECT_EQ(reopened[0].share_id,9u);
    ASSERT_EQ(f.run(),1u);EXPECT_EQ(Read(f.raw(),"SELECT worker_id,amount,status FROM payouts ORDER BY worker_id"),(Rows{{"3:a","1:2500","1:1"},{"3:b","1:7500","1:1"}}));
}
TEST(PoolCalculationInputs, MalformedLaterInputsRefuseBeforeAllocation) {
    for(const auto* change:{"UPDATE shares SET difficulty_real=-1 WHERE share_id=3","UPDATE shares SET difficulty_real=1e999 WHERE share_id=3","UPDATE shares SET difficulty_real='invalid' WHERE share_id=3","UPDATE shares SET difficulty=4294967296 WHERE share_id=3","UPDATE shares SET block_height=4294967296 WHERE share_id=3","UPDATE shares SET block_reward=-1 WHERE share_id=3","UPDATE shares SET wallet_address=X'61' WHERE share_id=3","UPDATE shares SET worker_id=CAST(X'610078' AS TEXT) WHERE share_id=3"}) {
        Fixture f;Sql(f.raw(),change);const auto before=f.state();auto& db=f.manager->getDatabase();
        EXPECT_THROW(db.getLastNShares(2),std::runtime_error);
        EXPECT_THROW(db.getSharesInRange(0,123),std::runtime_error);
        EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
    }
    for(const auto* change:{"UPDATE blocks SET total_reward='invalid' WHERE block_id=1","UPDATE blocks SET finder_worker=X'61' WHERE block_id=1","UPDATE blocks SET payouts_sent=2 WHERE block_id=1","UPDATE blocks SET round_difficulty=-1 WHERE block_id=1"}) {
        Fixture f;f.second();Sql(f.raw(),change);const auto before=f.state();
        EXPECT_THROW(f.manager->getDatabase().getRecentBlocks(2),std::runtime_error);EXPECT_EQ(f.state(),before);
    }
    Fixture f;Sql(f.raw(),"UPDATE shares SET status=9 WHERE share_id=3");const auto before=f.state();
    EXPECT_THROW(f.manager->getDatabase().getSharesInRange(0,123),std::runtime_error);EXPECT_EQ(f.state(),before);
    EXPECT_THROW(f.manager->getDatabase().getLastNShares(UINT64_MAX),std::runtime_error);
    EXPECT_THROW(f.manager->getDatabase().getSharesInRange(-1,20),std::runtime_error);
    EXPECT_THROW(f.manager->getDatabase().getSharesInRange(30,20),std::runtime_error);
}
TEST(PoolCalculationInputs, ReadRefusalAndTerminalCompletionNeverReturnPrefix) {
    for(int selection=0;selection<3;++selection) {
        Fixture f;f.second();auto& db=f.manager->getDatabase();const auto before=f.state();
        const auto invoke=[&] {if(selection==0)(void)db.getLastNShares(2);else if(selection==1)(void)db.getSharesInRange(0,123);else(void)db.getRecentBlocks(2);};
        const char* table=selection==2?"blocks":"shares";
        sqlite3_set_authorizer(f.raw(),[](void* ctx,int action,const char* name,const char*,const char*,const char*) {return action==SQLITE_READ&&name&&std::strcmp(name,static_cast<const char*>(ctx))==0?SQLITE_DENY:SQLITE_OK;},const_cast<char*>(table));
        EXPECT_THROW(invoke(),std::runtime_error);sqlite3_set_authorizer(f.raw(),nullptr,nullptr);EXPECT_EQ(f.state(),before);
        struct Interrupt {sqlite3* db;const char* marker;int rows=0;};Interrupt interrupt{f.raw(),selection==2?"FROM blocks ORDER BY found_at DESC":"FROM shares "};
        sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* ctx,void* stmt,void*) {auto& i=*static_cast<Interrupt*>(ctx);const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(sql&&std::strstr(sql,i.marker)&&++i.rows==2)sqlite3_interrupt(i.db);return 0;},&interrupt);
        EXPECT_THROW(invoke(),std::runtime_error);sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_EQ(interrupt.rows,2);EXPECT_EQ(f.state(),before);EXPECT_NO_THROW(invoke());
    }
}
TEST(PoolCalculationInputs, BorrowedReadAndPpsInputFailurePreserveAllocationOwner) {
    Fixture f;auto& db=f.manager->getDatabase();const auto before=f.state();
    Sql(f.raw(),"BEGIN IMMEDIATE; UPDATE shares SET difficulty_real=2 WHERE share_id=3");
    const auto captured=db.getLastNShares(2);ASSERT_EQ(captured.size(),2u);EXPECT_EQ(captured[1].difficulty_real,2.0);EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));
    EXPECT_EQ(db.getSharesInRange(0,123).size(),4u);EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));
    EXPECT_EQ(db.getRecentBlocks(2).size(),1u);EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));
    Sql(f.raw(),"ROLLBACK");EXPECT_EQ(f.state(),before);
    f.config.payout_mode=PayoutMode::PPS;
    sqlite3_set_authorizer(f.raw(),[](void*,int action,const char* table,const char*,const char*,const char*) {return action==SQLITE_READ&&table&&std::strcmp(table,"shares")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(f.run(),std::runtime_error);sqlite3_set_authorizer(f.raw(),nullptr,nullptr);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
    ASSERT_EQ(f.run(),1u);EXPECT_EQ(Read(f.raw(),"SELECT worker_id,amount FROM payouts ORDER BY worker_id"),(Rows{{"3:a","1:100"},{"3:b","1:300"},{"3:c","1:200"}}));
}
} // namespace pool_calculation_inputs_checks
