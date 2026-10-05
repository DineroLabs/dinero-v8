#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
// Observe actual snapshot writes; the table uses WITHOUT ROWID, so an
// update hook cannot distinguish preparation commits from reservation commits.
struct ShieldReservationCommitObserver {
    sqlite3* db;
    ShieldReservationCommitObserver(sqlite3* database,void* state,void (*callback)(sqlite3_context*,int,sqlite3_value**)):db(database){
        OrchardAdmissionFixture::Require(sqlite3_create_function_v2(db,"observe_shield_reservation_commit",0,SQLITE_UTF8,state,
            callback,nullptr,nullptr,nullptr)==SQLITE_OK);
        if(sqlite3_exec(db,"CREATE TEMP TRIGGER observe_shield_reservation_commit AFTER UPDATE ON main.orchard_wallet_snapshots BEGIN SELECT observe_shield_reservation_commit(); END",nullptr,nullptr,nullptr)!=SQLITE_OK){
            (void)Close();throw std::runtime_error("Shield reservation commit observer failed");
        }
    }
    ShieldReservationCommitObserver(const ShieldReservationCommitObserver&)=delete;
    ShieldReservationCommitObserver& operator=(const ShieldReservationCommitObserver&)=delete;
    ~ShieldReservationCommitObserver(){(void)Close();}
    bool Close() noexcept {
        if(!db)return true;
        sqlite3_commit_hook(db,nullptr,nullptr);
        const int drop=sqlite3_exec(db,"DROP TRIGGER IF EXISTS temp.observe_shield_reservation_commit",nullptr,nullptr,nullptr);
        const int unregister=sqlite3_create_function_v2(db,"observe_shield_reservation_commit",0,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr);
        db=nullptr;return drop==SQLITE_OK&&unregister==SQLITE_OK;
    }
};
struct ShieldReservationFixture:SharedPaymentFixture {
    ShieldReservationFixture(){Create(3);Create(17);}
    auto Inputs(){orchard::ResolvedInput input{};std::copy(funding.txid.begin(),funding.txid.end(),input.txid_wire.begin());
        input.output_index=funding.vout;input.sequence=UINT32_MAX;input.amount_una=funding.value.GetUna();input.script_pub_key=funding.spk;
        return std::vector<orchard::ResolvedInput>{input};}
    auto Payments(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto pin=lease->CopyRecoverySeed(Selected().session);const auto keys=orchard::WalletKeys::FromSeed(pin->Bytes(),17);
        return std::vector<orchard::WalletPayment>{{20000,keys.Receiver(orchard::WalletScope::External,{})}};}
    auto Queue(wallet::OrchardProofJobs& jobs,uint64_t expected=0){
        const auto view=View();const auto revision=expected?expected:Account(3).revision;const auto payments=Payments();
        auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::QueueCatalogShieldRequestForReplay(use->Wallet(),Selected().session,
            {Domain(),102,3},revision,*view,orchard::Hash{81},Inputs(),payments,
            std::vector<orchard::TransparentOutput>{{70000,script}},10000,jobs);}
    auto ReadShield(wallet::OrchardProofJobs& jobs){const auto view=View();const auto payments=Payments();
        auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::ReadCatalogShieldRequestProofForReplay(use->Wallet(),Selected().session,
            {Domain(),102,3},*view,orchard::Hash{81},Inputs(),payments,
            std::vector<orchard::TransparentOutput>{{70000,script}},10000,jobs);}
    auto Finish(wallet::OrchardProofJobs& jobs){const auto payments=Payments();
        auto use=WalletService::AcquireWalletUse(wallet);auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->finalizeRuntimeWalletShield(use->Wallet(),Selected().session,3,orchard::Hash{81},
            Inputs(),payments,std::vector<orchard::TransparentOutput>{{70000,script}},10000,jobs);}
    auto QueueSelected(wallet::OrchardProofJobs& jobs,uint64_t revision,
            const std::vector<orchard::ResolvedInput>& inputs,uint64_t session=0){
        const auto payments=Payments();auto use=WalletService::AcquireWalletUse(wallet);
        auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->queueRuntimeWalletShield(use->Wallet(),session?session:Selected().session,
            3,revision,orchard::Hash{81},inputs,payments,
            std::vector<orchard::TransparentOutput>{{70000,script}},10000,jobs);}
    auto ReserveSelected(uint64_t revision,const std::vector<orchard::ResolvedInput>& inputs,uint64_t session=0){
        const auto payments=Payments();auto use=WalletService::AcquireWalletUse(wallet);
        auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->reserveRuntimeWalletShield(use->Wallet(),session?session:Selected().session,
            3,revision,orchard::Hash{81},inputs,payments,std::vector<orchard::TransparentOutput>{{70000,script}},10000);}
    void ArchiveConfirmedShield(){
        // Replay records confirmation. Archival is a distinct existing API;
        // selected lookups are captured before acquiring the wallet transaction.
        const auto view=View();const auto current=Account(3);const auto id=orchard::Hash{81};
        const auto observation=current.account.Observations().find(id);
        Need(observation!=current.account.Observations().end()&&
            observation->second.outcome==wallet::OrchardAccountState::OperationOutcome::Confirmed);
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto seed=lease->CopyRecoverySeed(Selected().session);auto* db=lease->Database();
        Need(sqlite3_exec(db,"PRAGMA synchronous=FULL",nullptr,nullptr,nullptr)==SQLITE_OK);
        InventoryTestTransaction transaction(db);
        const auto inventory=wallet::OrchardOwnershipInventory::Read(db,seed->Bytes());
        const auto owner=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),[](const auto& a){return a.entry.account==3;});
        Need(owner!=inventory.accounts.end());
        wallet::OrchardOperationArchive archive(db,owner->identity,Domain(),seed->Bytes());
        const auto staged=archive.StageCompleted(current.revision,current.account,id,view->Point(view->Head()).lookups);
        Need(!staged.account.Operations().Entries().contains(id));
        transaction.Commit();
    }
    auto Stored(const std::vector<orchard::WalletPayment>& payments,uint64_t fee=10000){
        const auto view=View();auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::FindStoredShieldRequestForReplay(use->Wallet(),Selected().session,
            {Domain(),102,3},*view,orchard::Hash{81},payments,fee);}
    // Authenticated storage fixture: replace only the operation blob and its
    // length, retaining every other account byte. Never a production mutation.
    void StoreQueueFixture(const std::vector<uint8_t>& replacement){
        const auto current=Account(3);const auto encoded=current.account.Encode();
        const auto queue=current.account.Operations().Encode();
        std::vector<uint8_t> bytes(encoded.Bytes().begin(),encoded.Bytes().end());
        const auto found=std::search(bytes.begin(),bytes.end(),queue.Bytes().begin(),queue.Bytes().end());
        Need(found!=bytes.end()&&found-bytes.begin()>=4);
        Need(std::search(found+queue.Bytes().size(),bytes.end(),queue.Bytes().begin(),queue.Bytes().end())==bytes.end());
        const auto offset=static_cast<size_t>(found-bytes.begin());
        uint32_t length=0;for(size_t i=0;i<4;++i)length|=uint32_t(bytes[offset-4+i])<<(8*i);
        Need(length==queue.Bytes().size()&&replacement.size()<=UINT32_MAX);
        bytes.erase(bytes.begin()+offset,bytes.begin()+offset+length);
        bytes.insert(bytes.begin()+offset,replacement.begin(),replacement.end());
        for(size_t i=0;i<4;++i)bytes[offset-4+i]=replacement.size()>>(8*i);
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto pin=lease->CopyRecoverySeed(Selected().session);InventoryTestTransaction tx(lease->Database());
        const auto inventory=wallet::OrchardOwnershipInventory::Read(lease->Database(),pin->Bytes());
        const auto owner=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),[](const auto& a){return a.entry.account==3;});
        Need(owner!=inventory.accounts.end());orchard::WalletSnapshotStore store(lease->Database(),owner->identity,pin->Bytes());
        (void)store.StageReplaceRetaining(current.revision,orchard::WalletStateBytes(bytes));tx.Commit();
    }
    auto Reserve(){const auto view=View();const auto revision=Account(3).revision;const auto payments=Payments();auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::ReserveCatalogShieldForReplay(use->Wallet(),Selected().session,
            {Domain(),102,3},revision,*view,orchard::Hash{81},Inputs(),payments,std::vector<orchard::TransparentOutput>{{70000,script}},10000);}
};
}
TEST(WalletShieldReservations, DurableReservationPrecedesProofAndRefusesOrdinaryReuse){
    ShieldReservationFixture f;const auto before=f.Snapshot();auto reserved=f.Reserve();ASSERT_TRUE(reserved);
    ASSERT_EQ(reserved->committed_operations.Entries().size(),1u);
    const auto& input=reserved->committed_operations.Entries().at(orchard::Hash{81}).inputs.at(0);
    EXPECT_EQ(input.txid_wire,f.Inputs()[0].txid_wire);EXPECT_EQ(input.amount_una,100000u);
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());EXPECT_NE(f.Snapshot(),before);
    const auto committed=f.Snapshot();EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);
    // Real proof occurs only after the reservation owner/seed/SQLite scopes end.
    auto proved=std::move(reserved->plan).Prove(reserved->signing);EXPECT_FALSE(proved.Bytes().empty());
    EXPECT_EQ(f.Snapshot(),committed);f.Reopen();EXPECT_EQ(f.Snapshot(),committed);
    EXPECT_THROW(f.Reserve(),std::runtime_error);EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);
}
TEST(WalletShieldReservations, ExistingOrdinaryAndOtherAccountInputsRefuse){
    for(bool ordinary:{false,true}){
        ShieldReservationFixture f;
        if(ordinary){const auto payment=f.Pay();ASSERT_TRUE(payment.success)<<payment.error;}
        else f.ReserveFunding(17);
        const auto before=f.Snapshot();EXPECT_THROW(f.Reserve(),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
        f.Reopen();EXPECT_THROW(f.Reserve(),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
    }
}
TEST(WalletShieldReservations, ExactCoinAndDuplicateInputChecksPreserveOwners){
    ShieldReservationFixture f;const auto before=f.Snapshot();const auto view=f.View();const auto revision=f.Account(3).revision;const auto payments=f.Payments();
    for(int mode:{0,1,2,3}){
        auto inputs=f.Inputs();if(mode==0)++inputs[0].amount_una;if(mode==1)inputs[0].script_pub_key={0x51};
        if(mode==2)inputs.push_back(inputs[0]);if(mode==3)--inputs[0].sequence;
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_THROW(wallet::OrchardAccountDelivery::ReserveCatalogShieldForReplay(use->Wallet(),f.Selected().session,
            {f.Domain(),102,3},revision,*view,orchard::Hash{81},inputs,payments,std::vector<orchard::TransparentOutput>{{70000,f.script}},10000),std::runtime_error);
        EXPECT_EQ(f.Snapshot(),before);
    }
    EXPECT_TRUE(f.Reserve());
}
TEST(WalletShieldReservations, SqlReadWriteCommitAndBorrowedTransactionRefuse){
    ShieldReservationFixture f;const auto before=f.Snapshot();const auto view=f.View();const auto revision=f.Account(3).revision;const auto payments=f.Payments();
    auto reserve=[&]{auto use=WalletService::AcquireWalletUse(f.wallet);
        return wallet::OrchardAccountDelivery::ReserveCatalogShieldForReplay(use->Wallet(),f.Selected().session,
            {f.Domain(),102,3},revision,*view,orchard::Hash{81},f.Inputs(),payments,std::vector<orchard::TransparentOutput>{{70000,f.script}},10000);};
    f.Sql("CREATE TRIGGER refuse_shield_reservation BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'shield reservation refusal'); END");
    EXPECT_THROW(reserve(),std::runtime_error);f.Sql("DROP TRIGGER refuse_shield_reservation");EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.wallet->get().getCurrentDatabase();int commits=0;
    sqlite3_commit_hook(db,[](void* p){++*static_cast<int*>(p);return 1;},&commits);
    EXPECT_THROW(reserve(),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(commits,1);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
        return action==SQLITE_READ&&table&&std::string_view(table)=="utxos"?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(reserve(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
     EXPECT_THROW(reserve(),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));
     ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);}
    EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(reserve());
}
TEST(WalletShieldReservations, SelectedSourceSpansCommitAndReleasesBeforeProof){
    ShieldReservationFixture f;const auto revision=f.Account(3).revision;const auto inputs=f.Inputs();
    struct Observe{ChainstateService& source;int commits=0,preparations=0;
        bool selected=false,staged=false,staged_selected=true,preparation_selected=false;} observed{*f.f.service};
    auto* db=f.wallet->get().getCurrentDatabase();
    ShieldReservationCommitObserver cleanup(db,&observed,[](sqlite3_context* context,int,sqlite3_value**){
        auto& o=*static_cast<Observe*>(sqlite3_user_data(context));o.staged=true;
        o.staged_selected&=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);sqlite3_result_null(context);
    });
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Observe*>(p);
        if(!o.staged){++o.preparations;o.preparation_selected|=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);return 0;}
        ++o.commits;o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);return 0;},&observed);
    auto reserved=f.ReserveSelected(revision,inputs);ASSERT_TRUE(cleanup.Close());
    EXPECT_EQ(observed.preparations,1);EXPECT_FALSE(observed.preparation_selected);
    EXPECT_TRUE(observed.staged);EXPECT_TRUE(observed.staged_selected);
    ASSERT_TRUE(reserved);EXPECT_EQ(observed.commits,1);EXPECT_TRUE(observed.selected);
    EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
    const auto committed=f.Snapshot();EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);
    auto proof=std::move(reserved->plan).Prove(reserved->signing);EXPECT_FALSE(proof.Bytes().empty());
    EXPECT_EQ(f.Snapshot(),committed);f.Reopen();EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);
}
TEST(WalletShieldReservations, SelectedSourceRejectsMissingLifetimeBadCoinsAndBorrowedOwners){
    ShieldReservationFixture f;const auto revision=f.Account(3).revision;const auto before=f.Snapshot();
    const auto inputs=f.Inputs();const auto payments=f.Payments();
    {auto use=WalletService::AcquireWalletUse(f.wallet);
     EXPECT_THROW(f.f.service->reserveRuntimeWalletShield(use->Wallet(),f.Selected().session,3,revision,
        orchard::Hash{81},inputs,payments,std::vector<orchard::TransparentOutput>{{70000,f.script}},10000),std::runtime_error);}
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();EXPECT_THROW(f.ReserveSelected(revision,inputs),std::runtime_error);}
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     EXPECT_THROW(f.ReserveSelected(revision,inputs),std::runtime_error);EXPECT_TRUE(sqlite3_get_autocommit(lease->Database()));}
    EXPECT_THROW(f.ReserveSelected(revision,inputs,f.Selected().session+1),std::runtime_error);
    for(int mode:{0,1,2,3,4}){auto bad=inputs;
        if(mode==0)++bad[0].amount_una;if(mode==1)bad[0].script_pub_key={0x51};
        if(mode==2)bad[0].output_index=UINT32_MAX;if(mode==3)--bad[0].sequence;
        if(mode==4)bad.push_back(bad[0]);
        EXPECT_THROW(f.ReserveSelected(revision,bad),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
    }
    EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.ReserveSelected(revision,inputs));
}
TEST(WalletShieldReservations, ExactShieldRequestCommitsBeforePublishAndDoesNotRegenerate){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto revision=f.Account(3).revision;
    struct Observe{wallet::OrchardProofJobs& jobs;ChainstateService& source;int count=0,preparations=0;
        bool absent=false,staged=false,preparation_absent=true,preparation_selected=false;} observed{jobs,*f.f.service};
    auto* db=f.wallet->get().getCurrentDatabase();
    ShieldReservationCommitObserver cleanup(db,&observed,[](sqlite3_context* context,int,sqlite3_value**){
        static_cast<Observe*>(sqlite3_user_data(context))->staged=true;sqlite3_result_null(context);
    });
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Observe*>(p);
        if(!o.staged){++o.preparations;o.preparation_absent&=!o.jobs.Query(orchard::Hash{81});
            o.preparation_selected|=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);return 0;}
        ++o.count;o.absent=!o.jobs.Query(orchard::Hash{81});return 0;},&observed);
    auto request=f.Queue(jobs,revision);ASSERT_TRUE(cleanup.Close());
    EXPECT_EQ(observed.preparations,1);EXPECT_FALSE(observed.preparation_selected);
    EXPECT_TRUE(observed.preparation_absent);EXPECT_TRUE(observed.staged);
    ASSERT_TRUE(request->durable);EXPECT_TRUE(request->durable->request_commitment);EXPECT_TRUE(request->enqueued);
    EXPECT_EQ(observed.count,1);EXPECT_TRUE(observed.absent);EXPECT_FALSE(request->existing_request);
    const auto before=f.Snapshot();const auto retry=f.Queue(jobs,revision);
    EXPECT_TRUE(retry->existing_request);EXPECT_FALSE(retry->enqueued);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(retry->durable->request_commitment,request->durable->request_commitment);
    jobs.RequestStop();wallet::OrchardProofJobs absent;
    const auto stopped=f.Queue(absent,revision);EXPECT_TRUE(stopped->existing_request);EXPECT_FALSE(stopped->enqueued);
    EXPECT_FALSE(absent.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);
    f.Reopen();const auto reopened=f.Queue(absent,revision);EXPECT_TRUE(reopened->existing_request);EXPECT_FALSE(reopened->enqueued);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletShieldReservations, ShieldRequestMismatchAndRequiredCommitRetainOwners){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto revision=f.Account(3).revision;const auto view=f.View();
    const auto inputs=f.Inputs();const auto payments=f.Payments();const auto before=f.Snapshot();auto* db=f.wallet->get().getCurrentDatabase();
    auto queue=[&](uint32_t account,const auto& in,const auto& to,const auto& out,uint64_t fee){auto use=WalletService::AcquireWalletUse(f.wallet);
        return wallet::OrchardAccountDelivery::QueueCatalogShieldRequestForReplay(use->Wallet(),f.Selected().session,
            {f.Domain(),102,account},revision,*view,orchard::Hash{81},in,to,out,fee,jobs);};
    const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};
    int commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<int*>(p);return 1;},&commits);
    EXPECT_THROW(queue(3,inputs,payments,outputs,10000),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(commits,1);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);
    f.Sql("CREATE TRIGGER refuse_shield_request BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'shield request refusal'); END");
    EXPECT_THROW(queue(3,inputs,payments,outputs,10000),std::runtime_error);f.Sql("DROP TRIGGER refuse_shield_request");
    EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);
    ASSERT_TRUE(queue(3,inputs,payments,outputs,10000)->enqueued);const auto committed=f.Snapshot();
    for(int mode:{0,1,2,3,4,5}){auto in=inputs;auto to=payments;auto out=outputs;uint64_t fee=10000;
        if(mode==0)++in[0].amount_una;if(mode==1)in[0].txid_wire[0]^=1;
        if(mode==2)to[0].memo[0]=1;if(mode==3)++to[0].amount_una;
        if(mode==4)++out[0].amount_una;if(mode==5)++fee;
        EXPECT_THROW(queue(3,in,to,out,fee),std::runtime_error);EXPECT_EQ(f.Snapshot(),committed);
    }
    EXPECT_THROW(queue(17,inputs,payments,outputs,10000),std::runtime_error);EXPECT_EQ(f.Snapshot(),committed);
}
TEST(WalletShieldReservations, ShieldProofRequiresCurrentBoundOwnerAndRetainsReservation){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    auto queued=f.Queue(jobs);ASSERT_TRUE(queued->enqueued); // WalletService already owns the running executor.
    ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    const auto before=f.Snapshot();const auto proof=f.ReadShield(jobs);ASSERT_TRUE(proof.proof);
    EXPECT_EQ(proof.durable.inputs.size(),1u);EXPECT_EQ(f.Snapshot(),before);
    wallet::OrchardProofJobs absent;const auto missing=f.ReadShield(absent);EXPECT_FALSE(missing.state);EXPECT_FALSE(missing.proof);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.wallet->get().getCurrentDatabase();bool seen=false;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* word,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&word&&std::string_view(word)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;},&seen);
    EXPECT_THROW(f.ReadShield(jobs),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(seen);
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),proof.proof->Bytes());EXPECT_EQ(f.Snapshot(),before);
    f.Reopen();EXPECT_THROW(f.ReadShield(jobs),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletShieldReservations, UnpublishedShieldTicketPreservesCommittedReservation){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto view=f.View();
    const auto revision=f.Account(3).revision;const auto inputs=f.Inputs();const auto payments=f.Payments();
    wallet::OrchardShieldRequest prepared;
    {auto use=WalletService::AcquireWalletUse(f.wallet);
     prepared=wallet::OrchardAccountDelivery::PrepareCatalogShieldRequestForReplay(use->Wallet(),f.Selected().session,
         {f.Domain(),102,3},revision,*view,orchard::Hash{81},inputs,payments,
         std::vector<orchard::TransparentOutput>{{70000,f.script}},10000,jobs);}
    ASSERT_TRUE(prepared.result);ASSERT_TRUE(prepared.submission);EXPECT_FALSE(prepared.result->enqueued);
    EXPECT_FALSE(jobs.Query(orchard::Hash{81}));const auto committed=f.Snapshot();
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);
    // A durable retry cannot publish another job while the original committed
    // ticket is still privately owned by its host.
    auto retry=f.Queue(jobs,revision);EXPECT_TRUE(retry->existing_request);EXPECT_FALSE(retry->enqueued);
    EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),committed);
    const auto published=std::move(prepared).Publish();ASSERT_TRUE(published);EXPECT_TRUE(published->enqueued);
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),std::optional(wallet::OrchardProofJobs::State::Queued));
    EXPECT_EQ(f.Snapshot(),committed);
}
TEST(WalletShieldReservations, SelectedQueuedRequestCommitsBeforePublicationAndRetry){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto revision=f.Account(3).revision;const auto inputs=f.Inputs();
    struct Observe{ChainstateService& source;wallet::OrchardProofJobs& jobs;int commits=0;bool write=false,selected=false,hidden=false;}
        observed{*f.f.service,jobs};
    auto* db=f.wallet->get().getCurrentDatabase();
    sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
        if(action==SQLITE_UPDATE&&table&&std::string_view(table)=="orchard_wallet_snapshots")static_cast<Observe*>(p)->write=true;
        return SQLITE_OK;},&observed);
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Observe*>(p);if(!o.write)return 0;++o.commits;
        o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);
        o.hidden=!o.jobs.Query(orchard::Hash{81});return 0;},&observed);
    auto request=f.QueueSelected(jobs,revision,inputs);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);
    ASSERT_TRUE(request);EXPECT_TRUE(request->enqueued);EXPECT_EQ(observed.commits,1);
    EXPECT_TRUE(observed.selected);EXPECT_TRUE(observed.hidden);
    EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),std::optional(wallet::OrchardProofJobs::State::Queued));
    const auto before=f.Snapshot();wallet::OrchardProofJobs absent;
    auto retry=f.QueueSelected(absent,revision,inputs);EXPECT_TRUE(retry->existing_request);EXPECT_FALSE(retry->enqueued);
    EXPECT_FALSE(absent.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);
    // Invalid new work still refuses, while the exact retry keeps its bytes.
    auto changed=inputs;++changed[0].amount_una;EXPECT_THROW(f.QueueSelected(absent,revision,changed),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);f.Reopen();
    EXPECT_TRUE(f.QueueSelected(absent,revision,inputs)->existing_request);EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletShieldReservations, SelectedQueuedRefusalsLeaveNoPlanOrReservation){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto revision=f.Account(3).revision;
    const auto inputs=f.Inputs();const auto payments=f.Payments();const auto before=f.Snapshot();
    {auto use=WalletService::AcquireWalletUse(f.wallet);
     EXPECT_THROW(f.f.service->queueRuntimeWalletShield(use->Wallet(),f.Selected().session,3,revision,
        orchard::Hash{81},inputs,payments,std::vector<orchard::TransparentOutput>{{70000,f.script}},10000,jobs),std::runtime_error);}
    EXPECT_THROW(f.QueueSelected(jobs,revision,inputs,f.Selected().session+1),std::runtime_error);
    auto bad=inputs;bad[0].output_index=UINT32_MAX;EXPECT_THROW(f.QueueSelected(jobs,revision,bad),std::runtime_error);
    EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);
    f.Sql("CREATE TRIGGER refuse_selected_shield BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'selected shield refusal'); END");
    EXPECT_THROW(f.QueueSelected(jobs,revision,inputs),std::runtime_error);f.Sql("DROP TRIGGER refuse_selected_shield");
    EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.wallet->get().getCurrentDatabase();struct Fault{int commits=0;bool write=false;} fault;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
        if(action==SQLITE_UPDATE&&table&&std::string_view(table)=="orchard_wallet_snapshots")static_cast<Fault*>(p)->write=true;
        return SQLITE_OK;},&fault);
    sqlite3_commit_hook(db,[](void* p){auto& f=*static_cast<Fault*>(p);if(!f.write)return 0;++f.commits;return 1;},&fault);
    EXPECT_THROW(f.QueueSelected(jobs,revision,inputs),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(fault.commits,1);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);
    EXPECT_TRUE(f.QueueSelected(jobs,revision,inputs)->enqueued);
}
TEST(WalletShieldReservations, SelectedSigningRetainsReadyBeforeAdmissionAndReopen){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    const auto queued=f.QueueSelected(jobs,f.Account(3).revision,f.Inputs());ASSERT_TRUE(queued->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    struct Observe{ChainstateService& source;wallet::OrchardProofJobs& jobs;int commits=0;bool write=false,selected=false,retained=false;}
        observed{*f.f.service,jobs};auto* db=f.wallet->get().getCurrentDatabase();
    sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
        if(action==SQLITE_UPDATE&&table&&std::string_view(table)=="orchard_wallet_snapshots")static_cast<Observe*>(p)->write=true;
        return SQLITE_OK;},&observed);
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Observe*>(p);if(!o.write)return 0;++o.commits;
        o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);
        o.retained=o.jobs.Query(orchard::Hash{81})==wallet::OrchardProofJobs::State::Succeeded;return 0;},&observed);
    const auto body=f.Finish(jobs);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(observed.commits,1);EXPECT_TRUE(observed.selected);EXPECT_TRUE(observed.retained);
    EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    const auto account=f.Account(3);const auto& entry=account.account.Operations().Entries().at(orchard::Hash{81});
    EXPECT_EQ(entry.phase,wallet::OrchardOperationQueue::Phase::Ready);EXPECT_EQ(entry.transaction,body);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);ASSERT_EQ(envelope.Inputs().size(),1u);
    ASSERT_EQ(envelope.Inputs()[0].witness.size(),1u);EXPECT_EQ(envelope.Inputs()[0].witness[0].size(),64u);
    const auto ready=f.Snapshot();wallet::OrchardProofJobs absent;EXPECT_EQ(f.Finish(absent),body);EXPECT_EQ(f.Snapshot(),ready);
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.Finish(absent),body);
    EXPECT_EQ(f.Snapshot(),ready);EXPECT_FALSE(f.Pay().success);
    // Actual admission/mining occurs only after Ready and outside wallet SQL
    // and selected ownership; full wallet recovery must observe the deposit.
    const auto included=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(included);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
}
TEST(WalletShieldReservations, FailedShieldReadyWriteAndCommitKeepExactProofAndReservation){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    ASSERT_TRUE(f.QueueSelected(jobs,f.Account(3).revision,f.Inputs())->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    const auto before=f.Snapshot();const auto original=f.ReadShield(jobs);ASSERT_TRUE(original.proof);
    f.Sql("CREATE TRIGGER refuse_shield_ready BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'shield Ready refusal'); END");
    EXPECT_THROW(f.Finish(jobs),std::runtime_error);f.Sql("DROP TRIGGER refuse_shield_ready");
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),original.proof->Bytes());
    auto* db=f.wallet->get().getCurrentDatabase();struct Fault{int commits=0;bool write=false;} fault;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
        if(action==SQLITE_UPDATE&&table&&std::string_view(table)=="orchard_wallet_snapshots")static_cast<Fault*>(p)->write=true;
        return SQLITE_OK;},&fault);
    sqlite3_commit_hook(db,[](void* p){auto& f=*static_cast<Fault*>(p);if(!f.write)return 0;++f.commits;return 1;},&fault);
    EXPECT_THROW(f.Finish(jobs),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(fault.commits,1);
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),original.proof->Bytes());EXPECT_FALSE(f.Pay().success);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(f.Finish(jobs).empty());EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
}
TEST(WalletShieldReservations, ShieldCompletionRejectsMissingJobChangedRequestAndForeignSession){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    ASSERT_TRUE(f.QueueSelected(jobs,f.Account(3).revision,f.Inputs())->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    const auto before=f.Snapshot();wallet::OrchardProofJobs absent;EXPECT_THROW(f.Finish(absent),std::runtime_error);
    {auto source=ChainstateService::AcquireWalletIndexUse(f.f.service);auto inputs=f.Inputs();++inputs[0].amount_una;
     EXPECT_THROW(f.f.service->finalizeRuntimeWalletShield(use->Wallet(),f.Selected().session,3,orchard::Hash{81},inputs,
         f.Payments(),std::vector<orchard::TransparentOutput>{{70000,f.script}},10000,jobs),std::runtime_error);}
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(jobs.Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    f.Reopen();EXPECT_THROW(f.Finish(jobs),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);
}
namespace {
size_t ShieldDetailsSuffix(const wallet::OrchardOperationQueue::Entry& entry){
    ShieldReservationFixture::Need(entry.shield_request.has_value());const auto& details=*entry.shield_request;
    size_t size=1+4+4+8;
    for(const auto& payment:details.payments)size+=8+4+payment.address.size()+payment.memo.size();
    for(const auto& output:details.outputs)size+=8+4+output.script_pub_key.size();
    return size;
}
std::vector<uint8_t> ShieldQueueBytes(const wallet::OrchardOperationQueue& queue){
    const auto encoded=queue.Encode();return {encoded.Bytes().begin(),encoded.Bytes().end()};
}
}
TEST(WalletShieldReservations, StoredDetailsSurviveReopenAndPreserveLegacyEncodings){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto queued=f.Queue(jobs);
    ASSERT_TRUE(queued->durable);const auto& entry=*queued->durable;ASSERT_TRUE(entry.shield_request);
    const auto& details=*entry.shield_request;ASSERT_EQ(details.payments.size(),1u);ASSERT_EQ(details.outputs.size(),1u);
    EXPECT_EQ(details.payments[0].amount_una,20000u);EXPECT_EQ(details.payments[0].address,f.Payments()[0].recipient.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(details.payments[0].memo,f.Payments()[0].memo);EXPECT_EQ(details.outputs[0].amount_una,70000u);
    EXPECT_EQ(details.outputs[0].script_pub_key,f.script);EXPECT_EQ(details.fee_una,10000u);
    const auto queue=f.Account(3).account.Operations();const auto bytes=ShieldQueueBytes(queue);ASSERT_EQ(bytes[7],'3');
    EXPECT_EQ(ShieldQueueBytes(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(bytes),f.Domain())),bytes);
    auto legacy=bytes;legacy.resize(legacy.size()-ShieldDetailsSuffix(entry));legacy[7]='2';
    const auto old=wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(legacy),f.Domain());
    EXPECT_FALSE(old.Entries().at(orchard::Hash{81}).shield_request);EXPECT_EQ(ShieldQueueBytes(old),legacy);
    legacy.resize(legacy.size()-33);legacy[7]='1';
    const auto unbound=wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(legacy),f.Domain());
    EXPECT_FALSE(unbound.Entries().at(orchard::Hash{81}).request_commitment);EXPECT_EQ(ShieldQueueBytes(unbound),legacy);
    jobs.RequestStop();const auto before=f.Snapshot();f.Reopen();const auto stored=f.Stored(f.Payments());
    ASSERT_TRUE(stored);EXPECT_TRUE(stored->existing_request);EXPECT_FALSE(stored->enqueued);ASSERT_TRUE(stored->durable);
    EXPECT_EQ(stored->durable->inputs[0].txid_wire,entry.inputs[0].txid_wire);
    EXPECT_EQ(stored->transparent_outputs[0].script_pub_key,details.outputs[0].script_pub_key);EXPECT_EQ(f.Snapshot(),before);
    auto changed=f.Payments();changed[0].memo[7]=1;EXPECT_THROW(f.Stored(changed),std::runtime_error);
    EXPECT_THROW(f.Stored(f.Payments(),10001),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletShieldReservations, StoredDetailsRejectMalformedAndUnknownOldRequests){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto queued=f.Queue(jobs);jobs.RequestStop();
    const auto original=ShieldQueueBytes(f.Account(3).account.Operations());const auto suffix=ShieldDetailsSuffix(*queued->durable);
    const auto start=original.size()-suffix;const auto before=f.Snapshot();
    for(size_t length=start;length<original.size();++length){
        const std::vector<uint8_t> truncated(original.begin(),original.begin()+length);
        EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(truncated),f.Domain()),std::runtime_error);
    }
    for(int mode:{0,1,2,3,4}){auto bad=original;
        if(mode==0)bad[start]=2;
        if(mode==1)for(size_t i=1;i<=4;++i)bad[start+i]=0xff;
        if(mode==2)bad[start+5+8+4]=0; // Invalid canonical recipient address.
        if(mode==3)bad.back()^=0x80; // Out-of-range fee.
        if(mode==4)bad.push_back(0); // Exact EOF required.
        EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(bad),f.Domain()),std::runtime_error);
    }
    EXPECT_EQ(f.Snapshot(),before);
    auto old=original;old.resize(start);old[7]='2';f.StoreQueueFixture(old);const auto legacy=f.Snapshot();
    EXPECT_THROW(f.Stored(f.Payments()),std::runtime_error);EXPECT_EQ(f.Snapshot(),legacy);
    // Explicit full-request retries retain their predecessor contract.
    wallet::OrchardProofJobs absent;const auto exact=f.Queue(absent);EXPECT_TRUE(exact->existing_request);EXPECT_FALSE(exact->enqueued);
    EXPECT_EQ(f.Snapshot(),legacy);f.Reopen();EXPECT_THROW(f.Stored(f.Payments()),std::runtime_error);EXPECT_EQ(f.Snapshot(),legacy);
}
TEST(WalletShieldReservations, AuthenticatedDetailsRequireOriginalRequestBinding){
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto queued=f.Queue(jobs);jobs.RequestStop();
    auto bytes=ShieldQueueBytes(f.Account(3).account.Operations());const auto& details=*queued->durable->shield_request;
    const auto start=bytes.size()-ShieldDetailsSuffix(*queued->durable);
    const auto memo=start+1+4+8+4+details.payments[0].address.size();bytes[memo+7]^=1;
    // A memo is structurally valid. Only the authenticated host's identity,
    // account and complete request digest can detect this semantic mismatch.
    EXPECT_NO_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(bytes),f.Domain()));
    f.StoreQueueFixture(bytes);const auto changed=f.Snapshot();
    EXPECT_THROW(f.Stored(f.Payments()),std::runtime_error);
    EXPECT_THROW(f.Account(3),std::runtime_error);
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),changed);
    f.Reopen();EXPECT_THROW(f.Stored(f.Payments()),std::runtime_error);EXPECT_EQ(f.Snapshot(),changed);
}
TEST(WalletShieldReservations, ReadyAndSelectedArchiveRetainCompleteShieldDetails){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    ASSERT_TRUE(f.QueueSelected(jobs,f.Account(3).revision,f.Inputs())->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    const auto body=f.Finish(jobs);const auto ready=f.Stored(f.Payments());ASSERT_TRUE(ready&&ready->durable&&ready->durable->shield_request);
    EXPECT_EQ(ready->durable->transaction,body);EXPECT_EQ(ready->durable->shield_request->fee_una,10000u);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(envelope)));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
    f.ArchiveConfirmedShield();
    const auto archived=f.Stored(f.Payments());ASSERT_TRUE(archived&&archived->durable&&archived->durable->shield_request);
    EXPECT_TRUE(archived->archived);ASSERT_TRUE(archived->observation);
    EXPECT_EQ(archived->durable->transaction,body);EXPECT_EQ(archived->transparent_outputs[0].amount_una,70000u);
    EXPECT_EQ(archived->durable->shield_request->payments[0].address,ready->durable->shield_request->payments[0].address);
    const auto before=f.Snapshot();f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    const auto reopened=f.Stored(f.Payments());ASSERT_TRUE(reopened&&reopened->durable);EXPECT_TRUE(reopened->archived);
    EXPECT_EQ(reopened->durable->transaction,body);EXPECT_EQ(f.Snapshot(),before);
}
} // namespace dinero
#endif
