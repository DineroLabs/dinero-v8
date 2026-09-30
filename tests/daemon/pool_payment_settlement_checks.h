#pragma once
#include "pool/canonical_payment.h"
#include "pool/pool_manager.h"
#include <openssl/sha.h>
namespace dinero::pool {
struct PoolPaymentSettlementTestAccess {
    static sqlite3* Raw(PoolDB& db){return db.db_;}
    static PoolPaymentAttempt Begin(PoolDB& db,const PoolPaymentWalletBinding& binding,const std::vector<uint64_t>& ids){return db.beginPaymentAttempt(binding,ids);}
    static void Retain(PoolDB& db,const PoolPaymentAttempt& a,const PoolPaymentRetained& retained){db.retainPaymentAttempt(a,retained);}
    static bool Reconcile(const std::shared_ptr<ChainstateService>& source,PoolDB& db,const PoolPaymentAttempt& a){return PoolPaymentCanonicalOwner::Reconcile(source,db,a);}
};
}
namespace dinero {
namespace {
using SettlementAccess=pool::PoolPaymentSettlementTestAccess;
}
TEST(PoolPaymentSettlement, MissingSourcePreservesRetainedAttemptWithoutPayment) {
    const auto path=std::filesystem::temp_directory_path()/("pool-payment-settlement-missing-"+std::to_string(getpid())+".sqlite");
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove(path);std::filesystem::remove(path.string()+"-wal");std::filesystem::remove(path.string()+"-shm");}} cleanup{path};
    pool::PoolDB db(path.string());ASSERT_TRUE(db.initialize());pool::PoolPaymentAttempt a;a.retained=pool::PoolPaymentRetained{};
    EXPECT_FALSE(SettlementAccess::Reconcile({},db,a));EXPECT_TRUE(db.getPaymentAttempts().empty());
    auto unstarted=std::make_shared<ChainstateService>();
    EXPECT_THROW(SettlementAccess::Reconcile(unstarted,db,a),ChainstateService::WalletIndexUnavailable);
    EXPECT_TRUE(db.getPaymentAttempts().empty());
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct PoolSettlementFixture : CanonicalRecoveryFixture {
    std::unique_ptr<pool::PoolManager> manager;
    MempoolTransaction payment;
    std::shared_ptr<const RuntimeBlockBody> block;
    uint64_t amount{0};
    PoolSettlementFixture() {
        const OutPoint point(f.blocks[1].vtx.front().GetTxid(),0);
        payment=MempoolTransaction(SelectionSpend(f,point,f.replay->ProvenUtxos().at(point),100000));
        amount=payment.Historical().vout.at(0).GetValue();open();
        auto config=manager->getConfig();config.payout_mode=pool::PayoutMode::SOLO;config.pool_fee_percent=0;config.min_auto_payout=1;
        config.payment_funding=pool::PoolPaymentFunding{"canonical-recovery",1,100000};OrchardAdmissionFixture::Require(manager->setConfig(config));
        auto& db=manager->getDatabase();const auto address=VaultOwnerAddress(f.script);db.getOrCreateWorker("miner",address);
        pool::PoolBlock source;source.block_hash=f.blocks[1].GetHash().GetHex();source.height=1;source.finder_worker="miner";source.finder_address=address;
        source.reward=source.total_reward=source.distributable=amount;source.found_at=20;source.confirmations=source.required_confirmations=100;
        OrchardAdmissionFixture::Require(db.insertBlock(source));
        pool_orphan_accounting_checks::Sql(raw(),"UPDATE blocks SET confirmations=100,required_confirmations=100,pool_fee_percent=0");
        OrchardAdmissionFixture::Require(manager->processConfirmedBlocks()==1);
        pool::PoolPaymentWalletBinding binding;binding.funding=*config.payment_funding;binding.wallet.fill(7);binding.network=2;
        binding.genesis=VaultRawHash(uint256::FromHexUnsafe(Params().genesis_hash));
        const auto rows=db.getPayoutsReadyToSend();OrchardAdmissionFixture::Require(rows.size()==1);
        const auto attempt=SettlementAccess::Begin(db,binding,{rows[0].payout_id});
        pool::PoolPaymentRetained retained;retained.txid=VaultRawHash(payment.GetTxid().AsUint256());const auto bytes=payment.Serialize();
        OrchardAdmissionFixture::Require(SHA256(bytes.data(),bytes.size(),retained.body_sha256.data())!=nullptr);retained.fee_una=100000;
        SettlementAccess::Retain(db,attempt,retained);
    }
    void open(){manager=std::make_unique<pool::PoolManager>((f.path/"payment-settlement.sqlite").string());OrchardAdmissionFixture::Require(manager->initialize());}
    sqlite3* raw(){return SettlementAccess::Raw(manager->getDatabase());}
    auto attempt(){const auto a=manager->getDatabase().getPaymentAttempts();OrchardAdmissionFixture::Require(a.size()==1);return a[0];}
    bool reconcile(){return SettlementAccess::Reconcile(f.service,manager->getDatabase(),attempt());}
    void mine(){block=Mine(payment);}
    auto state(){return pool_orphan_accounting_checks::Read(raw(),"SELECT * FROM workers ORDER BY worker_id");}
    void check(bool settled) {
        const auto a=attempt();EXPECT_EQ(bool(a.settlement),settled);ASSERT_TRUE(a.retained);
        auto worker=manager->getDatabase().getWorker("miner");ASSERT_TRUE(worker);
        EXPECT_EQ(worker->pending_payout,settled?0u:amount);EXPECT_EQ(worker->total_paid,settled?amount:0u);
        const auto payouts=manager->getDatabase().getPayoutsForBlock(1);ASSERT_EQ(payouts.size(),1u);
        EXPECT_EQ(payouts[0].status,settled?pool::PayoutStatus::PAID:pool::PayoutStatus::CONFIRMED);
        EXPECT_EQ(payouts[0].txid,settled?payment.GetTxid().AsUint256().GetHex():std::string{});
        EXPECT_EQ(payouts[0].paid_at,settled?static_cast<int64_t>(block->Orchard().Header().timestamp):0);
    }
};
}
TEST(PoolPaymentSettlement, ActualCanonicalBodyReopenDisconnectAndReconnect) {
    PoolSettlementFixture f;const auto retained=f.attempt().retained;EXPECT_FALSE(f.reconcile());f.check(false);
    f.mine();ASSERT_TRUE(f.reconcile());f.check(true);const auto settled=f.attempt();
    EXPECT_EQ(settled.settlement->block,VaultRawHash(f.block->Orchard().Header().GetHash()));EXPECT_EQ(settled.settlement->height,102u);
    EXPECT_FALSE(f.reconcile());f.manager.reset();f.open();EXPECT_EQ(f.attempt(),settled);EXPECT_FALSE(f.reconcile());f.check(true);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_TRUE(f.reconcile());f.check(false);EXPECT_EQ(f.attempt().retained,retained);EXPECT_EQ(f.attempt().id,settled.id);EXPECT_EQ(f.attempt().members,settled.members);
    EXPECT_FALSE(f.reconcile());const auto connected=f.Submit(f.block->Orchard().WireBytes());ASSERT_TRUE(connected.accepted()&&connected.connected);
    ASSERT_TRUE(f.reconcile());f.check(true);EXPECT_EQ(f.attempt(),settled);
}
TEST(PoolPaymentSettlement, RequiredWritesAndCommitRollbackCompleteAccounting) {
    PoolSettlementFixture f;f.mine();const auto before=f.attempt();const auto balances=f.state();
    for(const auto* trigger:{
        "CREATE TRIGGER refuse_settlement BEFORE UPDATE OF total_paid ON workers BEGIN SELECT RAISE(ABORT,'fixture worker refusal'); END",
        "CREATE TRIGGER refuse_settlement BEFORE UPDATE OF status ON payouts BEGIN SELECT RAISE(ABORT,'fixture payout refusal'); END",
        "CREATE TRIGGER refuse_settlement BEFORE INSERT ON pool_payment_settlements BEGIN SELECT RAISE(ABORT,'fixture settlement refusal'); END"}) {
        pool_orphan_accounting_checks::Sql(f.raw(),trigger);
        EXPECT_THROW(f.reconcile(),std::runtime_error);
        pool_orphan_accounting_checks::Sql(f.raw(),"DROP TRIGGER refuse_settlement");EXPECT_EQ(f.attempt(),before);EXPECT_EQ(f.state(),balances);f.check(false);
    }
    bool committed=false;sqlite3_commit_hook(f.raw(),[](void* p){*static_cast<bool*>(p)=true;return 1;},&committed);
    EXPECT_THROW(SettlementAccess::Reconcile(f.f.service,f.manager->getDatabase(),before),std::runtime_error);
    sqlite3_commit_hook(f.raw(),nullptr,nullptr);EXPECT_TRUE(committed);EXPECT_EQ(f.attempt(),before);EXPECT_EQ(f.state(),balances);
    ASSERT_TRUE(f.reconcile());f.check(true);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto paid=f.attempt();const auto paid_balances=f.state();
    pool_orphan_accounting_checks::Sql(f.raw(),"CREATE TRIGGER refuse_undo BEFORE DELETE ON pool_payment_settlements BEGIN SELECT RAISE(ABORT,'fixture undo refusal'); END");
    EXPECT_THROW(f.reconcile(),std::runtime_error);
    pool_orphan_accounting_checks::Sql(f.raw(),"DROP TRIGGER refuse_undo");EXPECT_EQ(f.attempt(),paid);EXPECT_EQ(f.state(),paid_balances);
    ASSERT_TRUE(f.reconcile());f.check(false);
}
TEST(PoolPaymentSettlement, WrongBodyUnavailableSourceAndBorrowedTransactionPreserveState) {
    PoolSettlementFixture f;f.mine();const auto before=f.attempt();
    const auto original=util::hex(std::vector<uint8_t>(before.retained->body_sha256.begin(),before.retained->body_sha256.end()));
    pool_orphan_accounting_checks::Sql(f.raw(),"UPDATE pool_payment_attempts SET retained_body=CAST(X'0101010101010101010101010101010101010101010101010101010101010101' AS BLOB)");
    const auto invalid=f.attempt();
    EXPECT_THROW(f.reconcile(),std::runtime_error);
    EXPECT_EQ(f.attempt(),invalid);f.check(false);
    pool_orphan_accounting_checks::Sql(f.raw(),("UPDATE pool_payment_attempts SET retained_body=X'"+original+"'").c_str());
    pool_orphan_accounting_checks::Sql(f.raw(),"BEGIN IMMEDIATE");
    EXPECT_THROW(SettlementAccess::Reconcile(f.f.service,f.manager->getDatabase(),before),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));pool_orphan_accounting_checks::Sql(f.raw(),"ROLLBACK");EXPECT_EQ(f.attempt(),before);
    ASSERT_TRUE(f.reconcile());const auto paid=f.attempt();f.f.db.close();
    EXPECT_THROW(f.reconcile(),std::runtime_error);
    EXPECT_EQ(f.attempt(),paid);f.check(true);ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);EXPECT_FALSE(f.reconcile());
    f.wallet->Stop();f.f.ingress->Stop();f.f.service->Stop();
    EXPECT_THROW(f.reconcile(),std::exception);
    EXPECT_EQ(f.attempt(),paid);f.check(true);
}
TEST(PoolPaymentSettlement, CompleteSettlementInventoryAndCounterBoundsRefuse) {
    PoolSettlementFixture f;f.mine();const auto before=f.attempt();
    pool_orphan_accounting_checks::Sql(f.raw(),"UPDATE workers SET total_paid=9223372036854775807");const auto balances=f.state();
    EXPECT_THROW(f.reconcile(),std::runtime_error);
    EXPECT_EQ(f.attempt(),before);EXPECT_EQ(f.state(),balances);
    pool_orphan_accounting_checks::Sql(f.raw(),"UPDATE workers SET total_paid=0");ASSERT_TRUE(f.reconcile());const auto paid=f.attempt();
    struct Interrupt {sqlite3* db;bool hit=false;};Interrupt interruption{f.raw()};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* p,void* stmt,void*){auto& i=*static_cast<Interrupt*>(p);const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(sql&&std::strstr(sql,"SELECT attempt,block_hash,height,block_time")){i.hit=true;sqlite3_interrupt(i.db);}return 0;},&interruption);
    EXPECT_THROW(f.reconcile(),std::runtime_error);
    sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_TRUE(interruption.hit);EXPECT_EQ(f.attempt(),paid);f.check(true);
    pool_orphan_accounting_checks::Sql(f.raw(),"UPDATE pool_payment_settlements SET height='unavailable'");
    EXPECT_THROW(f.reconcile(),std::runtime_error);
    pool_orphan_accounting_checks::Sql(f.raw(),"UPDATE pool_payment_settlements SET height=102");EXPECT_EQ(f.attempt(),paid);EXPECT_FALSE(f.reconcile());
}
#endif
} // namespace dinero
