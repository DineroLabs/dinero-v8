#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "wallet/orchard_operation_archive.h"
#endif
namespace dinero {
TEST(OrchardSpendRequest, BackendPolicyRemainsExplicit){
    WalletServiceOwnerFixture f;auto use=WalletService::AcquireWalletUse(f.service);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_FALSE(use->OrchardProofs().Query(orchard::Hash{1}));
#else
    EXPECT_THROW(use->OrchardProofs(),std::runtime_error);
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardSpendRequestFixture : OrchardProofOwnerFixture {
    void ArchiveCompleted(uint8_t id=1,uint32_t number=3){
        // Mining/replay records the observation; archival is a separate explicit
        // existing API. Capture selected lookups before acquiring wallet SQLite.
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        const auto current=Account(number);auto use=WalletService::AcquireWalletUse(wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(lease->Session());auto* db=lease->Database();
        orchard::Hash identity{};
        {struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
         OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1",-1,&q.p,nullptr)==SQLITE_OK);
         OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_ROW&&sqlite3_column_type(q.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(q.p,0)==32);
         const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q.p,0));OrchardAdmissionFixture::Require(bytes);std::copy_n(bytes,32,identity.begin());
         OrchardAdmissionFixture::Require(identity!=orchard::Hash{}&&sqlite3_step(q.p)==SQLITE_DONE);}
        OrchardAdmissionFixture::Require(sqlite3_exec(db,"PRAGMA synchronous=FULL",nullptr,nullptr,nullptr)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
        try{
            wallet::OrchardOperationArchive archive(db,{orchard::WalletNetwork::Regtest,Domain().genesis_wire,identity,number},Domain(),seed->Bytes());
            const auto staged=archive.StageCompleted(current.revision,current.account,Operation(id),(*view)->Point((*view)->Head()).lookups);
            OrchardAdmissionFixture::Require(!staged.account.Operations().Entries().contains(Operation(id)));
            OrchardAdmissionFixture::Require(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);
        }catch(...){if(!sqlite3_get_autocommit(db))OrchardAdmissionFixture::Require(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK);throw;}
    }
    auto Payments(uint64_t amount=200000,uint32_t destination=17){
        return std::vector<orchard::WalletPayment>{{amount,AccountKeys(destination).Receiver(orchard::WalletScope::External,{})}};
    }
    auto Invoke(wallet::OrchardProofJobs& jobs,std::span<const orchard::WalletPayment> payments,
            std::span<const orchard::TransparentOutput> outputs,uint64_t fee=100000,
            uint64_t expected=0,uint32_t account=3,uint8_t id=1){
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        const auto revision=expected?expected:Account(account).revision;auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::QueueCatalogRequestForReplay(use->Wallet(),Session(),
            {Domain(),102,account},revision,**view,Operation(id),payments,outputs,fee,jobs);
    }
    auto Request(wallet::OrchardProofJobs& jobs,bool withdraw=false,uint64_t expected=0){
        const auto payments=Payments();const std::vector<orchard::TransparentOutput> outputs{{400000,f.script}};
        return withdraw?Invoke(jobs,{},outputs,100000,expected):Invoke(jobs,payments,{},100000,expected);
    }
};
}
TEST(OrchardSpendRequest, MatchingRetryAndReopenReturnOriginalReservationWithoutTask){
    OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs,missing;const auto revision=f.Account(3).revision;
    auto first=f.Request(jobs);ASSERT_TRUE(first->enqueued);ASSERT_TRUE(first->durable);ASSERT_TRUE(first->durable->request_commitment);EXPECT_FALSE(first->existing_request);
    const auto exact=f.Account(3).account.Operations().Encode();const auto before=f.Snapshot();
    for(auto* executor:{&jobs,&missing}){
        auto retry=f.Request(*executor,false,revision);ASSERT_TRUE(retry->durable);EXPECT_TRUE(retry->existing_request);EXPECT_FALSE(retry->enqueued);EXPECT_FALSE(retry->archived);
        EXPECT_EQ(retry->revision,first->revision);EXPECT_EQ(retry->durable->message,first->durable->message);EXPECT_EQ(retry->durable->nullifiers,first->durable->nullifiers);EXPECT_EQ(retry->durable->request_commitment,first->durable->request_commitment);
        EXPECT_EQ(f.Snapshot(),before);
    }
    missing.RequestStop();auto stopped=f.Request(missing,false,revision);EXPECT_TRUE(stopped->existing_request);EXPECT_FALSE(stopped->enqueued);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    auto reopened=f.Request(missing,false,revision);EXPECT_TRUE(reopened->existing_request);EXPECT_FALSE(reopened->enqueued);EXPECT_EQ(f.Snapshot(),before);
    const auto after=f.Account(3).account.Operations().Encode();EXPECT_TRUE(std::equal(exact.Bytes().begin(),exact.Bytes().end(),after.Bytes().begin(),after.Bytes().end()));
    EXPECT_FALSE(missing.Query(f.Operation(1)));EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
}
TEST(OrchardSpendRequest, ChangedRecipientsAmountsMemoOutputsFeeAndOrderRefuse){
    OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs;auto payments=f.Payments(100000);payments.push_back({100000,f.AccountKeys(3).Receiver(orchard::WalletScope::External,{})});
    auto first=f.Invoke(jobs,payments,{});ASSERT_TRUE(first->enqueued);const auto before=f.Snapshot();
    {auto changed=payments;changed[0].amount_una+=1;
    EXPECT_THROW(f.Invoke(jobs,changed,{}),std::runtime_error);}
    {auto changed=payments;changed[0].memo[511]=1;
    EXPECT_THROW(f.Invoke(jobs,changed,{}),std::runtime_error);}
    {const std::vector<orchard::WalletPayment> reversed{payments[1],payments[0]};
    EXPECT_THROW(f.Invoke(jobs,reversed,{}),std::runtime_error);}
    {auto changed=f.Payments();
    EXPECT_THROW(f.Invoke(jobs,changed,{}),std::runtime_error);}
    EXPECT_THROW(f.Invoke(jobs,payments,{},100001),std::runtime_error);
    const std::vector<orchard::TransparentOutput> extra{{1,f.f.script}};
    EXPECT_THROW(f.Invoke(jobs,payments,extra),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.Invoke(jobs,payments,{})->existing_request);
}
TEST(OrchardSpendRequest, UnknownLegacyBindingWrongAccountAndStaleNewRevisionRefuse){
    OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs;const auto before=f.Snapshot();const auto payments=f.Payments();
    EXPECT_THROW(f.Invoke(jobs,payments,{},100000,f.Account(3).revision+1),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
    auto legacy=f.Reserve();const auto reserved=f.Snapshot();
    EXPECT_THROW(f.Request(jobs),std::runtime_error);EXPECT_EQ(f.Snapshot(),reserved);EXPECT_FALSE(jobs.Query(f.Operation(1)));
    OrchardSpendRequestFixture bound;wallet::OrchardProofJobs second;auto request=bound.Request(second);const auto exact=bound.Snapshot();const auto same=bound.Payments();
    EXPECT_THROW(bound.Invoke(second,same,{},100000,0,17),std::runtime_error);EXPECT_EQ(bound.Snapshot(),exact);
    EXPECT_TRUE(bound.Request(second)->existing_request);
}
TEST(OrchardSpendRequest, WholeCatalogReadsAndReadCommitRequiredForRetry){
    OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs;auto request=f.Request(jobs);const auto before=f.Snapshot();
    f.Sql("CREATE TEMP TABLE saved_request_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();
    EXPECT_THROW(f.Request(jobs,false,request->revision),std::runtime_error);EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_request_owner; DROP TABLE saved_request_owner");EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.Database();sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(f.Request(jobs,false,request->revision),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    bool seen=false;sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;},&seen);
    EXPECT_THROW(f.Request(jobs,false,request->revision),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(seen);EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().lockWallet();
    EXPECT_THROW(f.Request(jobs,false,request->revision),std::runtime_error);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    EXPECT_TRUE(f.Request(jobs)->existing_request);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardSpendRequest, FailedInitialWritesAndCommitPublishNeitherBindingNorTask){
    OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs;const auto before=f.Snapshot();const auto revision=f.Account(3).revision;
    f.Sql("CREATE TRIGGER refuse_request BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'request refusal'); END");
    EXPECT_THROW(f.Request(jobs,false,revision),std::runtime_error);f.Sql("DROP TRIGGER refuse_request");EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(jobs.Query(f.Operation(1)));f.ExpectAllSlotsFree(jobs);
    bool seen=false;auto* db=f.Database();sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&seen);
    EXPECT_THROW(f.Request(jobs,false,revision),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(seen);EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(jobs.Query(f.Operation(1)));f.ExpectAllSlotsFree(jobs);
    auto first=f.Request(jobs,false,revision);EXPECT_TRUE(first->enqueued);EXPECT_FALSE(first->existing_request);EXPECT_TRUE(first->durable->request_commitment);
}
TEST(OrchardSpendRequest, VersionedQueuePreservesLegacyAndRejectsMalformedBindings){
    OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs;auto request=f.Request(jobs);const auto encoded=f.Account(3).account.Operations().Encode();
    const auto original=std::vector<uint8_t>(encoded.Bytes().begin(),encoded.Bytes().end());ASSERT_GT(original.size(),33u);EXPECT_EQ(original[7],'2');
    auto roundtrip=wallet::OrchardOperationQueue::Restore(encoded,f.Domain());EXPECT_EQ(roundtrip.Entries().at(f.Operation(1)).request_commitment,request->durable->request_commitment);
    auto malformed=original;malformed[malformed.size()-33]=2;
    EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(malformed),f.Domain()),std::runtime_error);
    malformed=original;std::fill(malformed.end()-32,malformed.end(),0);
    EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(malformed),f.Domain()),std::runtime_error);
    for(size_t cut=1;cut<=33;++cut)
    EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(std::span<const uint8_t>(original).first(original.size()-cut)),f.Domain()),std::runtime_error);
    malformed=original;malformed.push_back(0);
    EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(malformed),f.Domain()),std::runtime_error);
    // A predecessor unbound encoding remains explicitly unknown. Do not infer
    // a request commitment from its operation ID, message, or recipient labels.
    auto predecessor=original;predecessor[7]='1';predecessor.resize(predecessor.size()-33);
    const auto legacy=wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(predecessor),f.Domain());EXPECT_FALSE(legacy.Entries().at(f.Operation(1)).request_commitment);
    const auto preserved=legacy.Encode();EXPECT_EQ(std::vector<uint8_t>(preserved.Bytes().begin(),preserved.Bytes().end()),predecessor);
    predecessor[7]='2';predecessor.push_back(0);
    EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(predecessor),f.Domain()),std::runtime_error);
}
TEST(OrchardSpendRequest, ReadyArchiveAndReactivationRetainExactRequestAndSignedBytes){
    OrchardSpendRequestFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();const auto original_revision=f.Account(3).revision;
    auto request=f.Request(jobs,true);ASSERT_TRUE(request->enqueued);ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    auto captured=f.Read(jobs);ASSERT_TRUE(captured.proof);const auto envelope=orchard::TransactionEnvelope::Create(0,{},request->transparent_outputs,request->fee_una,captured.proof->Bytes());
    const auto authorization=f.AuthorizeAtSelectedTip(envelope);
    auto ready=f.Ready(1,request->revision,authorization);const auto exact=authorization.Orchard().CanonicalBytes();const auto before=f.Snapshot();
    auto retry=f.Request(jobs,true,original_revision);ASSERT_TRUE(retry->durable);EXPECT_TRUE(retry->existing_request);EXPECT_FALSE(retry->enqueued);EXPECT_EQ(retry->durable->transaction,exact);EXPECT_EQ(f.Snapshot(),before);
    const std::vector<orchard::TransparentOutput> changed{{400001,f.f.script}};
    EXPECT_THROW(f.Invoke(jobs,{},changed,100000,original_revision),std::runtime_error);
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());(void)f.Mine(body);EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    // A completed observation alone remains a current operation. Exercise the
    // actual archive owner explicitly before asserting the archived retry form.
    const auto observed=f.Snapshot();auto current=f.Request(jobs,true,original_revision);
    ASSERT_TRUE(current->durable);EXPECT_FALSE(current->archived);EXPECT_TRUE(current->observation);
    EXPECT_EQ(current->durable->transaction,exact);EXPECT_EQ(f.Snapshot(),observed);
    f.ArchiveCompleted();
    const auto completed=f.Snapshot();auto archived=f.Request(jobs,true,original_revision);ASSERT_TRUE(archived->durable);EXPECT_TRUE(archived->archived);EXPECT_TRUE(archived->observation);EXPECT_EQ(archived->durable->transaction,exact);EXPECT_EQ(archived->durable->request_commitment,request->durable->request_commitment);EXPECT_EQ(f.Snapshot(),completed);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto reactivated=f.Snapshot();auto reopened=f.Request(jobs,true,original_revision);EXPECT_FALSE(reopened->archived);EXPECT_TRUE(reopened->existing_request);EXPECT_FALSE(reopened->enqueued);EXPECT_EQ(reopened->durable->transaction,exact);EXPECT_EQ(f.Snapshot(),reactivated);
}
#endif
} // namespace dinero
