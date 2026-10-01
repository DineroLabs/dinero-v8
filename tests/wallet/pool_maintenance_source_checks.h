#pragma once
#include "pool/canonical_maintenance.h"
namespace dinero::pool {
struct PoolMaintenanceSourceTestAccess {
    static void Check(PoolManager& m){m.checkBlockConfirmations();}
    static uint32_t Allocate(PoolManager& m,const std::vector<PoolBlock>& captured){return m.db_->allocateConfirmedBlockPayouts(*m.calculator_,&captured);}
};
}
namespace {
class PoolMaintenanceSource : public PoolPaymentAttempt {
protected:
    using Access=dinero::pool::PoolMaintenanceSourceTestAccess;
    void reconcile(){dinero::pool::CanonicalPoolMaintenance::Reconcile(chain,*pool);}
    auto blocks(){return pool->getDatabase().getRecordedBlocks();}
    void no_wallet_effects(){EXPECT_TRUE(attempts().empty());EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);}
    void second_block() {
        dinero::pool::PoolBlock b;b.block_hash=canonical->blocks[2].GetHash().GetHex();b.height=2;
        b.finder_worker="miner";b.finder_address=modern_address;b.reward=b.total_reward=b.distributable=100000;
        b.required_confirmations=100;b.found_at=21;
        ASSERT_TRUE(pool->getDatabase().insertBlock(b));
        sql(raw(),"UPDATE blocks SET confirmations=0,required_confirmations=100,pool_fee_percent=0,pool_fee_amount=0,distributable=100000 WHERE height=2");
    }
};
TEST_F(PoolMaintenanceSource, ActualPeriodicSourceMaturesNewBlockAndPreservesExistingAllocation) {
    second_block();const auto original=pool->getDatabase().getPayoutsForBlock(1);ASSERT_EQ(original.size(),1u);
    auto cfg=pool->getConfig();cfg.payment_funding.reset();ASSERT_TRUE(pool->setConfig(cfg));
    pool->setChainstateSource(chain);pool->runMaintenance();
    auto rows=blocks();ASSERT_EQ(rows.size(),2u);EXPECT_EQ(rows[0].confirmations,101u);EXPECT_EQ(rows[1].confirmations,100u);
    EXPECT_TRUE(rows[0].payouts_calculated);EXPECT_TRUE(rows[1].payouts_calculated);EXPECT_GT(rows[0].confirmed_at,0);EXPECT_GT(rows[1].confirmed_at,0);
    EXPECT_EQ(pool->getDatabase().getPayoutsForBlock(1)[0].allocation_origin,original[0].allocation_origin);
    auto fresh=pool->getDatabase().getPayoutsForBlock(2);ASSERT_EQ(fresh.size(),1u);ASSERT_TRUE(fresh[0].allocation_origin);
    const auto reference=fresh[0].allocation_origin;EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,120000u);
    pool->runMaintenance();EXPECT_EQ(blocks(),rows);EXPECT_EQ(pool->getDatabase().getPayoutsForBlock(2)[0].allocation_origin,reference);
    EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,120000u);no_wallet_effects();
}
TEST_F(PoolMaintenanceSource, MissingStoppedAndInconsistentSourcesNeverOrphanMatureBlocks) {
    const auto before=blocks();
    EXPECT_THROW(Access::Check(*pool),std::runtime_error);
    pool->setChainstateSource(chain);const auto tip=canonical->db.getTip();ASSERT_TRUE(tip.ok());auto wrong=tip->hash;wrong.begin()[0]^=1;
    ASSERT_EQ(canonical->db.setTip(canonical->token,wrong,tip->height,tip->work),dinero::Status::Ok);
    EXPECT_THROW(Access::Check(*pool),std::runtime_error);
    EXPECT_EQ(blocks(),before);unpaid();ASSERT_EQ(canonical->db.setTip(canonical->token,tip->hash,tip->height,tip->work),dinero::Status::Ok);
    canonical->db.close();
    EXPECT_THROW(Access::Check(*pool),std::runtime_error);
    EXPECT_EQ(blocks(),before);unpaid();ASSERT_EQ(canonical->db.init(canonical->root/"chain"),dinero::Status::Ok);
    chain->Stop();
    EXPECT_THROW(Access::Check(*pool),std::runtime_error);
    EXPECT_EQ(blocks(),before);unpaid();no_wallet_effects();
}
TEST_F(PoolMaintenanceSource, CompleteInventoryAndLateBodyValidationPrecedeEveryConfirmationWrite) {
    second_block();const auto before=blocks();
    for(const auto& column:std::vector<std::string>{"height","total_reward"}) {
        sql(raw(),"UPDATE blocks SET "+column+"="+column+"+1 WHERE block_id=2");const auto changed=blocks();
        EXPECT_THROW(reconcile(),std::runtime_error);
        EXPECT_EQ(blocks(),changed);sql(raw(),"UPDATE blocks SET "+column+"="+column+"-1 WHERE block_id=2");
    }
    sql(raw(),"UPDATE blocks SET block_hash=CAST(block_hash AS BLOB) WHERE block_id=2");
    EXPECT_THROW(reconcile(),std::runtime_error);
    sql(raw(),"UPDATE blocks SET block_hash=CAST(block_hash AS TEXT) WHERE block_id=2");EXPECT_EQ(blocks(),before);
    struct Interrupt {sqlite3* db;bool hit=false;};Interrupt interrupt{raw()};
    sqlite3_trace_v2(raw(),SQLITE_TRACE_ROW,[](unsigned,void* p,void* s,void*) {
        auto& i=*static_cast<Interrupt*>(p);const auto* query=sqlite3_sql(static_cast<sqlite3_stmt*>(s));
        if(query && std::strstr(query,"FROM blocks ORDER BY block_id")){i.hit=true;sqlite3_interrupt(i.db);}return 0;
    },&interrupt);
    EXPECT_THROW(reconcile(),std::runtime_error);
    sqlite3_trace_v2(raw(),0,nullptr,nullptr);EXPECT_TRUE(interrupt.hit);EXPECT_EQ(blocks(),before);
    sqlite3_set_authorizer(raw(),[](void*,int a,const char* t,const char*,const char*,const char*){return a==SQLITE_READ && t && std::strcmp(t,"blocks")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(reconcile(),std::runtime_error);
    sqlite3_set_authorizer(raw(),nullptr,nullptr);EXPECT_EQ(blocks(),before);unpaid();no_wallet_effects();
    EXPECT_NO_THROW(reconcile());EXPECT_EQ(blocks()[1].confirmations,100u);
}
TEST_F(PoolMaintenanceSource, CheckedConfirmationWritesCommitAndCapturedOwnerPreserveFlags) {
    auto& db=pool->getDatabase();const auto before=blocks();ASSERT_EQ(before.size(),1u);
    sql(raw(),"CREATE TRIGGER refuse_confirmation BEFORE UPDATE OF confirmations ON blocks BEGIN SELECT RAISE(ABORT,'fixture confirmation refusal'); END");
    EXPECT_THROW(db.updateBlockConfirmationsChecked(before[0],101,123),std::runtime_error);
    sql(raw(),"DROP TRIGGER refuse_confirmation");EXPECT_EQ(blocks(),before);
    unsigned commits=0;sqlite3_commit_hook(raw(),[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(db.updateBlockConfirmationsChecked(before[0],101,123),std::runtime_error);
    sqlite3_commit_hook(raw(),nullptr,nullptr);EXPECT_EQ(commits,1u);EXPECT_EQ(blocks(),before);
    sql(raw(),"BEGIN IMMEDIATE");
    EXPECT_THROW(db.updateBlockConfirmationsChecked(before[0],101,123),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(raw()));sql(raw(),"ROLLBACK");
    sql(raw(),"UPDATE blocks SET payouts_sent=1");const auto changed=blocks();
    EXPECT_THROW(db.updateBlockConfirmationsChecked(before[0],101,123),std::runtime_error);
    EXPECT_EQ(blocks(),changed);sql(raw(),"UPDATE blocks SET payouts_sent=0");
    db.updateBlockConfirmationsChecked(before[0],101,123);auto expected=before[0];expected.confirmations=101;expected.confirmed_at=123;
    EXPECT_EQ(blocks()[0],expected);unpaid();no_wallet_effects();
}
TEST_F(PoolMaintenanceSource, AllocationTransactionRechecksExactEligibleSourceRows) {
    second_block();auto captured=blocks();ASSERT_EQ(captured.size(),2u);
    // Direct DB comparison component: these sequential snapshots are not
    // advertised as chain authority or a concurrent/multiprocess race test.
    sql(raw(),"UPDATE blocks SET confirmations=100 WHERE block_id=2");
    EXPECT_THROW(Access::Allocate(*pool,captured),std::runtime_error);
    EXPECT_TRUE(pool->getDatabase().getPayoutsForBlock(2).empty());unpaid();
    captured=blocks();sql(raw(),"UPDATE blocks SET finder_address='different' WHERE block_id=2");
    EXPECT_THROW(Access::Allocate(*pool,captured),std::runtime_error);
    EXPECT_TRUE(pool->getDatabase().getPayoutsForBlock(2).empty());unpaid();
    sql(raw(),"UPDATE blocks SET finder_address='"+modern_address+"' WHERE block_id=2");
    EXPECT_NO_THROW(reconcile());EXPECT_EQ(pool->getDatabase().getPayoutsForBlock(2).size(),1u);no_wallet_effects();
}
TEST_F(PoolMaintenanceSource, CanonicalPreviouslyOrphanedRecordRefusesCreditRecreation) {
    // Retain an explicitly synthetic alternate header/body in the actual
    // archive. It is not independently validated or claimed to be PoW-valid;
    // the selected 101-block source remains the actual validated fixture.
    auto fork=canonical->blocks[1];++fork.header.timestamp;const auto hash=fork.GetHash();
    auto metadata=canonical->db.getHeaderMetadata(canonical->blocks[1].GetHash());ASSERT_TRUE(metadata.ok());
    ASSERT_EQ(canonical->db.putHeader(canonical->token,hash,fork.header,1,metadata->chainwork),dinero::Status::Ok);
    ASSERT_EQ(canonical->db.putBlock(canonical->token,hash,fork),dinero::Status::Ok);
    const auto position=canonical->files->writeBlock(hash,fork);ASSERT_TRUE(position.ok());
    metadata->status_flags=dinero::BLOCK_HAVE_DATA;metadata->file_number=position->file_number;metadata->data_pos=position->offset;metadata->data_size=position->size;
    ASSERT_EQ(canonical->db.putHeaderMetadata(canonical->token,hash,*metadata),dinero::Status::Ok);
    sql(raw(),"UPDATE blocks SET block_hash='"+hash.GetHex()+"'");
    EXPECT_NO_THROW(reconcile());ASSERT_TRUE(blocks()[0].orphaned);
    EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,0u);
    const auto orphaned=blocks();EXPECT_NO_THROW(reconcile());EXPECT_EQ(blocks(),orphaned);
    // A sequential recorded-identity edit is used only to exercise refusal of
    // a canonical record carrying the old orphaned accounting state.
    sql(raw(),"UPDATE blocks SET block_hash='"+source_hash+"'");
    const auto before=blocks();ASSERT_TRUE(before[0].orphaned);const auto payouts=pool->getDatabase().getPayoutsForBlock(1);
    ASSERT_EQ(payouts.size(),1u);const auto credit=pool->getDatabase().getWorker("miner")->pending_payout;
    EXPECT_THROW(reconcile(),std::runtime_error);
    EXPECT_EQ(blocks(),before);EXPECT_EQ(pool->getDatabase().getPayoutsForBlock(1)[0].status,payouts[0].status);
    EXPECT_EQ(pool->getDatabase().getPayoutsForBlock(1)[0].allocation_origin,payouts[0].allocation_origin);
    EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,credit);no_wallet_effects();
}
} // namespace
