#pragma once
#include "rpc/orchard_account_rpc.h"
namespace dinero {
namespace {
using Recovery=WalletService::CanonicalRecovery;
struct CanonicalRecoveryHistoricalFixture : WalletIndexOwnerFixture {
    std::shared_ptr<WalletService> wallet=std::make_shared<WalletService>();
    CanonicalRecoveryHistoricalFixture() {
        context.wallet=wallet;
        if(!wallet->Init(context))throw std::runtime_error("actual wallet Init");
        auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(source);
        use->Wallet().setUTXOIndex(&index->Index());use->Wallet().create("historical-recovery");
    }
    ~CanonicalRecoveryHistoricalFixture(){wallet->Stop();context.wallet.reset();wallet.reset();}
};
}
TEST(WalletCanonicalRecovery, MissingOwnersAndConfiguredUnavailableSourceDefer) {
    auto missing=std::make_shared<WalletService>();std::string error;
    EXPECT_EQ(missing->RecoverActiveWalletFromCanonicalSource(&error),Recovery::Deferred);EXPECT_FALSE(error.empty());
    CanonicalRecoveryHistoricalFixture f;
    MutableParams().orchard_activation_height=102;MutableParams().orchard_branch_id=1;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),Recovery::Deferred);EXPECT_FALSE(error.empty());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
    EXPECT_TRUE(sqlite3_get_autocommit(lease->Database()));
    EXPECT_EQ(use->Wallet().getCurrentWalletName(),"historical-recovery");
}
TEST(WalletCanonicalRecovery, UnsetProfileAndBorrowedLeasePreserveHistoricalWallet) {
    CanonicalRecoveryHistoricalFixture f;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    {auto lease=use->Wallet().AcquireDatabaseLease();const auto session=lease->Session();
     EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
     EXPECT_EQ(lease->Session(),session);EXPECT_TRUE(sqlite3_get_autocommit(lease->Database()));}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    EXPECT_TRUE(f.wallet->RecoverActiveWalletFromSnapshotIfNeeded());
    {auto selected=f.source->AcquireBlockIngressActivationLock();
     EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    const auto previous_session=use->Wallet().AcquireDatabaseLease()->Session();
    use->Wallet().open("historical-recovery");
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(nullptr,previous_session),Recovery::Deferred);
    EXPECT_FALSE(f.wallet->RecoverActiveWalletFromSnapshotIfNeeded(nullptr,previous_session));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct CanonicalRecoveryFixture : OrchardCycleFixture {
    std::shared_ptr<WalletService> wallet=std::make_shared<WalletService>();
    explicit CanonicalRecoveryFixture(bool encrypted=false) {
        // Stop the old adapter before Init replaces its coin owner. Retain the
        // stopped service for base-fixture cleanup if initialization throws.
        f.ingress->Stop();context.mempool.reset();
        auto config=std::make_shared<ConfigService>();config->Set("datadir",(f.path/"wallet-runtime").string());
        context.config=config;context.logger=std::make_shared<LoggerService>("");context.logger_interface=f.logger.get();
        context.block_storage=files;
        OrchardAdmissionFixture::Require(f.service->Init(context));
        ShieldedStateStartupTestAccess::PopulateInitializedParent(*f.service,*parent,*f.replay);
        f.ingress=std::make_shared<MempoolService>();OrchardAdmissionFixture::Require(f.ingress->Init(context));context.mempool=f.ingress;
        context.wallet=wallet;OrchardAdmissionFixture::Require(wallet->Init(context));
        {auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
         use->Wallet().setUTXOIndex(&index->Index());use->Wallet().create("canonical-recovery");
         (void)use->Wallet().getNewAddress();
         if(encrypted){use->Wallet().encryptWallet("canonical-fixture-pass");use->Wallet().unlockWallet("canonical-fixture-pass",0);}}
    }
    ~CanonicalRecoveryFixture(){wallet->Stop();context.wallet.reset();wallet.reset();}
    uint64_t Session(){auto use=WalletService::AcquireWalletUse(wallet);return use->Wallet().AcquireDatabaseLease()->Session();}
    auto Keys(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto seed=lease->CopyRecoverySeed(lease->Session());return orchard::WalletKeys::FromSeed(seed->Bytes(),0);}
    void EnrollAccount() {
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto seed=lease->CopyRecoverySeed(lease->Session());const auto keys=orchard::WalletKeys::FromSeed(seed->Bytes(),0);
        const auto encoded=lease->EnsureDeliveryIdentity();orchard::Hash identity{};
        for(size_t i=0;i<32;++i)identity[i]=uint8_t(std::stoul(encoded.substr(7+2*i,2),nullptr,16));
        auto account=dinero::wallet::OrchardAccountState::Begin(Domain(),keys.ExportFullViewingKey(),102,parent->hash)
            .IssueReceiver(orchard::WalletScope::External).first;
        OrchardAdmissionFixture::Require(sqlite3_exec(lease->Database(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
        orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());
        orchard::WalletSnapshotStore store(lease->Database(),{orchard::WalletNetwork::Regtest,Domain().genesis_wire,identity,0},seed->Bytes());
        OrchardAdmissionFixture::Require(store.StageReplaceRetaining(0,account.Encode())==1);
        OrchardAdmissionFixture::Require(sqlite3_exec(lease->Database(),"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);
    }
    void MineAndAdopt() {
        const auto keys=Keys();const auto [body,bundle]=Shield(keys);(void)Mine(body);
        ExecutionContext request;request.daemon=&context;request.walletName="canonical-recovery";
        din::Json params;params["account"]=Json::UInt64(0);
        const auto created=rpc_context_wallet_orchard_createaccount(request,params);
        OrchardAdmissionFixture::Require(!created.isMember("error"));
        auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto origin=f.service->getRuntimeWalletOrigin(use->Wallet(),session,&index->Index());
        OrchardAdmissionFixture::Require(origin.ok());
        OrchardAdmissionFixture::Require(f.service->adoptRuntimeWalletOrigin(use->Wallet(),index->Index(),**origin)==Status::Ok);
    }
    auto ReadAccount() {
        // Source captured outside wallet database/key ownership.
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        auto use=WalletService::AcquireWalletUse(wallet);const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        return dinero::wallet::OrchardAccountDelivery::ReadForReplay(use->Wallet(),session,{Domain(),102,0},**view);
    }
    void MineEmpty() {
        BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
        const auto block=assembler.CreateOrchardBlock(OrchardMiningPayout);OrchardAdmissionFixture::Require(bool(block));
        const auto result=Submit(block->WireBytes());OrchardAdmissionFixture::Require(result.accepted()&&result.connected);
    }
};
}
TEST(WalletCanonicalRecovery, EarnedPrefixAndEncryptedUnlockRestoreActualNote) {
    CanonicalRecoveryFixture f(true);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    f.MineAndAdopt();
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    auto recovered=f.ReadAccount();EXPECT_EQ(recovered.account.Scan().BalanceUna(),500000u);
    ASSERT_EQ(recovered.account.Scan().Notes().size(),1u);EXPECT_EQ(recovered.account.Delivery().sequence,1u);
    const auto first_session=f.Session();
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");EXPECT_TRUE(use->Wallet().isLocked());}
    // Match the existing wallet.open service flow: reopen clears the live
    // script map, then checked binding reload restores the recorded inventory.
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    EXPECT_NE(f.Session(),first_session);EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    ExecutionContext context;context.daemon=&f.context;din::Json request=din::arr();request.append("canonical-fixture-pass");
    const auto unlocked=::rpc_context_wallet_unlock(context,request);
    EXPECT_TRUE(unlocked["success"].asBool());EXPECT_FALSE(unlocked.isMember("recovery_warning")) << unlocked["recovery_warning"].asString();
    EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
    // A retained canonical disconnect remains authoritative even after the
    // selected tip moves below activation. It must not select legacy scanning.
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(f.f.service->GetActiveTip()->height,f.parent->height);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto disconnected=f.ReadAccount();
    EXPECT_EQ(disconnected.account.Scan().BalanceUna(),0u);
    EXPECT_EQ(disconnected.account.Delivery().sequence,2u);
}
TEST(WalletCanonicalRecovery, ActualStartupResumesLaggingStoresWithoutHeightReset) {
    CanonicalRecoveryFixture f;f.MineAndAdopt();f.MineEmpty();const auto before=f.Session();
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
    ASSERT_TRUE(f.wallet->Start());EXPECT_NE(f.Session(),before);
    const auto account=f.ReadAccount();EXPECT_EQ(account.account.Scan().BalanceUna(),500000u);
    EXPECT_EQ(account.account.Delivery().sequence,2u);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
    const auto indexed=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session);
    ASSERT_TRUE(ordinary);ASSERT_TRUE(indexed);EXPECT_EQ(ordinary->cursor.sequence,2u);EXPECT_EQ(indexed->cursor,ordinary->cursor);
}
TEST(WalletCanonicalRecovery, MissingBaselineNeverEnrollsOrUsesLegacySnapshotPath) {
    CanonicalRecoveryFixture f;f.EnrollAccount();const auto keys=f.Keys();const auto [body,bundle]=f.Shield(keys);(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    EXPECT_FALSE(f.wallet->RecoverActiveWalletFromSnapshotIfNeeded());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    EXPECT_FALSE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session));
    EXPECT_FALSE(RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session));
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
}
TEST(WalletCanonicalRecovery, RequiredWriteFailureRetainsPrefixForRetry) {
    CanonicalRecoveryFixture f;f.MineAndAdopt();
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     ASSERT_EQ(sqlite3_exec(lease->Database(),"CREATE TRIGGER refuse_recovery_account BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'fixture required write refusal'); END",nullptr,nullptr,nullptr),SQLITE_OK);}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     ASSERT_EQ(sqlite3_exec(lease->Database(),"DROP TRIGGER refuse_recovery_account",nullptr,nullptr,nullptr),SQLITE_OK);}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
}
#endif
} // namespace dinero
