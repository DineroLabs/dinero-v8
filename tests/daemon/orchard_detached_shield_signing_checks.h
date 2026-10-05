#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedShieldSigningTestAccess {
    using Points=std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>;
    static auto Sign(WalletManager& w,uint64_t session,const OrchardAccountDelivery::Profile& profile,
            const RuntimeAccountReplay& view,const orchard::Hash& id,
            std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
            std::span<const orchard::TransparentOutput> outputs,uint64_t fee,
            const consensus::OrchardCoinSnapshot& snapshot,OrchardProofJobs& jobs,const Points& points){
        return OrchardAccountDelivery::SignShieldWithRestorePoints(w,session,profile,view,id,
            inputs,payments,outputs,fee,snapshot,jobs,points);
    }
};
}
namespace dinero {
namespace {
struct DetachedShieldSigningFixture:ShieldHistoryFixture {
    std::shared_ptr<const RuntimeAccountReplay> view;
    uint64_t session=0;
    std::vector<orchard::ResolvedInput> inputs;
    std::vector<orchard::WalletPayment> payments;
    std::vector<orchard::TransparentOutput> outputs;
    DetachedShieldSigningFixture(){
        Complete();view=View();session=Selected().session;inputs=Inputs();payments=Payments();outputs={{70000,script}};
    }
    auto Capture(wallet::OrchardProofJobs& jobs,bool altered_output=false){
        const auto proof=ReadShield(jobs);Need(bool(proof.proof));
        std::vector<orchard::EnvelopeInput> raw;
        for(const auto& input:inputs)raw.push_back({input.txid_wire,input.output_index,input.sequence,{},{}});
        auto requested_outputs=outputs;if(altered_output)++requested_outputs.front().amount_una;
        const auto draft=orchard::TransactionEnvelope::Create(0,raw,requested_outputs,10000,proof.proof->Bytes());
        auto selected=f.service->AcquireBlockIngressActivationLock();
        const auto* coins=f.service->GetConsensusUTXOSet();const auto* tip=f.service->GetActiveTip();
        Need(coins&&tip&&tip->height>=0&&coins->GetHeight()==uint32_t(tip->height));
        CurrentSpendView current(*coins);
        return std::make_unique<const consensus::OrchardCoinSnapshot>(
            consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(draft,current));
    }
    auto Sign(wallet::OrchardProofJobs& jobs,const consensus::OrchardCoinSnapshot& snapshot,
            const wallet::OrchardDetachedShieldSigningTestAccess::Points& points,uint64_t fee=10000){
        auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardDetachedShieldSigningTestAccess::Sign(use->Wallet(),session,{Domain(),102,3},
            *view,orchard::Hash{81},inputs,payments,outputs,fee,snapshot,jobs,points);
    }
    void Verify(const orchard::TransactionEnvelope& envelope){
        auto selected=f.service->AcquireBlockIngressActivationLock();
        const auto* coins=f.service->GetConsensusUTXOSet();const auto* tip=f.service->GetActiveTip();
        Need(coins&&tip&&tip->height>=0&&coins->GetHeight()==uint32_t(tip->height));
        CurrentSpendView current(*coins);
        const auto snapshot=consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(envelope,current);
        (void)consensus::VerifyOrchardAuthorizations(snapshot,Domain(),uint32_t(tip->height)+1,{});
    }
};
}
TEST(OrchardDetachedShieldSigning, RestoresOutsideOwnersSignsExactInputsWithoutPublishingReady){
    DetachedShieldSigningFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto snapshot=f.Capture(jobs);const auto proof=f.ReadShield(jobs);const auto before=f.Snapshot();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        auto point=f.view->Point(cursor);const auto original=point.lookups.origin;
        point.lookups.origin=[&,original](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
            EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
            return original(height,hash,txid);
        };return point;
    };
    auto result=f.Sign(jobs,*snapshot,provider);
    ASSERT_TRUE(result);
    EXPECT_GT(points,0u);
    ASSERT_EQ(result->Inputs().size(),1u);
    ASSERT_EQ(result->Inputs()[0].witness.size(),1u);
    EXPECT_EQ(result->Inputs()[0].witness[0].size(),64u);
    EXPECT_NO_THROW(f.Verify(*result));
    EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),proof.proof->Bytes());
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    EXPECT_EQ(f.Account(3).account.Operations().Entries().at(orchard::Hash{81}).phase,wallet::OrchardOperationQueue::Phase::Reserved);
    // The actual service still retains Ready/history before admission and mining.
    const auto body=f.Finish(jobs);
    EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
    EXPECT_NO_THROW(f.Verify(envelope));
    ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(envelope)));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
}
TEST(OrchardDetachedShieldSigning, CaptureRecheckKeyReadAndCommitRefusalsPreserveProof){
    DetachedShieldSigningFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto snapshot=f.Capture(jobs);const auto exact=f.ReadShield(jobs).proof->Bytes();const auto before=f.Snapshot();
    auto* db=wallet.getCurrentDatabase();size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;return f.view->Point(cursor);};
    bool denied=false;sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;
    },&denied);
    std::unique_ptr<orchard::TransactionEnvelope> result;
    EXPECT_THROW(result=f.Sign(jobs,*snapshot,provider),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(denied);
    EXPECT_EQ(points,0u);
    EXPECT_FALSE(result);
    for(int fault:{0,1}){
        DetachedCatalogSqlHooks hooks(db);hooks.deny_final_read=fault==0;hooks.refuse_final_commit=fault==1;
        EXPECT_THROW(result=f.Sign(jobs,*snapshot,provider),std::runtime_error);
        EXPECT_FALSE(result);
    EXPECT_EQ(hooks.commits,fault==0?1u:2u);
    }
    struct KeyRead {bool denied=false;size_t commits=0;} key_read;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        auto& o=*static_cast<KeyRead*>(p);
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT")++o.commits;
        if(o.commits&&action==SQLITE_READ&&name&&(std::string_view(name)=="taproot_keys"||std::string_view(name)=="address_derivation_paths")){
            o.denied=true;return SQLITE_DENY;
        }return SQLITE_OK;
    },&key_read);
    EXPECT_THROW(result=f.Sign(jobs,*snapshot,provider),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(key_read.denied);
    EXPECT_FALSE(result);
    EXPECT_TRUE(sqlite3_get_autocommit(db));
    EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),exact);
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    EXPECT_NO_THROW(result=f.Sign(jobs,*snapshot,provider));
    ASSERT_TRUE(result);
    EXPECT_NO_THROW(f.Verify(*result));
}
TEST(OrchardDetachedShieldSigning, OtherAccountChangeDuringRestorationRefusesBeforeSigning){
    DetachedShieldSigningFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto snapshot=f.Capture(jobs);const auto original=f.Account(3);const auto exact=f.ReadShield(jobs).proof->Bytes();bool changed=false;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        if(!changed){changed=true;(void)wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(
            wallet,f.session,{f.Domain(),102,17},*f.view,orchard::WalletScope::External);}
        return f.view->Point(cursor);
    };
    std::unique_ptr<orchard::TransactionEnvelope> result;
    EXPECT_THROW(result=f.Sign(jobs,*snapshot,provider),wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_TRUE(changed);
    EXPECT_FALSE(result);const auto after=f.Snapshot();
    EXPECT_EQ(f.Account(3).revision,original.revision);
    EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),exact);
    const auto retry=[&](RuntimeOutboxCursor cursor){return f.view->Point(cursor);};
    EXPECT_NO_THROW(result=f.Sign(jobs,*snapshot,retry));
    ASSERT_TRUE(result);
    EXPECT_NO_THROW(f.Verify(*result));
    EXPECT_EQ(f.Snapshot(),after);
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
}
TEST(OrchardDetachedShieldSigning, BorrowedOwnersLockedWalletAndSourceRefusalReturnNoSignature){
    DetachedShieldSigningFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto snapshot=f.Capture(jobs);const auto before=f.Snapshot();const auto exact=f.ReadShield(jobs).proof->Bytes();
    auto* db=wallet.getCurrentDatabase();size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;return f.view->Point(cursor);};
    {auto lease=wallet.AcquireDatabaseLease();
    EXPECT_THROW((void)f.Sign(jobs,*snapshot,provider),std::runtime_error);}
    ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)f.Sign(jobs,*snapshot,provider),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));
    ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    wallet.lockWallet();
    EXPECT_THROW((void)f.Sign(jobs,*snapshot,provider),std::runtime_error);
    wallet.unlockWallet("canonical-fixture-pass",0);
    EXPECT_EQ(points,0u);
    const auto refused=[&](RuntimeOutboxCursor)->wallet::OrchardAccountDelivery::RestorePoint{
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));throw std::runtime_error("isolated unavailable signing source");
    };
    EXPECT_THROW((void)f.Sign(jobs,*snapshot,refused),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),exact);
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    auto result=f.Sign(jobs,*snapshot,provider);
    ASSERT_TRUE(result);
    EXPECT_NO_THROW(f.Verify(*result));
}
TEST(OrchardDetachedShieldSigning, ExactRequestSnapshotAndExecutorOwnerRemainMandatory){
    DetachedShieldSigningFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto snapshot=f.Capture(jobs);const auto altered=f.Capture(jobs,true);const auto exact=f.ReadShield(jobs).proof->Bytes();const auto before=f.Snapshot();
    const auto provider=[&](RuntimeOutboxCursor cursor){return f.view->Point(cursor);};
    wallet::OrchardProofJobs absent;
    EXPECT_THROW((void)f.Sign(absent,*snapshot,provider),std::runtime_error);
    EXPECT_THROW((void)f.Sign(jobs,*snapshot,provider,10001),std::runtime_error);
    EXPECT_THROW((void)f.Sign(jobs,*altered,provider),std::runtime_error);
    EXPECT_THROW((void)wallet::OrchardDetachedShieldSigningTestAccess::Sign(wallet,f.session+1,{f.Domain(),102,3},
        *f.view,orchard::Hash{81},f.inputs,f.payments,f.outputs,10000,*snapshot,jobs,provider),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),exact);
    auto result=f.Sign(jobs,*snapshot,provider);
    ASSERT_TRUE(result);
    EXPECT_NO_THROW(f.Verify(*result));
}
} // namespace dinero
#endif
