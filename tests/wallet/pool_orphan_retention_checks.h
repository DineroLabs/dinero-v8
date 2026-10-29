#pragma once
namespace dinero::pool {
struct PoolOrphanRetentionTestAccess {
    static void Apply(PoolDB& db,const PoolBlock& expected,bool orphan){db.transitionCanonicalOrphan(expected,orphan);}
};
}
namespace {
class PoolOrphanRetention : public PoolPaymentAttempt {
protected:
    using Rows=std::vector<std::vector<std::string>>;
    Rows read(const std::string& query) {
        sqlite3_stmt* s=nullptr;if(sqlite3_prepare_v2(raw(),query.c_str(),-1,&s,nullptr)!=SQLITE_OK)throw std::runtime_error("retention fixture read prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owner(s,sqlite3_finalize);Rows result;int rc;
        while((rc=sqlite3_step(s))==SQLITE_ROW) {
            std::vector<std::string> row;for(int c=0;c<sqlite3_column_count(s);++c) {
                const auto type=sqlite3_column_type(s,c);const auto* data=sqlite3_column_blob(s,c);const int size=sqlite3_column_bytes(s,c);
                row.push_back(std::to_string(type)+":"+(data?std::string(static_cast<const char*>(data),size):std::string{}));
            }result.push_back(std::move(row));
        }
        if(rc!=SQLITE_DONE)throw std::runtime_error("retention fixture read incomplete");return result;
    }
    Rows state(){Rows result;for(const auto* t:{"blocks","payouts","workers","pool_orphan_blocks","pool_orphan_payouts"}){auto rows=read(std::string("SELECT * FROM ")+t+" ORDER BY 1");result.insert(result.end(),rows.begin(),rows.end());}return result;}
    dinero::pool::PoolBlock block(){return pool->getDatabase().getRecordedBlocks().at(0);}
    void apply(bool orphan){dinero::pool::PoolOrphanRetentionTestAccess::Apply(pool->getDatabase(),block(),orphan);}
    void no_wallet_effects(){EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->submits,0);}
    void reconcile(){dinero::pool::CanonicalPoolMaintenance::Reconcile(chain,*pool);}
};
TEST_F(PoolOrphanRetention, ExactRowsCreditsAndOriginsRestoreThroughActualSourceAfterReopen) {
    const auto payouts=read("SELECT * FROM payouts ORDER BY payout_id");const auto origins=read("SELECT allocation_origin FROM payouts");
    // The retained DB transition is exercised directly as a component here;
    // actual selected-source restoration follows. This setup is not a claimed
    // whole-node disconnect or a proof that this source block was noncanonical.
    apply(true);ASSERT_TRUE(block().orphaned);EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,0u);
    const auto after=state();apply(true);EXPECT_EQ(state(),after);
    ASSERT_TRUE(pool->getDatabase().addWorkerPending("miner",17));pool.reset();open_pool();
    EXPECT_NO_THROW(reconcile());EXPECT_FALSE(block().orphaned);EXPECT_EQ(block().confirmations,101u);
    EXPECT_EQ(read("SELECT * FROM payouts ORDER BY payout_id"),payouts);EXPECT_EQ(read("SELECT allocation_origin FROM payouts"),origins);
    EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,20017u);
    EXPECT_TRUE(read("SELECT * FROM pool_orphan_blocks").empty());EXPECT_TRUE(read("SELECT * FROM pool_orphan_payouts").empty());
    const auto restored=state();EXPECT_NO_THROW(reconcile());EXPECT_EQ(state(),restored);
    apply(true);apply(false);EXPECT_EQ(state(),restored);EXPECT_TRUE(attempts().empty());no_wallet_effects();
}
TEST_F(PoolOrphanRetention, SaturatingDebitAndNullableMetadataRestoreWithoutInventingCredit) {
    sql(raw(),"UPDATE workers SET pending_payout=7 WHERE worker_id='miner'");
    sql(raw(),"UPDATE payouts SET txid=NULL,error_message=NULL,retry_count=7,last_retry_at=9");
    const auto before=read("SELECT * FROM payouts");apply(true);
    EXPECT_EQ(read("SELECT balance_before,balance_after,debit FROM pool_orphan_payouts"),(Rows{{"1:7","1:0","1:7"}}));
    ASSERT_TRUE(pool->getDatabase().addWorkerPending("miner",13));apply(false);
    EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,20u);EXPECT_EQ(read("SELECT * FROM payouts"),before);
    EXPECT_EQ(read("SELECT typeof(txid),typeof(error_message) FROM payouts"),(Rows{{"3:null","3:null"}}));
    EXPECT_TRUE(attempts().empty());no_wallet_effects();
}
TEST_F(PoolOrphanRetention, RetentionWritesCommitAndBorrowedTransactionRollbackTogether) {
    const std::vector<std::pair<std::string,std::string>> triggers={{"INSERT","pool_orphan_blocks"},{"INSERT","pool_orphan_payouts"},{"UPDATE","workers"},{"UPDATE","payouts"},{"UPDATE","blocks"}};
    for(const auto& [event,table]:triggers) {
        sql(raw(),"CREATE TRIGGER refuse_retention BEFORE "+event+" ON "+table+" BEGIN SELECT RAISE(ABORT,'fixture retention refusal'); END");const auto before=state();
        EXPECT_THROW(apply(true),std::runtime_error)<<table;
        EXPECT_EQ(state(),before);sql(raw(),"DROP TRIGGER refuse_retention");
    }
    const auto before=state();const auto expected=block();unsigned commits=0;
    sqlite3_commit_hook(raw(),[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(dinero::pool::PoolOrphanRetentionTestAccess::Apply(pool->getDatabase(),expected,true),std::runtime_error);
    sqlite3_commit_hook(raw(),nullptr,nullptr);EXPECT_EQ(commits,1u);EXPECT_EQ(state(),before);
    sql(raw(),"BEGIN IMMEDIATE");
    EXPECT_THROW(dinero::pool::PoolOrphanRetentionTestAccess::Apply(pool->getDatabase(),expected,true),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(raw()));sql(raw(),"ROLLBACK");EXPECT_EQ(state(),before);
    apply(true);apply(false);EXPECT_EQ(state(),before);no_wallet_effects();
}
TEST_F(PoolOrphanRetention, RestoreWritesCommitAndMissingOwnerNeverPublishPartialCredits) {
    apply(true);const auto after=state();
    for(const auto& [event,table]:std::vector<std::pair<std::string,std::string>>{{"UPDATE","workers"},{"UPDATE","payouts"},{"UPDATE","blocks"},{"DELETE","pool_orphan_payouts"},{"DELETE","pool_orphan_blocks"}}) {
        sql(raw(),"CREATE TRIGGER refuse_restore BEFORE "+event+" ON "+table+" BEGIN SELECT RAISE(ABORT,'fixture restore refusal'); END");
        EXPECT_THROW(apply(false),std::runtime_error)<<table;
        EXPECT_EQ(state(),after);sql(raw(),"DROP TRIGGER refuse_restore");
    }
    const auto expected=block();unsigned commits=0;sqlite3_commit_hook(raw(),[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(dinero::pool::PoolOrphanRetentionTestAccess::Apply(pool->getDatabase(),expected,false),std::runtime_error);
    sqlite3_commit_hook(raw(),nullptr,nullptr);EXPECT_EQ(commits,1u);EXPECT_EQ(state(),after);
    sql(raw(),"CREATE TEMP TABLE saved_orphan AS SELECT * FROM pool_orphan_blocks");sql(raw(),"DELETE FROM pool_orphan_blocks");const auto missing=state();
    EXPECT_THROW(apply(false),std::runtime_error);
    EXPECT_EQ(state(),missing);sql(raw(),"INSERT INTO pool_orphan_blocks SELECT * FROM saved_orphan");sql(raw(),"DROP TABLE saved_orphan");EXPECT_EQ(state(),after);
    apply(false);EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,20000u);no_wallet_effects();
}
TEST_F(PoolOrphanRetention, CompleteRetainedPostimageAndDebitReadsRefuseBeforeRestoration) {
    // A second legacy failed row is explicit component metadata, never a new
    // authenticated allocation reference or a recreated payout.
    sql(raw(),"INSERT INTO payouts(block_id,worker_id,wallet_address,amount,share_percent,share_count,difficulty_sum,status,txid,error_message,calculated_at,paid_at,retry_count,last_retry_at) SELECT block_id,worker_id,wallet_address,100,share_percent,share_count,difficulty_sum,3,'','prior-failure',calculated_at,0,0,0 FROM payouts WHERE payout_id=1");
    apply(true);const auto after=state();
    const std::vector<std::pair<std::string,std::string>> changes={
        {"UPDATE pool_orphan_payouts SET debit=-1 WHERE payout_id=1","UPDATE pool_orphan_payouts SET debit=20000 WHERE payout_id=1"},
        {"UPDATE pool_orphan_payouts SET balance_after=1 WHERE payout_id=1","UPDATE pool_orphan_payouts SET balance_after=0 WHERE payout_id=1"},
        {"UPDATE pool_orphan_payouts SET amount=101 WHERE payout_id=2","UPDATE pool_orphan_payouts SET amount=100 WHERE payout_id=2"},
        {"UPDATE payouts SET error_message='changed' WHERE payout_id=2","UPDATE payouts SET error_message='prior-failure' WHERE payout_id=2"},
        {"UPDATE pool_orphan_blocks SET payout_count=3","UPDATE pool_orphan_blocks SET payout_count=2"},
        {"UPDATE pool_orphan_payouts SET worker_wallet='foreign' WHERE payout_id=2","UPDATE pool_orphan_payouts SET worker_wallet='"+modern_address+"' WHERE payout_id=2"}};
    for(const auto& [change,restore]:changes){sql(raw(),change);const auto changed=state();
        EXPECT_THROW(apply(false),std::runtime_error)<<change;
        EXPECT_EQ(state(),changed);sql(raw(),restore);EXPECT_EQ(state(),after);
    }
    const auto expected=block();struct Interrupt {sqlite3* db;bool hit=false;};Interrupt interrupt{raw()};
    sqlite3_trace_v2(raw(),SQLITE_TRACE_ROW,[](unsigned,void* p,void* statement,void*) {
        auto& i=*static_cast<Interrupt*>(p);const auto* query=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(query && std::strstr(query,"FROM pool_orphan_payouts WHERE block_id=? ORDER BY")){i.hit=true;sqlite3_interrupt(i.db);}return 0;
    },&interrupt);
    EXPECT_THROW(dinero::pool::PoolOrphanRetentionTestAccess::Apply(pool->getDatabase(),expected,false),std::runtime_error);
    sqlite3_trace_v2(raw(),0,nullptr,nullptr);EXPECT_TRUE(interrupt.hit);EXPECT_EQ(state(),after);
    sqlite3_set_authorizer(raw(),[](void*,int a,const char* table,const char*,const char*,const char*){return a==SQLITE_READ && table && std::strcmp(table,"pool_orphan_payouts")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(apply(false),std::runtime_error);
    sqlite3_set_authorizer(raw(),nullptr,nullptr);EXPECT_EQ(state(),after);
    apply(false);EXPECT_EQ(pool->getDatabase().getWorker("miner")->pending_payout,20000u);EXPECT_EQ(read("SELECT status,error_message FROM payouts WHERE payout_id=2"),(Rows{{"1:3","3:prior-failure"}}));no_wallet_effects();
}
TEST_F(PoolOrphanRetention, PaidUncertainAttemptedAndUnretainedLegacyOwnersRefuse) {
    for(const auto& [change,restore]:std::vector<std::pair<std::string,std::string>>{
        {"UPDATE payouts SET status=2","UPDATE payouts SET status=1"},
        {"UPDATE payouts SET txid='uncertain'","UPDATE payouts SET txid=''"},
        {"UPDATE payouts SET paid_at=1","UPDATE payouts SET paid_at=0"}}) {
        sql(raw(),change);const auto before=state();
        EXPECT_THROW(apply(true),std::runtime_error);
        EXPECT_EQ(state(),before);sql(raw(),restore);
    }
    sql(raw(),"UPDATE blocks SET orphaned=1");const auto legacy=state();
    EXPECT_THROW(reconcile(),std::runtime_error);
    EXPECT_EQ(state(),legacy);sql(raw(),"UPDATE blocks SET orphaned=0");
    EXPECT_EQ(pool->sendPendingPayouts(),0u);const auto attempt=attempts();ASSERT_EQ(attempt.size(),1u);const auto before=state();
    EXPECT_THROW(apply(true),std::runtime_error);
    EXPECT_EQ(state(),before);EXPECT_EQ(attempts(),attempt);no_wallet_effects();
}
} // namespace
