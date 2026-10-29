#pragma once
#include "pool/payout_calculator.h"
#include "pool_orphan_accounting_checks.h"

namespace pool_round_owner_checks {
using namespace dinero::pool;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
struct Fixture : pool_orphan_accounting_checks::Fixture {
    PoolConfig config;
    Fixture() {
        config.payout_mode=PayoutMode::PROP;config.pool_fee_percent=0;
        Sql(raw(),"DELETE FROM round_shares; DELETE FROM rounds");
        Sql(raw(),"INSERT INTO rounds(round_id,block_id,total_shares,total_difficulty,started_at,ended_at) VALUES(1,99,1,20,10,20),(29,1,2,4,10,20),(100,0,0,0,10,0)");
        Sql(raw(),"INSERT INTO round_shares(round_id,worker_id,difficulty_sum,share_count) VALUES(1,'c',20,1),(29,'a',1,1),(29,'b',3,1)");
        Sql(raw(),"UPDATE blocks SET total_reward=10000,confirmations=100,required_confirmations=100,payouts_calculated=0");
    }
    Rows state() {auto result=pool_orphan_accounting_checks::Fixture::state();for(const auto* table:{"rounds","round_shares"}){auto rows=Read(raw(),std::string("SELECT * FROM ")+table+" ORDER BY 1,2");result.insert(result.end(),rows.begin(),rows.end());}return result;}
    PoolBlock block() {auto result=manager->getDatabase().getBlock(1);if(!result)throw std::runtime_error("fixture block missing");return *result;}
    auto calculate(const PoolBlock& value) {return PayoutCalculator(manager->getDatabase(),config).calculatePROP(value);}
};
TEST(PoolRoundOwner, RecordedRoundWinsOverUnrelatedMatchingBlockNumber) {
    Fixture f;const auto before=f.state();const auto payments=f.calculate(f.block());
    ASSERT_EQ(payments.size(),2u);EXPECT_EQ(payments[0].worker_id,"a");EXPECT_EQ(payments[0].amount,2500u);
    EXPECT_EQ(payments[1].worker_id,"b");EXPECT_EQ(payments[1].amount,7500u);
    for(const auto& row:payments){EXPECT_EQ(row.block_id,1u);EXPECT_EQ(row.wallet_address,"address-"+row.worker_id);}
    const auto captured=f.manager->getDatabase().getRoundForBlock(1);ASSERT_TRUE(captured);EXPECT_EQ(captured->round_id,29u);
    EXPECT_EQ(f.state(),before);f.manager.reset();f.open();const auto again=f.calculate(f.block());
    ASSERT_EQ(again.size(),2u);EXPECT_EQ(again[0].amount,2500u);EXPECT_EQ(again[1].amount,7500u);EXPECT_EQ(f.state(),before);
}
TEST(PoolRoundOwner, MissingForeignAndDuplicateRoundRefuseBeforeAllocationWrites) {
    Fixture f;const auto before=f.state();
    for(uint64_t id:{uint64_t{0},uint64_t{87}}) {
        auto block=f.block();block.block_id=id;EXPECT_THROW(f.calculate(block),std::runtime_error);EXPECT_EQ(f.state(),before);
    }
    Sql(f.raw(),"UPDATE rounds SET block_id=2 WHERE round_id=29");const auto altered=f.state();
    PayoutCalculator calc(f.manager->getDatabase(),f.config);EXPECT_THROW(calc.processConfirmedBlocks(),std::runtime_error);
    EXPECT_EQ(f.state(),altered);EXPECT_EQ(Read(f.raw(),"SELECT payouts_calculated FROM blocks"),(Rows{{"1:0"}}));
    Sql(f.raw(),"UPDATE rounds SET block_id=1 WHERE round_id IN (1,29)");const auto duplicate=f.state();
    EXPECT_THROW(f.manager->getDatabase().getRoundForBlock(1),std::runtime_error);
    EXPECT_THROW(calc.processConfirmedBlocks(),std::runtime_error);EXPECT_EQ(f.state(),duplicate);
}
TEST(PoolRoundOwner, CompleteTypedContributionsAndReadErrors) {
    for(const auto* change:{"UPDATE round_shares SET difficulty_sum=-1 WHERE worker_id='b'","UPDATE round_shares SET difficulty_sum='invalid' WHERE worker_id='b'","UPDATE round_shares SET worker_id=CAST(X'620063' AS TEXT) WHERE worker_id='b'","UPDATE rounds SET total_difficulty='invalid' WHERE round_id=29","UPDATE rounds SET block_id=NULL WHERE round_id=29"}) {
        Fixture f;Sql(f.raw(),change);const auto before=f.state();EXPECT_THROW(f.manager->getDatabase().getRound(29),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
    }
    Fixture f;const auto before=f.state();
    sqlite3_set_authorizer(f.raw(),[](void*,int action,const char* table,const char*,const char*,const char*) {return action==SQLITE_READ&&table&&std::strcmp(table,"round_shares")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(f.manager->getDatabase().getRound(29),std::runtime_error);sqlite3_set_authorizer(f.raw(),nullptr,nullptr);
    struct Interrupt{sqlite3* db;int rows=0;};Interrupt interrupted{f.raw()};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* ctx,void* stmt,void*) {auto& i=*static_cast<Interrupt*>(ctx);auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(sql&&std::strstr(sql,"FROM round_shares")&&++i.rows==2)sqlite3_interrupt(i.db);return 0;},&interrupted);
    EXPECT_THROW(f.manager->getDatabase().getRound(29),std::runtime_error);sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);
    EXPECT_EQ(interrupted.rows,2);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));EXPECT_EQ(f.state(),before);
    const auto valid=f.manager->getDatabase().getRound(29);ASSERT_TRUE(valid);EXPECT_EQ(valid->worker_difficulty.size(),2u);
    EXPECT_FALSE(f.manager->getDatabase().getRound(87));
    EXPECT_FALSE(f.manager->getDatabase().getRoundForBlock(87));
    bool association_interrupted=false;
    struct Lookup {sqlite3* db;bool* hit;};Lookup lookup{f.raw(),&association_interrupted};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* ctx,void* stmt,void*) {
        auto& l=*static_cast<Lookup*>(ctx);auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));
        if(sql&&std::strstr(sql,"SELECT round_id FROM rounds WHERE block_id")){*l.hit=true;sqlite3_interrupt(l.db);}return 0;
    },&lookup);
    EXPECT_THROW(f.manager->getDatabase().getRoundForBlock(1),std::runtime_error);sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);
    EXPECT_TRUE(association_interrupted);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));EXPECT_EQ(f.state(),before);
    EXPECT_TRUE(f.manager->getDatabase().getRoundForBlock(1));
}
TEST(PoolRoundOwner, CallerTransactionAndCommitRefusalPreserveOwners) {
    Fixture f;auto& db=f.manager->getDatabase();const auto before=f.state();
    Sql(f.raw(),"BEGIN IMMEDIATE; UPDATE round_shares SET difficulty_sum=2 WHERE round_id=29 AND worker_id='a'");
    const auto borrowed=db.getRoundForBlock(1);ASSERT_TRUE(borrowed);EXPECT_EQ(borrowed->worker_difficulty.at("a"),2.0);EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));
    Sql(f.raw(),"ROLLBACK");EXPECT_EQ(db.getRound(29)->worker_difficulty.at("a"),1.0);
    bool refused=false;sqlite3_set_authorizer(f.raw(),[](void* raw,int action,const char* name,const char*,const char*,const char*) {if(action==SQLITE_TRANSACTION&&name&&std::strcmp(name,"COMMIT")==0){*static_cast<bool*>(raw)=true;return SQLITE_DENY;}return SQLITE_OK;},&refused);
    EXPECT_THROW(db.getRoundForBlock(1),std::runtime_error);sqlite3_set_authorizer(f.raw(),nullptr,nullptr);
    EXPECT_TRUE(refused);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));EXPECT_EQ(f.state(),before);EXPECT_TRUE(db.getRound(29));
}
} // namespace pool_round_owner_checks
