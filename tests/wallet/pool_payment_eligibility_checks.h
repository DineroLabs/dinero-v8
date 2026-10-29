#pragma once
#include "pool/canonical_payment.h"
namespace dinero::pool {
struct PoolAttemptSourceTestAccess {
    static PoolPaymentSourceSnapshot Capture(PoolDB& db,const std::vector<uint64_t>& ids){return db.capturePaymentSources(ids);}
    static PoolPaymentAttempt BeginCaptured(PoolDB& db,const PoolPaymentWalletBinding& b,const std::vector<uint64_t>& ids,const PoolPaymentSourceSnapshot& s){return db.beginPaymentAttempt(b,ids,s);}
};
}
namespace {
class PoolAttemptSource : public PoolPaymentAttempt {
protected:
    void untouched() {
        EXPECT_TRUE(attempts().empty());EXPECT_TRUE(service->get().getPendingPayments().empty());
        EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);unpaid();
    }
    dinero::pool::PoolPaymentWalletBinding database_fixture_binding() {
        // Only for the DB comparison/COMMIT component below; never passed to a
        // wallet dispatcher or treated as authenticated real wallet identity.
        dinero::pool::PoolPaymentWalletBinding b;b.funding=*pool->getConfig().payment_funding;b.wallet.fill(37);b.network=2;
        const auto h=dinero::uint256::FromHexUnsafe(dinero::Params().genesis_hash);std::copy(h.begin(),h.end(),b.genesis.begin());return b;
    }
};
TEST_F(PoolAttemptSource, MissingInconsistentAndStoppedSourcePreserveUnclaimedAllocations) {
    daemon.chainstate.reset();
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    untouched();daemon.chainstate=chain;
    const auto tip=canonical->db.getTip();ASSERT_TRUE(tip.ok());auto wrong=tip->hash;wrong.begin()[0]^=1;
    ASSERT_EQ(canonical->db.setTip(canonical->token,wrong,tip->height,tip->work),dinero::Status::Ok);
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    untouched();ASSERT_EQ(canonical->db.setTip(canonical->token,tip->hash,tip->height,tip->work),dinero::Status::Ok);
    chain->Stop();
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    untouched();
}
TEST_F(PoolAttemptSource, StoredConfirmationsCannotReplaceCanonicalDepthHashOrReward) {
    const auto cfg=pool->getConfig();
    const std::vector<std::pair<std::string,std::string>> changes={
        {"UPDATE blocks SET height=101","UPDATE blocks SET height=1"},
        {"UPDATE blocks SET block_hash='"+std::string(64,'b')+"'","UPDATE blocks SET block_hash='"+source_hash+"'"},
        {"UPDATE blocks SET total_reward=20001","UPDATE blocks SET total_reward=20000"},
        {"UPDATE blocks SET required_confirmations=102,confirmations=100000","UPDATE blocks SET required_confirmations=100,confirmations=100"}};
    for(const auto& [change,restore]:changes) {
        sql(raw(),change);
        EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error)<<change;
        untouched();sql(raw(),restore);
    }
    auto raised=cfg;raised.required_confirmations=102;ASSERT_TRUE(pool->setConfig(raised));
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    untouched();ASSERT_TRUE(pool->setConfig(cfg));
    healthy_preflight();ingress->submit=[&](const dinero::Transaction& tx){submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    ASSERT_EQ(pool->sendPendingPayouts(),1u);ASSERT_EQ(attempts().size(),1u);EXPECT_TRUE(attempts()[0].retained);EXPECT_EQ(ingress->submits,1);unpaid();
}
TEST_F(PoolAttemptSource, CompleteSourceReadsAndUnavailableBodyRefuseBeforeWalletEffects) {
    struct Interruption {sqlite3* db;bool hit=false;};Interruption i{raw()};
    sqlite3_trace_v2(raw(),SQLITE_TRACE_ROW,[](unsigned,void* p,void* statement,void*) {
        auto& state=*static_cast<Interruption*>(p);const auto* s=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(s && std::strstr(s,"SELECT p.block_id,p.worker_id,p.wallet_address,p.amount,p.allocation_origin,b.block_hash")){state.hit=true;sqlite3_interrupt(state.db);}return 0;
    },&i);
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    sqlite3_trace_v2(raw(),0,nullptr,nullptr);EXPECT_TRUE(i.hit);untouched();
    sqlite3_set_authorizer(raw(),[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ && table && std::strcmp(table,"blocks")==0?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    sqlite3_set_authorizer(raw(),nullptr,nullptr);untouched();
    sql(raw(),"UPDATE blocks SET block_hash=CAST(block_hash AS BLOB)");
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    sql(raw(),"UPDATE blocks SET block_hash=CAST(block_hash AS TEXT)");untouched();
    canonical->db.close();
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    untouched();ASSERT_EQ(canonical->db.init(canonical->root/"chain"),dinero::Status::Ok);
    healthy_preflight();ingress->submit=[&](const dinero::Transaction& tx){submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    ASSERT_EQ(pool->sendPendingPayouts(),1u);EXPECT_EQ(ingress->submits,1);
}
TEST_F(PoolAttemptSource, CapturedSelectionAndPolicyMustStillMatchInsideAttemptTransaction) {
    using A=dinero::pool::PoolAttemptSourceTestAccess;auto& db=pool->getDatabase();const std::vector<uint64_t> ids{1};
    const auto captured=A::Capture(db,ids);const auto binding=database_fixture_binding();
    const std::vector<std::pair<std::string,std::string>> changes={
        {"UPDATE payouts SET amount=amount+1","UPDATE payouts SET amount=amount-1"},
        {"UPDATE payouts SET wallet_address='different-address'","UPDATE payouts SET wallet_address='"+modern_address+"'"},
        {"UPDATE blocks SET height=height+1","UPDATE blocks SET height=height-1"},
        {"UPDATE blocks SET total_reward=total_reward+1","UPDATE blocks SET total_reward=total_reward-1"},
        {"UPDATE config SET value='101' WHERE key='required_confirmations'","UPDATE config SET value='100' WHERE key='required_confirmations'"}};
    for(const auto& [change,restore]:changes) {
        sql(raw(),change);
        EXPECT_THROW(A::BeginCaptured(db,binding,ids,captured),std::runtime_error)<<change;
        EXPECT_TRUE(attempts().empty());EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->submits,0);sql(raw(),restore);
    }
    EXPECT_EQ(A::Capture(db,ids),captured);untouched();
}
TEST_F(PoolAttemptSource, CapturedSelectionCommitRefusalPreservesAllAttemptAndWalletState) {
    using A=dinero::pool::PoolAttemptSourceTestAccess;auto& db=pool->getDatabase();const std::vector<uint64_t> ids{1};
    const auto captured=A::Capture(db,ids);const auto binding=database_fixture_binding();unsigned commits=0;
    sqlite3_commit_hook(raw(),[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(A::BeginCaptured(db,binding,ids,captured),std::runtime_error);
    sqlite3_commit_hook(raw(),nullptr,nullptr);EXPECT_EQ(commits,1u);untouched();EXPECT_EQ(A::Capture(db,ids),captured);
    healthy_preflight();ingress->submit=[&](const dinero::Transaction& tx){submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    ASSERT_EQ(pool->sendPendingPayouts(),1u);ASSERT_EQ(attempts().size(),1u);EXPECT_EQ(ingress->submits,1);unpaid();
}
} // namespace
