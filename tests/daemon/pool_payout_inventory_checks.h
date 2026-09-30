#pragma once
#include "pool_payment_lifetime_checks.h"
namespace pool_payout_inventory_checks {
using namespace dinero::pool;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
struct Fixture : pool_calculation_inputs_checks::Fixture {
    Fixture() {if(run()!=1)throw std::runtime_error("fixture allocation");}
    std::vector<Payout> selected(int selection) {
        auto& db=manager->getDatabase();
        if(selection==0)return db.getPayoutsReadyToSend();
        if(selection==1)return db.getPendingPayouts();
        if(selection==2)return db.getWorkerPayouts("a",10);
        return db.getPayoutsForBlock(1);
    }
    void selection(int selection) {
        Sql(raw(),"UPDATE payouts SET status="+std::to_string(selection==1?0:1)+",amount=100,calculated_at=CASE worker_id WHEN 'a' THEN 10 ELSE 20 END");
        if(selection==2)Sql(raw(),"UPDATE payouts SET calculated_at=30-calculated_at,worker_id='a'");
    }
};
TEST(PoolPayoutInventory, ExactColumnsSelectionAndReopen) {
    Fixture f;auto& db=f.manager->getDatabase();const auto initial=f.state();
    auto ready=db.getPayoutsReadyToSend();ASSERT_EQ(ready.size(),2u);EXPECT_EQ(ready[0].status,PayoutStatus::CONFIRMED);
    EXPECT_TRUE(db.getPendingPayouts().empty());auto worker=db.getWorkerPayouts("a",10);ASSERT_EQ(worker.size(),1u);EXPECT_EQ(worker[0].amount,2500u);
    const auto block=db.getPayoutsForBlock(1);ASSERT_EQ(block.size(),2u);EXPECT_EQ(block[0].amount,7500u);EXPECT_EQ(block[1].amount,2500u);
    EXPECT_TRUE(db.getWorkerPayouts("a",0).empty());EXPECT_TRUE(db.getPayoutsForBlock(99).empty());EXPECT_EQ(f.state(),initial);
    Sql(f.raw(),"ALTER TABLE payouts ADD COLUMN unrelated_display TEXT; UPDATE payouts SET status=3,txid=NULL,error_message=NULL,retry_count=4294967295,last_retry_at=42 WHERE worker_id='b'");
    auto before=f.state();auto pending=db.getPendingPayouts();ASSERT_EQ(pending.size(),1u);EXPECT_EQ(pending[0].worker_id,"b");EXPECT_EQ(pending[0].retry_count,UINT32_MAX);EXPECT_EQ(pending[0].last_retry_at,42);EXPECT_TRUE(pending[0].txid.empty());EXPECT_TRUE(pending[0].error_message.empty());
    // The existing Payout representation documents zero block ID for PPS.
    // Seed that historical shape explicitly; it is not canonical eligibility.
    Sql(f.raw(),"PRAGMA foreign_keys=OFF; UPDATE payouts SET block_id=0 WHERE worker_id='b'; PRAGMA foreign_keys=ON");
    before=f.state();const auto pps=db.getPayoutsForBlock(0);ASSERT_EQ(pps.size(),1u);EXPECT_EQ(pps[0].block_id,0u);EXPECT_EQ(pps[0].worker_id,"b");
    EXPECT_EQ(db.getPayoutsReadyToSend().size(),1u);EXPECT_EQ(db.getWorkerPayouts("b",UINT32_MAX).size(),1u);EXPECT_EQ(f.state(),before);
    f.manager.reset();f.open();EXPECT_EQ(f.state(),before);EXPECT_EQ(f.manager->getDatabase().getPendingPayouts()[0].retry_count,UINT32_MAX);EXPECT_EQ(f.manager->getDatabase().getPayoutsForBlock(0).size(),1u);
}
TEST(PoolPayoutInventory, MalformedLaterRowsRefuseAllSelectedViews) {
    for(int selection=0;selection<4;++selection) {
        for(const auto* change:{"amount=-1","amount='invalid'","wallet_address=X'61'","wallet_address=CAST(X'610062' AS TEXT)","share_percent=1e999","difficulty_sum=-1","retry_count=4294967296","paid_at=NULL","last_retry_at=-1","txid=X'61'","error_message=CAST(X'610062' AS TEXT)"}) {
            Fixture f;f.selection(selection);Sql(f.raw(),std::string("UPDATE payouts SET ")+change+" WHERE payout_id=(SELECT MAX(payout_id) FROM payouts)");const auto before=f.state();
            EXPECT_THROW(f.selected(selection),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
        }
    }
    Fixture f;Sql(f.raw(),"UPDATE payouts SET status=7 WHERE worker_id='b'");const auto before=f.state();
    EXPECT_THROW(f.manager->getDatabase().getPayoutsForBlock(1),std::runtime_error);EXPECT_EQ(f.state(),before);
    EXPECT_THROW(f.manager->getDatabase().getPayoutsForBlock(UINT64_MAX),std::runtime_error);
    EXPECT_THROW(f.manager->getDatabase().getWorkerPayouts(std::string("a\0b",3),10),std::runtime_error);
}
TEST(PoolPayoutInventory, ReadAndEofFailureBeforeAnyCallbackOrWrite) {
    for(int selection=0;selection<4;++selection) {
        Fixture f;f.selection(selection);const auto before=f.state();unsigned calls=0;
        f.manager->setPaymentCallback([&](const std::string&,uint64_t,std::string&){++calls;return false;});
        sqlite3_set_authorizer(f.raw(),[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::strcmp(table,"payouts")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
        EXPECT_THROW(f.selected(selection),std::runtime_error);
        if(selection==0)EXPECT_THROW(f.manager->sendPendingPayouts(),std::runtime_error);
        if(selection==1)EXPECT_THROW(f.manager->retryFailedPayouts(3),std::runtime_error);
        sqlite3_set_authorizer(f.raw(),nullptr,nullptr);EXPECT_EQ(calls,0u);EXPECT_EQ(f.state(),before);
        struct Interrupted {sqlite3* db;int rows=0;};Interrupted interrupted{f.raw()};
        sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* context,void* statement,void*){auto& i=*static_cast<Interrupted*>(context);const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));if(sql&&std::strstr(sql,"FROM payouts WHERE")&&++i.rows==2)sqlite3_interrupt(i.db);return 0;},&interrupted);
        EXPECT_THROW(f.selected(selection),std::runtime_error);sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_EQ(interrupted.rows,2);EXPECT_EQ(f.state(),before);EXPECT_EQ(calls,0u);EXPECT_EQ(f.selected(selection).size(),2u);
    }
}
TEST(PoolPayoutInventory, BorrowedReadsAndWholeBatchAmountPreflight) {
    Fixture f;const auto before=f.state();Sql(f.raw(),"BEGIN IMMEDIATE; UPDATE payouts SET amount=123 WHERE worker_id='a'");
    auto& db=f.manager->getDatabase();EXPECT_EQ(db.getWorkerPayouts("a",10)[0].amount,123u);EXPECT_EQ(db.getPayoutsForBlock(1).size(),2u);EXPECT_EQ(db.getPayoutsReadyToSend().size(),2u);EXPECT_TRUE(db.getPendingPayouts().empty());EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));
    Sql(f.raw(),"ROLLBACK");EXPECT_EQ(f.state(),before);
    f.second();f.config.payout_mode=PayoutMode::SOLO;ASSERT_EQ(f.run(),1u);unsigned calls=0;f.manager->setPaymentCallback([&](const std::string&,uint64_t,std::string&){++calls;return false;});
    Sql(f.raw(),"UPDATE payouts SET wallet_address='address-z',amount=CASE worker_id WHEN 'b' THEN 9223372036854775807 ELSE 1 END WHERE worker_id IN ('b','c')");
    auto invalid=f.state();EXPECT_THROW(f.manager->sendPendingPayouts(),std::runtime_error);EXPECT_EQ(calls,0u);EXPECT_EQ(f.state(),invalid);
    Sql(f.raw(),"UPDATE payouts SET amount=0 WHERE worker_id='b'");invalid=f.state();EXPECT_THROW(f.manager->sendPendingPayouts(),std::runtime_error);EXPECT_EQ(calls,0u);EXPECT_EQ(f.state(),invalid);
    Sql(f.raw(),"UPDATE payouts SET amount=2 WHERE worker_id='b'");EXPECT_EQ(f.manager->sendPendingPayouts(),3u);EXPECT_EQ(calls,2u);
}
} // namespace pool_payout_inventory_checks
