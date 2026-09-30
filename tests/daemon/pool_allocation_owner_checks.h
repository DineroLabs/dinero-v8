#pragma once
#include "pool_round_owner_checks.h"
#include <limits>
namespace pool_allocation_owner_checks {
using namespace dinero::pool;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
struct Fixture : pool_round_owner_checks::Fixture {
    Fixture() {Sql(raw(),"DELETE FROM payouts");}
    uint32_t run() {return PayoutCalculator(manager->getDatabase(),config).processConfirmedBlocks();}
    void second() {
        Sql(raw(),"INSERT INTO blocks(block_id,block_hash,height,finder_worker,finder_address,reward,total_reward,pool_fee_percent,pool_fee_amount,distributable,confirmations,required_confirmations,found_at) VALUES(2,'second-block',8,'c','address-c',5000,5000,0,0,5000,100,100,124)");
        Sql(raw(),"INSERT INTO rounds(round_id,block_id,total_shares,total_difficulty,started_at,ended_at) VALUES(30,2,1,1,20,30)");
        Sql(raw(),"INSERT INTO round_shares(round_id,worker_id,difficulty_sum,share_count) VALUES(30,'c',1,1)");
    }
};
TEST(PoolAllocationOwner, ActualCalculatorAtomicReopenAndIdempotence) {
    Fixture f;const auto sync=Read(f.raw(),"PRAGMA synchronous");
    ASSERT_EQ(f.run(),1u);
    EXPECT_EQ(Read(f.raw(),"SELECT worker_id,pending_payout FROM workers ORDER BY worker_id"),(Rows{{"3:a","1:2600"},{"3:b","1:7510"},{"3:c","1:9"},{"3:d","1:8"}}));
    EXPECT_EQ(Read(f.raw(),"SELECT block_id,worker_id,wallet_address,amount,status,txid,paid_at,retry_count FROM payouts ORDER BY worker_id"),(Rows{{"1:1","3:a","3:address-a","1:2500","1:1","3:","1:0","1:0"},{"1:1","3:b","3:address-b","1:7500","1:1","3:","1:0","1:0"}}));
    EXPECT_EQ(Read(f.raw(),"SELECT payouts_calculated FROM blocks"),(Rows{{"1:1"}}));
    const auto after=f.state();EXPECT_EQ(f.run(),0u);EXPECT_EQ(f.state(),after);EXPECT_EQ(Read(f.raw(),"PRAGMA synchronous"),sync);
    f.manager.reset();f.open();EXPECT_EQ(f.state(),after);EXPECT_EQ(f.run(),0u);EXPECT_EQ(f.state(),after);
}
TEST(PoolAllocationOwner, RequiredWritesAndCommitRollbackWithoutPublishedCount) {
    for(int failure=0;failure<4;++failure) {
        Fixture f;
        if(failure==0)Sql(f.raw(),"CREATE TRIGGER refuse_insert BEFORE INSERT ON payouts WHEN NEW.worker_id='b' BEGIN SELECT RAISE(ABORT,'fixture payout refusal'); END");
        if(failure==1)Sql(f.raw(),"CREATE TRIGGER refuse_credit BEFORE UPDATE OF pending_payout ON workers WHEN OLD.worker_id='b' BEGIN SELECT RAISE(ABORT,'fixture credit refusal'); END");
        if(failure==2)Sql(f.raw(),"CREATE TRIGGER refuse_flag BEFORE UPDATE OF payouts_calculated ON blocks BEGIN SELECT RAISE(ABORT,'fixture flag refusal'); END");
        auto before=f.state();uint32_t count=99;
        struct Commit {uint32_t* count;bool called=false,unpublished=false;};Commit commit{&count};
        if(failure==3)sqlite3_commit_hook(f.raw(),[](void* p){auto& c=*static_cast<Commit*>(p);c.called=true;c.unpublished=*c.count==99;return 1;},&commit);
        EXPECT_THROW(count=f.run(),std::runtime_error);sqlite3_commit_hook(f.raw(),nullptr,nullptr);
        EXPECT_EQ(count,99u);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
        if(failure==3){EXPECT_TRUE(commit.called);EXPECT_TRUE(commit.unpublished);}
        if(failure==0)Sql(f.raw(),"DROP TRIGGER refuse_insert");if(failure==1)Sql(f.raw(),"DROP TRIGGER refuse_credit");if(failure==2)Sql(f.raw(),"DROP TRIGGER refuse_flag");
        ASSERT_EQ(f.run(),1u);
    }
}
TEST(PoolAllocationOwner, CompleteReadsAndExistingOwnersRefuseWithoutWrites) {
    for(const auto* change:{"UPDATE blocks SET payouts_calculated=2","UPDATE blocks SET confirmations='invalid'","UPDATE blocks SET height=4294967296","UPDATE workers SET wallet_address=CAST(X'616464726573732D610078' AS TEXT) WHERE worker_id='a'","DELETE FROM workers WHERE worker_id='a'","UPDATE workers SET pending_payout=-1 WHERE worker_id='b'","UPDATE workers SET pending_payout=9223372036854775807 WHERE worker_id='b'"}) {
        Fixture f;Sql(f.raw(),change);const auto before=f.state();EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
    }
    Fixture f;auto before=f.state();
    sqlite3_set_authorizer(f.raw(),[](void*,int action,const char* table,const char*,const char*,const char*) {return action==SQLITE_READ&&table&&std::strcmp(table,"blocks")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(f.run(),std::runtime_error);sqlite3_set_authorizer(f.raw(),nullptr,nullptr);EXPECT_EQ(f.state(),before);
    struct Interrupt {sqlite3* db;bool hit=false;};Interrupt interruption{f.raw()};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* ctx,void* stmt,void*) {auto& i=*static_cast<Interrupt*>(ctx);const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(sql&&std::strstr(sql,"confirmed_at FROM blocks ORDER BY")){i.hit=true;sqlite3_interrupt(i.db);}return 0;},&interruption);
    EXPECT_THROW(f.run(),std::runtime_error);sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_TRUE(interruption.hit);EXPECT_EQ(f.state(),before);
    Payout old;old.block_id=1;old.worker_id="a";old.wallet_address="address-a";old.amount=4;old.calculated_at=10;
    ASSERT_TRUE(f.manager->getDatabase().insertPayout(old));before=f.state();EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before);
    // An existing row is ambiguous historical allocation, never authorization to duplicate it.
}
TEST(PoolAllocationOwner, BorrowedTransactionAndLateBatchFailurePreserveAllOwners) {
    Fixture f;f.second();const auto before=f.state();const auto sync=Read(f.raw(),"PRAGMA synchronous");
    Sql(f.raw(),"BEGIN IMMEDIATE; UPDATE workers SET last_seen=42 WHERE worker_id='d'");const auto borrowed=f.state();
    EXPECT_THROW(f.run(),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));EXPECT_EQ(f.state(),borrowed);
    Sql(f.raw(),"ROLLBACK");EXPECT_EQ(f.state(),before);EXPECT_EQ(Read(f.raw(),"PRAGMA synchronous"),sync);
    Sql(f.raw(),"CREATE TRIGGER refuse_late BEFORE UPDATE OF payouts_calculated ON blocks WHEN OLD.block_id=2 BEGIN SELECT RAISE(ABORT,'fixture second block refusal'); END");
    EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
    Sql(f.raw(),"DROP TRIGGER refuse_late");ASSERT_EQ(f.run(),2u);
    EXPECT_EQ(Read(f.raw(),"SELECT payouts_calculated FROM blocks ORDER BY block_id"),(Rows{{"1:1"},{"1:1"}}));
    EXPECT_EQ(Read(f.raw(),"SELECT pending_payout FROM workers WHERE worker_id='c'"),(Rows{{"1:5009"}}));
    EXPECT_EQ(f.run(),0u);
}
} // namespace pool_allocation_owner_checks
