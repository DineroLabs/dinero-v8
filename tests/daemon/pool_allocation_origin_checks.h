#pragma once
#include "pool_allocation_owner_checks.h"
namespace pool_allocation_origin_checks {
using namespace dinero::pool;
using pool_allocation_owner_checks::Fixture;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
TEST(PoolAllocationOrigins, ActualCreationAndReopenPreserveReferences) {
    Fixture f;ASSERT_EQ(f.run(),1u);auto& db=f.manager->getDatabase();
    const auto rows=db.getPayoutsForBlock(1);ASSERT_EQ(rows.size(),2u);
    ASSERT_TRUE(rows[0].allocation_origin);ASSERT_TRUE(rows[1].allocation_origin);
    EXPECT_NE(*rows[0].allocation_origin,*rows[1].allocation_origin);
    for(const auto& row:rows)EXPECT_TRUE(std::any_of(row.allocation_origin->begin(),row.allocation_origin->end(),[](uint8_t b){return b!=0;}));
    const auto before=f.state();EXPECT_EQ(f.run(),0u);EXPECT_EQ(f.state(),before);
    f.manager.reset();f.open();EXPECT_EQ(f.state(),before);
    const auto reopened=f.manager->getDatabase().getPayoutsForBlock(1);ASSERT_EQ(reopened.size(),2u);
    for(size_t i=0;i<rows.size();++i) {
        EXPECT_EQ(reopened[i].payout_id,rows[i].payout_id);EXPECT_EQ(reopened[i].worker_id,rows[i].worker_id);
        EXPECT_EQ(reopened[i].amount,rows[i].amount);EXPECT_EQ(reopened[i].allocation_origin,rows[i].allocation_origin);
    }
    // The database uniqueness guard refuses accidental reuse, preserving both.
    EXPECT_THROW(Sql(f.raw(),"UPDATE payouts SET allocation_origin=(SELECT allocation_origin FROM payouts WHERE worker_id='a') WHERE worker_id='b'"),std::runtime_error);
    EXPECT_EQ(f.state(),before);EXPECT_EQ(f.run(),0u);EXPECT_EQ(f.state(),before);
}
TEST(PoolAllocationOrigins, RequiredReferenceInsertAndCommitAreAtomic) {
    for(int failure=0;failure<2;++failure) {
        Fixture f;const auto before=f.state();uint32_t published=91;
        if(failure==0)Sql(f.raw(),"CREATE TRIGGER refuse_reference BEFORE INSERT ON payouts WHEN NEW.worker_id='b' AND typeof(NEW.allocation_origin)='blob' AND length(NEW.allocation_origin)=32 BEGIN SELECT RAISE(ABORT,'fixture reference refusal'); END");
        struct Commit {uint32_t* count;bool called=false;bool old=false;};Commit commit{&published};
        if(failure==1)sqlite3_commit_hook(f.raw(),[](void* raw){auto& c=*static_cast<Commit*>(raw);c.called=true;c.old=*c.count==91;return 1;},&commit);
        EXPECT_THROW(published=f.run(),std::runtime_error);
        sqlite3_commit_hook(f.raw(),nullptr,nullptr);EXPECT_EQ(published,91u);EXPECT_EQ(f.state(),before);
        EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
        if(failure==1){EXPECT_TRUE(commit.called);EXPECT_TRUE(commit.old);}
        if(failure==0)Sql(f.raw(),"DROP TRIGGER refuse_reference");
        ASSERT_EQ(f.run(),1u);const auto rows=f.manager->getDatabase().getPayoutsForBlock(1);
        ASSERT_EQ(rows.size(),2u);EXPECT_TRUE(rows[0].allocation_origin);EXPECT_TRUE(rows[1].allocation_origin);
    }
}
TEST(PoolAllocationOrigins, LegacySchemaAndCompatibilityInsertDoNotEnroll) {
    Fixture f;Payout old;old.block_id=1;old.worker_id="a";old.wallet_address="address-a";old.amount=4;old.calculated_at=10;
    ASSERT_TRUE(f.manager->getDatabase().insertPayout(old));
    const auto before=Read(f.raw(),"SELECT payout_id,block_id,worker_id,wallet_address,amount,status FROM payouts");
    Sql(f.raw(),"DROP INDEX idx_payouts_allocation_origin; ALTER TABLE payouts DROP COLUMN allocation_origin");
    f.manager.reset();f.open();
    EXPECT_EQ(Read(f.raw(),"SELECT payout_id,block_id,worker_id,wallet_address,amount,status FROM payouts"),before);
    const auto rows=f.manager->getDatabase().getPayoutsForBlock(1);ASSERT_EQ(rows.size(),1u);EXPECT_FALSE(rows[0].allocation_origin);
    EXPECT_EQ(Read(f.raw(),"SELECT allocation_origin FROM payouts"),(Rows{{"5:"}}));
    auto state=f.state();EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),state);
    std::array<uint8_t,32> supplied{};supplied[0]=1;old.allocation_origin=supplied;
    EXPECT_FALSE(f.manager->getDatabase().insertPayout(old));EXPECT_EQ(f.state(),state);
    // Reopen never fabricates an origin for a present predecessor payout.
    f.manager.reset();f.open();EXPECT_EQ(f.state(),state);
    EXPECT_FALSE(f.manager->getDatabase().getPayoutsForBlock(1).at(0).allocation_origin);
}
TEST(PoolAllocationOrigins, MalformedAndIncompleteReferenceReadsRefuse) {
    for(const auto* value:{"zeroblob(32)","zeroblob(33)","X'01'","'not-a-blob'"}) {
        Fixture f;ASSERT_EQ(f.run(),1u);
        Sql(f.raw(),std::string("UPDATE payouts SET allocation_origin=")+value+" WHERE worker_id='b'");
        auto& db=f.manager->getDatabase();const auto before=f.state();
        EXPECT_THROW(db.getPayoutsForBlock(1),std::runtime_error);
        EXPECT_THROW(db.getPayoutsReadyToSend(),std::runtime_error);
        EXPECT_THROW(db.getWorkerPayouts("b"),std::runtime_error);
        EXPECT_EQ(f.state(),before);
        Sql(f.raw(),"UPDATE payouts SET status=3 WHERE worker_id='b'");const auto failed=f.state();
        EXPECT_THROW(db.getPendingPayouts(),std::runtime_error);EXPECT_EQ(f.state(),failed);
    }
    Fixture f;ASSERT_EQ(f.run(),1u);auto& db=f.manager->getDatabase();const auto before=f.state();
    sqlite3_set_authorizer(f.raw(),[](void*,int action,const char* table,const char* column,const char*,const char*) {
        return action==SQLITE_READ && table && column && std::strcmp(table,"payouts")==0 && std::strcmp(column,"allocation_origin")==0?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(db.getPayoutsForBlock(1),std::runtime_error);
    sqlite3_set_authorizer(f.raw(),nullptr,nullptr);EXPECT_EQ(f.state(),before);
    struct Interrupt {sqlite3* db;int rows=0;};Interrupt interrupt{f.raw()};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* raw,void* statement,void*) {
        auto& i=*static_cast<Interrupt*>(raw);const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(sql && std::strstr(sql,"allocation_origin FROM payouts WHERE block_id") && ++i.rows==2)sqlite3_interrupt(i.db);
        return 0;
    },&interrupt);
    EXPECT_THROW(db.getPayoutsForBlock(1),std::runtime_error);
    sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_EQ(interrupt.rows,2);EXPECT_EQ(f.state(),before);
    EXPECT_EQ(db.getPayoutsForBlock(1).size(),2u);EXPECT_EQ(f.state(),before);
}
} // namespace pool_allocation_origin_checks
