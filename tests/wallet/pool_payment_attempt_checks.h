#pragma once
#include "pool/pool_manager.h"
#include "pool/wallet_payment_backend.h"
namespace dinero::pool {
struct PoolPaymentAttemptTestAccess {static sqlite3* Raw(PoolDB& db){return db.db_;}};
}
namespace {
class PoolPaymentAttempt : public WalletBatchRpc {
protected:
    std::unique_ptr<dinero::pool::PoolManager> pool;
    std::filesystem::path pool_path;
    sqlite3* raw(){return dinero::pool::PoolPaymentAttemptTestAccess::Raw(pool->getDatabase());}
    void open_pool() {
        pool=std::make_unique<dinero::pool::PoolManager>(pool_path.string());
        if(!pool->initialize())throw std::runtime_error("fixture pool initialization");
        pool->setPaymentBackend(dinero::pool::MakeWalletPoolPaymentBackend(daemon));
    }
    void SetUp() override {
        dinero::SelectParams(dinero::Chain::REGTEST);WalletBatchRpc::SetUp();if(HasFatalFailure())return;
        pool_path=root/"pool.sqlite";open_pool();auto cfg=pool->getConfig();cfg.payout_mode=dinero::pool::PayoutMode::SOLO;
        cfg.pool_fee_percent=0;cfg.min_auto_payout=1;cfg.payment_funding=dinero::pool::PoolPaymentFunding{"owner",1,10000};ASSERT_TRUE(pool->setConfig(cfg));
        auto& db=pool->getDatabase();db.getOrCreateWorker("miner",modern_address);
        dinero::pool::PoolBlock block;block.block_hash=std::string(64,'a');block.height=7;block.finder_worker="miner";block.finder_address=modern_address;
        block.reward=block.total_reward=block.distributable=20000;block.found_at=20;block.confirmations=block.required_confirmations=100;
        ASSERT_TRUE(db.insertBlock(block));sql(raw(),"UPDATE blocks SET confirmations=100,required_confirmations=100,total_reward=20000,distributable=20000,pool_fee_percent=0");
        ASSERT_EQ(pool->processConfirmedBlocks(),1u);ASSERT_EQ(db.getPayoutsReadyToSend().size(),1u);ASSERT_TRUE(db.getPayoutsReadyToSend()[0].allocation_origin);
    }
    void TearDown() override {pool.reset();WalletBatchRpc::TearDown();}
    std::vector<dinero::pool::PoolPaymentAttempt> attempts(){return pool->getDatabase().getPaymentAttempts();}
    void unpaid() {
        const auto payouts=pool->getDatabase().getPayoutsForBlock(1);ASSERT_EQ(payouts.size(),1u);
        EXPECT_EQ(payouts[0].status,dinero::pool::PayoutStatus::CONFIRMED);EXPECT_TRUE(payouts[0].txid.empty());EXPECT_EQ(payouts[0].paid_at,0);
        auto worker=pool->getDatabase().getWorker("miner");ASSERT_TRUE(worker);EXPECT_EQ(worker->pending_payout,20000u);EXPECT_EQ(worker->total_paid,0u);
    }
    void healthy_preflight() {
        ingress->test=[&](const dinero::Transaction& tx) {
            before_preflight(tx);EXPECT_TRUE(sqlite3_get_autocommit(raw()));const auto a=attempts();EXPECT_EQ(a.size(),1u);
            if(!a.empty()){EXPECT_FALSE(a[0].retained);EXPECT_EQ(a[0].amount,20000u);}unpaid();
            return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());
        };
    }
    void submission(const dinero::Transaction& tx) {
        auto& wallet=service->get();EXPECT_TRUE(sqlite3_get_autocommit(raw()));EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));
        const auto rows=wallet.getPendingPayments();ASSERT_EQ(rows.size(),1u);const auto a=attempts();ASSERT_EQ(a.size(),1u);
        EXPECT_FALSE(a[0].retained);ASSERT_TRUE(rows[0].intent.request);const auto& request=*rows[0].intent.request;
        EXPECT_EQ(request.domain,dinero::PendingPaymentRequestDomain::PoolPayout);EXPECT_EQ(request.id,a[0].id);
        EXPECT_EQ(request.owner,a[0].members.front().origin);ASSERT_EQ(request.pool_origins.size(),1u);EXPECT_EQ(request.pool_origins[0],a[0].members[0].origin);
        EXPECT_EQ(rows[0].intent.amount_una,20000u);EXPECT_TRUE(rows[0].intent.additional_recipients.empty());
        EXPECT_EQ(rows[0].signed_body,tx.Serialize(dinero::TxSerializationMode::WithWitness));verify(tx,{hd});unpaid();
    }
};
TEST_F(PoolPaymentAttempt, ActualUnknownSubmissionRetainsAndReopensWithoutPayingOrResending) {
    healthy_preflight();ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult{submission(tx);throw std::runtime_error("fixture unknown submission outcome");};
    ASSERT_EQ(pool->sendPendingPayouts(),1u);ASSERT_EQ(ingress->tests,1);ASSERT_EQ(ingress->submits,1);
    auto saved=attempts();ASSERT_EQ(saved.size(),1u);ASSERT_TRUE(saved[0].retained);const auto body=service->get().getPendingPayments().at(0).signed_body;unpaid();
    dinero::pool::PoolDB::OrphanResult refused;refused.pending_reversed=91;
    EXPECT_FALSE(pool->getDatabase().reconcileOrphanedBlock(std::string(64,'a'),refused));EXPECT_EQ(refused.pending_reversed,91u);EXPECT_EQ(attempts(),saved);unpaid();
    pool.reset();service->get().open("owner");service->get().unlockWallet("historical-rpc",0);open_pool();daemon.chainstate.reset();daemon.tx_ingress=nullptr;
    EXPECT_EQ(pool->sendPendingPayouts(),0u);EXPECT_EQ(pool->retryFailedPayouts(pool->getConfig().max_payout_retries),0u);EXPECT_EQ(attempts(),saved);
    EXPECT_EQ(service->get().getPendingPayments().at(0).signed_body,body);EXPECT_EQ(ingress->submits,1);EXPECT_EQ(ingress->tests,1);unpaid();
}
TEST_F(PoolPaymentAttempt, UnavailablePreflightAttemptNeverRegeneratesAfterRetryOrReopen) {
    EXPECT_EQ(pool->sendPendingPayouts(),0u);auto saved=attempts();ASSERT_EQ(saved.size(),1u);EXPECT_FALSE(saved[0].retained);
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->submits,0);const auto tests=ingress->tests;
    healthy_preflight();ingress->submit=[&](const dinero::Transaction& tx){submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    EXPECT_EQ(pool->sendPendingPayouts(),0u);EXPECT_EQ(pool->retryFailedPayouts(pool->getConfig().max_payout_retries),0u);EXPECT_EQ(ingress->tests,tests);EXPECT_EQ(ingress->submits,0);
    pool.reset();open_pool();auto changed=pool->getConfig();changed.payment_funding->maximum_fee_una=20000;ASSERT_TRUE(pool->setConfig(changed));
    EXPECT_EQ(pool->sendPendingPayouts(),0u);EXPECT_EQ(attempts(),saved);EXPECT_EQ(ingress->tests,tests);EXPECT_EQ(ingress->submits,0);unpaid();
}
TEST_F(PoolPaymentAttempt, RequiredMembershipWriteAndCommitRefuseBeforeWalletEffects) {
    healthy_preflight();ingress->submit=[&](const dinero::Transaction& tx){submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    sql(raw(),"CREATE TRIGGER refuse_member BEFORE INSERT ON pool_payment_members BEGIN SELECT RAISE(ABORT,'fixture member refusal'); END");
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    sql(raw(),"DROP TRIGGER refuse_member");EXPECT_TRUE(attempts().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);unpaid();
    struct Commit {bool hit=false;};Commit commit;sqlite3_commit_hook(raw(),[](void* p){static_cast<Commit*>(p)->hit=true;return 1;},&commit);
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    sqlite3_commit_hook(raw(),nullptr,nullptr);EXPECT_TRUE(commit.hit);EXPECT_TRUE(attempts().empty());EXPECT_EQ(ingress->tests,0);unpaid();
    sql(raw(),"BEGIN IMMEDIATE");
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(raw()));sql(raw(),"ROLLBACK");EXPECT_TRUE(attempts().empty());
    ASSERT_EQ(pool->sendPendingPayouts(),1u);EXPECT_EQ(ingress->submits,1);unpaid();
}
TEST_F(PoolPaymentAttempt, RetentionWriteFailureResolvesCommittedWalletBodyWithoutResubmission) {
    healthy_preflight();ingress->submit=[&](const dinero::Transaction& tx){submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    sql(raw(),"CREATE TRIGGER refuse_retained BEFORE UPDATE ON pool_payment_attempts BEGIN SELECT RAISE(ABORT,'fixture retained refusal'); END");
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    ASSERT_EQ(ingress->submits,1);auto saved=attempts();ASSERT_EQ(saved.size(),1u);EXPECT_FALSE(saved[0].retained);
    const auto payment=service->get().getPendingPayments().at(0);sql(raw(),"DROP TRIGGER refuse_retained");daemon.chainstate.reset();daemon.tx_ingress=nullptr;
    EXPECT_EQ(pool->sendPendingPayouts(),1u);ASSERT_TRUE(attempts()[0].retained);EXPECT_EQ(ingress->submits,1);EXPECT_EQ(ingress->tests,1);
    EXPECT_EQ(service->get().getPendingPayments().at(0).signed_body,payment.signed_body);unpaid();
}
TEST_F(PoolPaymentAttempt, ExplicitWalletPolicyAndAllocationOwnershipRequiredBeforeEffects) {
    const auto cfg=pool->getConfig();auto change=cfg;change.payment_funding.reset();ASSERT_TRUE(pool->setConfig(change));
    EXPECT_EQ(pool->sendPendingPayouts(),0u);EXPECT_TRUE(attempts().empty());
    change=cfg;change.payment_funding->wallet_name="other";ASSERT_TRUE(pool->setConfig(change));
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    ASSERT_TRUE(pool->setConfig(cfg));service->get().lockWallet();
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    service->get().unlockWallet("historical-rpc",0);EXPECT_TRUE(attempts().empty());EXPECT_EQ(ingress->tests,0);
    sql(raw(),"UPDATE payouts SET allocation_origin=NULL");
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    EXPECT_TRUE(attempts().empty());EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->submits,0);unpaid();
}
TEST_F(PoolPaymentAttempt, CompleteAttemptReadsAndClosedOwnerRefuseWithoutAnotherDispatch) {
    EXPECT_EQ(pool->sendPendingPayouts(),0u);const auto saved=attempts();ASSERT_EQ(saved.size(),1u);const int tests=ingress->tests;
    struct Interrupt {sqlite3* db;bool hit=false;};Interrupt interrupt{raw()};
    sqlite3_trace_v2(raw(),SQLITE_TRACE_ROW,[](unsigned,void* p,void* raw,void*) {
        auto& i=*static_cast<Interrupt*>(p);const char* sqltext=sqlite3_sql(static_cast<sqlite3_stmt*>(raw));
        if(sqltext && std::strstr(sqltext,"SELECT m.origin,m.payout_id,m.attempt")){i.hit=true;sqlite3_interrupt(i.db);}return 0;
    },&interrupt);
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    sqlite3_trace_v2(raw(),0,nullptr,nullptr);EXPECT_TRUE(interrupt.hit);EXPECT_EQ(attempts(),saved);EXPECT_EQ(ingress->tests,tests);
    sql(raw(),"UPDATE pool_payment_members SET amount=amount+1");
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    sql(raw(),"UPDATE pool_payment_members SET amount=amount-1");EXPECT_EQ(attempts(),saved);EXPECT_EQ(ingress->submits,0);
    pool->ClosePayments();
    EXPECT_THROW(pool->sendPendingPayouts(),std::runtime_error);
    EXPECT_EQ(attempts(),saved);EXPECT_EQ(ingress->tests,tests);unpaid();
}
} // namespace
