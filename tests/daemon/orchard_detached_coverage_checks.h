#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedCoverageTestAccess {
    static auto Prepare(WalletManager& wallet,uint64_t session,std::shared_ptr<const RuntimeAccountReplay> replay,
            const std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>& points){
        return OrchardAccountDelivery::PrepareCatalogWithRestorePoints(wallet,session,std::move(replay),points);
    }
    static void Recheck(WalletManager& wallet,uint64_t session,
            const std::shared_ptr<const RuntimeAccountReplay>& replay,const OrchardCatalogCapture& capture){
        OrchardAccountDelivery::RecheckCatalogCaptureInTransaction(wallet,session,replay,capture);
    }
};
}
namespace dinero {
namespace {
struct CoverageOwnerSqlObserver {
    sqlite3* db;ChainstateService& source;bool deny_selected;
    size_t outside_reads=0,selected_reads=0;
    CoverageOwnerSqlObserver(sqlite3* connection,ChainstateService& service,bool deny)
        :db(connection),source(service),deny_selected(deny){
        OrchardAdmissionFixture::Require(sqlite3_set_authorizer(db,
            [](void* value,int action,const char* table,const char*,const char*,const char*){
                auto& self=*static_cast<CoverageOwnerSqlObserver*>(value);
                if(action!=SQLITE_READ||!table||std::string_view(table)!="orchard_wallet_snapshots")return SQLITE_OK;
                // SQL-free same-thread observation; no reentrant database work.
                if(ShieldedStateStartupTestAccess::VaultSelectedHeld(self.source)){
                    ++self.selected_reads;return self.deny_selected?SQLITE_DENY:SQLITE_OK;
                }
                ++self.outside_reads;return SQLITE_OK;
            },this)==SQLITE_OK);
    }
    ~CoverageOwnerSqlObserver(){sqlite3_set_authorizer(db,nullptr,nullptr);}
};
}
TEST(OrchardDetachedCoverage, PreparedRestoreReleasesOwnersAndTransactionRecheckDoesNotRestore){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(17));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    const auto before=f.Snapshot();size_t points=0,origins=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        auto point=(*view)->Point(cursor);const auto lookup=point.lookups.origin;
        point.lookups.origin=[&,lookup](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
            return lookup(height,hash,txid);
        };
        return point;
    };
    const auto prepared=wallet::OrchardDetachedCoverageTestAccess::Prepare(wallet,session,*view,provider);
    ASSERT_TRUE(prepared);EXPECT_GT(points,0u);EXPECT_GT(origins,0u);const auto restored_points=points,restored_origins=origins;
    {
        auto lease=wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_NO_THROW(wallet::OrchardDetachedCoverageTestAccess::Recheck(wallet,session,*view,*prepared));
        EXPECT_FALSE(sqlite3_get_autocommit(db));EXPECT_EQ(points,restored_points);EXPECT_EQ(origins,restored_origins);
        ASSERT_EQ(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_THROW(wallet::OrchardDetachedCoverageTestAccess::Recheck(wallet,session,*view,*prepared),std::runtime_error);
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_EQ(f.Snapshot(),before);
    {
        auto lease=wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_THROW(wallet::OrchardDetachedCoverageTestAccess::Prepare(wallet,session,*view,provider),std::runtime_error);
        EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(points,restored_points);EXPECT_EQ(origins,restored_origins);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardDetachedCoverage, ExactReplaySessionAndCatalogChangesRefuseBeforeEffects){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    const auto view=f.f.service->getRuntimeAccountReplay();const auto other=f.f.service->getRuntimeAccountReplay();
    ASSERT_TRUE(view.ok());ASSERT_TRUE(other.ok());ASSERT_NE(view->get(),other->get());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto session=f.Session();auto* db=wallet.getCurrentDatabase();
    const auto provider=[&](RuntimeOutboxCursor cursor){return (*view)->Point(cursor);};
    const auto prepared=wallet::OrchardDetachedCoverageTestAccess::Prepare(wallet,session,*view,provider);
    const auto before=f.Snapshot();
    {
        auto lease=wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_THROW(wallet::OrchardDetachedCoverageTestAccess::Recheck(wallet,session,*other,*prepared),std::runtime_error);
        EXPECT_THROW(wallet::OrchardDetachedCoverageTestAccess::Recheck(wallet,session+1,*view,*prepared),std::runtime_error);
        EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(f.Snapshot(),before);ASSERT_FALSE(f.Call(29).isMember("error"));const auto changed=f.Snapshot();
    {
        auto lease=wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_THROW(wallet::OrchardDetachedCoverageTestAccess::Recheck(wallet,session,*view,*prepared),std::runtime_error);
        EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(f.Snapshot(),changed);
    const auto retry=wallet::OrchardDetachedCoverageTestAccess::Prepare(wallet,session,*view,provider);
    {
        auto lease=wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_NO_THROW(wallet::OrchardDetachedCoverageTestAccess::Recheck(wallet,session,*view,*retry));
        ASSERT_EQ(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(f.Snapshot(),changed);
}
TEST(OrchardDetachedCoverage, ActualCoverageCapturesBeforeSelectedAndRefusesFinalReadBeforeIndex){
    SharedPaymentFixture f;f.Create(3);f.Create(17);(void)f.wallet->get().getNewChangeAddress("","taproot");
    const auto source=CaptureScriptCoverage(f);ASSERT_TRUE(source.ok());
    const auto before=f.Snapshot();const auto indexed=CoverageIndexSnapshot(f);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    auto* db=use->Wallet().getCurrentDatabase();
    {
        CoverageOwnerSqlObserver observer(db,*f.f.service,true);
        EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
        EXPECT_GT(observer.outside_reads,0u);EXPECT_GT(observer.selected_reads,0u);
    }
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);
    {
        CoverageOwnerSqlObserver observer(db,*f.f.service,false);
        EXPECT_EQ(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
        EXPECT_GT(observer.outside_reads,0u);EXPECT_GT(observer.selected_reads,0u);
    }
    EXPECT_FALSE(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(use->Wallet(),index->Index(),f.Selected().session));
    const auto completed=f.Snapshot();const auto completed_index=CoverageIndexSnapshot(f);
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
    }
    EXPECT_EQ(f.Snapshot(),completed);EXPECT_EQ(CoverageIndexSnapshot(f),completed_index);
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(use->Wallet()));
}
TEST(OrchardDetachedCoverage, PrefixFactsBindDigestsAcrossDisconnectAndReconnect){
    ShieldHistoryFixture f;f.Complete();const auto body=f.FinishOwned();
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
    const auto included=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(included);
    const auto captured=CaptureScriptCoverage(f);ASSERT_TRUE(captured.ok());
    const auto captured_view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(captured_view.ok());
    ASSERT_EQ((*captured)->Head(),(*captured_view)->Head());
    const auto verify=[&](const auto& projection,const auto& replay){
        ASSERT_EQ(projection->Head(),replay->Head());
        for(uint64_t sequence=1;sequence<=replay->Head().sequence;++sequence){
            const auto cursor=replay->Event(sequence)->cursor;
            const auto expected=replay->Point(cursor).checkpoint;
            const auto tip=projection->PrefixTip(cursor);ASSERT_TRUE(tip);
            EXPECT_EQ(tip->first,expected.block_hash);EXPECT_EQ(tip->second,expected.height);
            auto altered=cursor;altered.digest.begin()[0]^=1;
            EXPECT_FALSE(projection->PrefixTip(altered));
        }
        EXPECT_FALSE(projection->PrefixTip({}));
        auto future=projection->Head();++future.sequence;
        EXPECT_FALSE(projection->PrefixTip(future));
    };
    const auto before=f.Snapshot();const auto indexed=CoverageIndexSnapshot(f);
    verify(*captured,*captured_view);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);
    f.Disconnect();
    const auto disconnected=CaptureScriptCoverage(f);ASSERT_TRUE(disconnected.ok());
    const auto disconnected_view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(disconnected_view.ok());
    verify(*disconnected,*disconnected_view);verify(*captured,*captured_view);
    ASSERT_GT((*disconnected)->Head().sequence,(*captured)->Head().sequence);
    EXPECT_FALSE((*captured)->PrefixTip((*disconnected)->Head()));
    const auto retained=(*disconnected)->PrefixTip((*captured)->Head());ASSERT_TRUE(retained);
    EXPECT_EQ(retained->first,(*captured)->TipHash());EXPECT_EQ(retained->second,(*captured)->TipHeight());
    const auto reattached=f.Submit(included->Orchard().WireBytes());
    ASSERT_TRUE(reattached.accepted()&&reattached.connected)<<reattached.reason;
    const auto reconnected=CaptureScriptCoverage(f);ASSERT_TRUE(reconnected.ok());
    const auto reconnected_view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(reconnected_view.ok());
    verify(*reconnected,*reconnected_view);verify(*disconnected,*disconnected_view);
    ASSERT_GT((*reconnected)->Head().sequence,(*disconnected)->Head().sequence);
    EXPECT_EQ((*reconnected)->TipHash(),(*captured)->TipHash());
    EXPECT_NE((*reconnected)->Head().digest,(*captured)->Head().digest);
    auto substituted=(*reconnected)->Head();substituted.digest=(*captured)->Head().digest;
    EXPECT_FALSE((*reconnected)->PrefixTip(substituted));
}
} // namespace dinero
#endif
