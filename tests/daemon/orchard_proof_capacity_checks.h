#pragma once
namespace dinero {
TEST(OrchardProofCapacity, BackendPolicyRemainsExplicit){
    WalletServiceOwnerFixture f;auto use=WalletService::AcquireWalletUse(f.service);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_FALSE(use->OrchardProofs().Query(orchard::Hash{1}));
#else
    EXPECT_THROW(use->OrchardProofs(),std::runtime_error);
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardProofCapacityFixture : OrchardServiceProofFixture {
    auto Queue(wallet::OrchardProofJobs& jobs,bool withdraw=false){
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        const auto revision=Account(3).revision;auto use=WalletService::AcquireWalletUse(wallet);
        std::vector<orchard::WalletPayment> payments;std::vector<orchard::TransparentOutput> outputs;
        if(withdraw)outputs.push_back({400000,f.script});
        else payments.push_back({200000,AccountKeys(17).Receiver(orchard::WalletScope::External,{})});
        return wallet::OrchardAccountDelivery::QueueCatalogSpendForReplay(use->Wallet(),Session(),
            {Domain(),102,3},revision,**view,Operation(1),payments,outputs,100000,jobs);
    }
    auto Ticket(wallet::OrchardProofJobs& jobs,uint8_t id){
        // Capacity-only plans use synthetic transparent inputs. No admission,
        // wallet reservation or broadcast is attempted with these plans.
        const std::vector<orchard::ResolvedInput> inputs{{Operation(id),0,0xffffffffu,300000,f.script}};
        const auto context=orchard::SigningContext::Create(Domain(),0,inputs,{},100000);
        const std::vector<orchard::WalletPayment> payments{{200000,AccountKeys(17).Receiver(orchard::WalletScope::External,{})}};
        return jobs.Prepare(Operation(id),orchard::WalletBundlePlan::PrepareShield(AccountKeys(3),payments),context);
    }
    void ExpectAllSlotsFree(wallet::OrchardProofJobs& jobs){
        std::vector<std::unique_ptr<wallet::OrchardProofJobs::Submission>> slots;
        for(uint8_t id=80;id<84;++id){slots.push_back(Ticket(jobs,id));EXPECT_FALSE(jobs.Query(Operation(id)));}
        EXPECT_THROW(Ticket(jobs,84),std::runtime_error);
    }
};
}
TEST(OrchardProofCapacity, CapacityAndStoppedExecutorRefuseBeforeDurableReservation){
    OrchardProofCapacityFixture f;wallet::OrchardProofJobs idle;const auto before=f.Snapshot();
    std::vector<std::unique_ptr<wallet::OrchardProofJobs::Submission>> slots;
    for(uint8_t id=80;id<84;++id){slots.push_back(f.Ticket(idle,id));EXPECT_FALSE(idle.Query(f.Operation(id)));EXPECT_FALSE(idle.Cancel(f.Operation(id)));}
    EXPECT_THROW(f.Ticket(idle,80),std::runtime_error);
    EXPECT_THROW(f.Queue(idle),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(idle.Query(f.Operation(1)));
    slots.clear();f.ExpectAllSlotsFree(idle);
    idle.RequestStop();
    EXPECT_THROW(f.Queue(idle),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardProofCapacity, UnpublishedTicketRequiresBindingAndOutlivesIdleExecutorSafely){
    OrchardProofCapacityFixture f;std::unique_ptr<wallet::OrchardProofJobs::Submission> ticket;
    auto queue=wallet::OrchardOperationQueue::Empty(f.Domain());
    {
        wallet::OrchardProofJobs idle;ticket=f.Ticket(idle,80);
        EXPECT_FALSE(ticket->Publish());EXPECT_FALSE(idle.Query(f.Operation(80)));
        EXPECT_THROW(ticket->Bind(queue),std::runtime_error);
        queue=queue.Reserve(f.Operation(80),ticket->Intent());ticket->Bind(queue);
        EXPECT_FALSE(idle.Query(f.Operation(80)));
        // Healthy idle destruction; no worker starts, no race or churn.
    }
    EXPECT_FALSE(ticket->Publish());ticket.reset();
    EXPECT_EQ(queue.Entries().at(f.Operation(80)).phase,wallet::OrchardOperationQueue::Phase::Reserved);
}
TEST(OrchardProofCapacity, FailedWritesAndCommitReleaseOnlyPreparedCapacity){
    OrchardProofCapacityFixture f;wallet::OrchardProofJobs idle;const auto before=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_capacity_reserve BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'reservation refusal'); END");
    EXPECT_THROW(f.Queue(idle),std::runtime_error);f.Sql("DROP TRIGGER refuse_capacity_reserve");
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(idle.Query(f.Operation(1)));f.ExpectAllSlotsFree(idle);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto revision=f.Account(3).revision;
    auto use=WalletService::AcquireWalletUse(f.wallet);const auto session=f.Session();auto* db=f.Database();
    const std::vector<orchard::TransparentOutput> outputs{{400000,f.f.script}};
    struct Observe{wallet::OrchardProofJobs* jobs;orchard::Hash id;bool seen=false,hidden=false;} observed{&idle,f.Operation(1)};
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Observe*>(p);o.seen=true;o.hidden=!o.jobs->Query(o.id);return 1;},&observed);
    EXPECT_THROW(wallet::OrchardAccountDelivery::QueueCatalogSpendForReplay(use->Wallet(),session,{f.Domain(),102,3},revision,**view,f.Operation(1),{},outputs,100000,idle),std::runtime_error);
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(observed.seen);EXPECT_TRUE(observed.hidden);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(idle.Query(f.Operation(1)));f.ExpectAllSlotsFree(idle);
    const auto request=f.Queue(idle,true);ASSERT_TRUE(request);EXPECT_TRUE(request->enqueued);
    EXPECT_EQ(idle.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
    EXPECT_EQ(f.Account(3).account.Operations().Entries().at(f.Operation(1)).phase,wallet::OrchardOperationQueue::Phase::Reserved);
}
TEST(OrchardProofCapacity, ActualServicePublishesAfterCommitThenRetainsReadyBeforeUnshield){
    OrchardProofCapacityFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto revision=f.Account(3).revision;
    const auto session=f.Session();auto* db=f.Database();const std::vector<orchard::TransparentOutput> outputs{{400000,f.f.script}};
    struct Observe{wallet::OrchardProofJobs* jobs;orchard::Hash id;bool seen=false,hidden=false;} observed{&jobs,f.Operation(1)};
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Observe*>(p);o.seen=true;o.hidden=!o.jobs->Query(o.id);return 0;},&observed);
    auto request=wallet::OrchardAccountDelivery::QueueCatalogSpendForReplay(use->Wallet(),session,{f.Domain(),102,3},revision,**view,f.Operation(1),{},outputs,100000,jobs);
    sqlite3_commit_hook(db,nullptr,nullptr);ASSERT_TRUE(request);EXPECT_TRUE(observed.seen);EXPECT_TRUE(observed.hidden);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto reserved=f.Account(3);EXPECT_EQ(reserved.revision,request->revision);
    auto proof=jobs.CopyResult(request->operation_id,reserved.account.Operations());ASSERT_TRUE(proof);
    const auto envelope=orchard::TransactionEnvelope::Create(0,{},request->transparent_outputs,request->fee_una,proof->Bytes());
    const auto authorization=f.AuthorizeAtSelectedTip(envelope);
    const auto ready=f.Ready(1,request->revision,authorization);
    EXPECT_EQ(ready.account.Operations().Entries().at(request->operation_id).transaction,authorization.Orchard().CanonicalBytes());
    const auto retained=jobs.TakeResult(request->operation_id);ASSERT_TRUE(retained);EXPECT_EQ(retained->Bytes(),proof->Bytes());
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto txid=body.GetTxid().AsUint256();(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    ASSERT_TRUE(f.f.db.getCoin(txid,0).ok());EXPECT_EQ(f.f.db.getCoin(txid,0)->amount,400000u);
}
#endif
} // namespace dinero
