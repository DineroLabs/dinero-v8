#pragma once
#include "daemon/services/logger_service.h"
#include "wallet/wallet_worker.h"
#include "wallet/wallet_index_v3_schema.h"
namespace dinero {
namespace {
struct WalletIndexOwnerFixture {
    ChainParams params=Params();
    DaemonContext* previous=DaemonContext::instance();
    ServiceOwnerLogger wallet_logger;
    DaemonContext context;
    std::filesystem::path root;
    ChainDB db;
    std::shared_ptr<ChainstateService> source;
    explicit WalletIndexOwnerFixture(bool full_archive_v3=false,bool preserve_v2_coin=false) {
        SelectParams(Chain::REGTEST);
        char path[]="/tmp/dinero-wallet-index-owner-XXXXXX";
        if(!mkdtemp(path))throw std::runtime_error("fixture directory");root=path;
        if(db.init((root/"chain").string())!=Status::Ok)throw std::runtime_error("fixture database");
        if(full_archive_v3) {
            const auto index_path=root/"blockchain"/"utxo";
            std::filesystem::create_directories(index_path.parent_path());
            {
                UTXOIndex legacy(index_path.string());
                if(!legacy.Initialize()||!legacy.SetMetadata("v3_startup_probe","preserved"))
                    throw std::runtime_error("fixture legacy wallet index");
                if(preserve_v2_coin) {
                    uint256 hash;hash.begin()[0]=7;
                    const std::vector<uint8_t> script{0x51};
                    const std::string path="m/86'/1448'/0'/0/7";
                    legacy.RegisterAddress(script,path);
                    if(!legacy.AddUTXO(WalletUTXO(TxId(hash),0,AmountUna::Una(77),script,path,7)))
                        throw std::runtime_error("fixture legacy wallet coin");
                }
            }
            (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
        }
        auto config=std::make_shared<ConfigService>();config->Set("datadir",root.string());
        auto logger=std::make_shared<LoggerService>("");
        context.config=config;context.logger=logger;context.logger_interface=&wallet_logger;
        source=std::make_shared<ChainstateService>();context.chainstate=source;
        DaemonContext::setInstance(&context);source->setChainDB(&db);
        if(!source->Init(context))throw std::runtime_error("fixture actual chainstate Init");
    }
    ~WalletIndexOwnerFixture() {
        if(source)source->Stop();context.chainstate.reset();source.reset();
        BlockAcceptor::SetContext(previous);DaemonContext::setInstance(previous);
        db.close();MutableParams()=params;std::filesystem::remove_all(root);
    }
};
struct WalletServiceIndexFixture : WalletIndexOwnerFixture {
    std::shared_ptr<WalletService> wallet=std::make_shared<WalletService>();
    bool v3;
    explicit WalletServiceIndexFixture(bool use_v3,bool preserve_v2_coin=false)
        :WalletIndexOwnerFixture(use_v3,preserve_v2_coin),v3(use_v3) {
        context.wallet=wallet;
        if(!wallet->Init(context))throw std::runtime_error("fixture wallet service Init");
        wallet->get().create("startup-owner");
    }
    ~WalletServiceIndexFixture(){
        if(v3)wallet->get().setUTXOIndex(nullptr);
        wallet->Stop();context.wallet.reset();wallet.reset();
    }
};
void LegacyizeForV3Enrollment(WalletManager& wallet) {
    auto* db=wallet.getCurrentDatabase();
    if(!db || sqlite3_exec(db,
        "DELETE FROM settings WHERE key IN ('orchard_account_catalog_v1','wallet_initial_owner_v1');"
        "DROP TABLE IF EXISTS orchard_wallet_retained;"
        "DROP TABLE IF EXISTS orchard_wallet_snapshots;"
        "DROP TABLE IF EXISTS orchard_wallet_schema;"
        "DROP TABLE IF EXISTS shielded_state_v1;",
        nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("fixture legacy wallet setup");
    sqlite3_stmt* column=nullptr;
    if(sqlite3_prepare_v2(db,
        "SELECT 1 FROM pragma_table_info('wallet_meta') WHERE name='runtime_delivery_id'",
        -1,&column,nullptr)!=SQLITE_OK)
        throw std::runtime_error("fixture identity column read");
    const int found=sqlite3_step(column);
    if(found!=SQLITE_ROW&&found!=SQLITE_DONE) {
        sqlite3_finalize(column);
        throw std::runtime_error("fixture identity column read");
    }
    sqlite3_finalize(column);
    if(found==SQLITE_ROW && sqlite3_exec(db,
        "ALTER TABLE wallet_meta DROP COLUMN runtime_delivery_id",
        nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("fixture identity removal");
}
int V3PreservedLegacyCoinCount(const std::filesystem::path& index_path) {
    sqlite3* raw=nullptr;
    if(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)
        throw std::runtime_error("fixture v3 index read");
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    sqlite3_stmt* statement=nullptr;
    if(sqlite3_prepare_v2(db.get(),"SELECT count(*) FROM wallet_utxos_legacy_v2",-1,&statement,nullptr)!=SQLITE_OK)
        throw std::runtime_error("fixture v2 coin read");
    const std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(statement,sqlite3_finalize);
    if(sqlite3_step(q.get())!=SQLITE_ROW)throw std::runtime_error("fixture v2 coin count");
    const int count=sqlite3_column_int(q.get(),0);
    if(sqlite3_step(q.get())!=SQLITE_DONE)throw std::runtime_error("fixture v2 coin count incomplete");
    return count;
}
}
TEST(ChainstateWalletIndexOwner, FullArchiveV3OpensAuthenticatedMetadataOnly) {
    WalletIndexOwnerFixture f(true);
    ASSERT_TRUE(f.source->HasFullArchiveWalletIndexV3());
    auto index=ChainstateService::AcquireWalletIndexUse(f.source);
    EXPECT_TRUE(index->Index().IsFullArchiveV3MetadataOnly());
    EXPECT_EQ(index->Index().GetMetadata("v3_startup_probe"),std::optional<std::string>("preserved"));
    EXPECT_THROW((void)index->Index().GetBalance(),std::runtime_error);
    EXPECT_EQ(ReadWalletIndexSchemaVersionReadOnly(f.root/"blockchain"/"utxo"),3);
}
TEST(ChainstateWalletIndexOwner, FullArchiveV3RefusesMissingAuthenticatedBackup) {
    WalletIndexOwnerFixture f(true);
    const auto index_path=f.root/"blockchain"/"utxo";
    const auto backup=RequireFullArchiveWalletIndexV3(index_path).rollback_backup;
    f.source->Stop();f.context.chainstate.reset();f.source.reset();
    ASSERT_TRUE(std::filesystem::remove(backup));
    f.source=std::make_shared<ChainstateService>();f.context.chainstate=f.source;
    f.source->setChainDB(&f.db);
    EXPECT_FALSE(f.source->Init(f.context));
    EXPECT_EQ(ReadWalletIndexSchemaVersionReadOnly(index_path),3);
}
TEST(ChainstateWalletIndexOwner, V3WalletServiceRefusesLegacyWorkerAndIndexBinding) {
    WalletServiceIndexFixture f(true);
    ASSERT_FALSE(WalletNotify::IsInitialized());
    ASSERT_TRUE(f.wallet->Start());
    EXPECT_FALSE(WalletNotify::IsInitialized());
    EXPECT_EQ(f.wallet->get().getUTXOIndex(),nullptr);
    EXPECT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    std::string error;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::Deferred);
    EXPECT_NE(error.find("canonical wallet recovery deferred"),std::string::npos);
    EXPECT_FALSE(f.wallet->RecoverActiveWalletFromSnapshotIfNeeded(&error));
    EXPECT_NE(error.find("v3 owner-scoped recovery"),std::string::npos);
    EXPECT_THROW(f.wallet->get().setUTXOIndex(f.source->utxoIndex()),std::runtime_error);
    EXPECT_EQ(f.wallet->get().getUTXOIndex(),nullptr);
    EXPECT_THROW(WalletNotify::Initialize(f.source->utxoIndex(),&f.wallet->get()),std::runtime_error);
    EXPECT_FALSE(WalletNotify::IsInitialized());
}
TEST(ChainstateWalletIndexOwner, V2WalletServiceRetainsLegacyWorkerAndIndexBinding) {
    WalletServiceIndexFixture f(false);
    ASSERT_FALSE(WalletNotify::IsInitialized());
    ASSERT_TRUE(f.wallet->Start());
    EXPECT_TRUE(WalletNotify::IsInitialized());
    EXPECT_EQ(f.wallet->get().getUTXOIndex(),f.source->utxoIndex());
    EXPECT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
}
TEST(ChainstateWalletIndexOwner, V3WalletSwitchAndAddressCreationRemainOffLegacyIndex) {
    WalletServiceIndexFixture f(true);
    ASSERT_TRUE(f.wallet->Start());
    ASSERT_FALSE(f.wallet->get().getNewAddress("first-v3").empty());
    f.wallet->get().create("second-owner");
    ASSERT_FALSE(f.wallet->get().getNewAddress("second-v3").empty());
    EXPECT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    f.wallet->get().open("startup-owner");
    EXPECT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    EXPECT_EQ(f.wallet->get().getUTXOIndex(),nullptr);
    EXPECT_FALSE(WalletNotify::IsInitialized());
    EXPECT_EQ(f.source->utxoIndex()->GetMetadata("v3_startup_probe"),
        std::optional<std::string>("preserved"));
}
TEST(ChainstateWalletIndexOwner, V3WalletServiceRefusesExistingLegacyWorker) {
    WalletServiceIndexFixture f(true);
    UTXOIndex old_index((f.root/"old-worker-index").string());
    ASSERT_TRUE(old_index.Initialize());
    ASSERT_FALSE(WalletNotify::IsInitialized());
    WalletNotify::Initialize(&old_index,&f.wallet->get());
    ASSERT_TRUE(WalletNotify::IsInitialized());
    EXPECT_FALSE(f.wallet->Start());
    EXPECT_EQ(f.wallet->get().getUTXOIndex(),nullptr);
    EXPECT_TRUE(WalletNotify::IsInitialized());
    WalletNotify::Shutdown();
}
TEST(ChainstateWalletIndexOwner, V3EnrollmentCreatesIdentityAndPendingTogether) {
    WalletServiceIndexFixture f(true,true);
    ASSERT_TRUE(f.wallet->Start());
    ASSERT_FALSE(f.wallet->get().getNewAddress("v3-enrollment").empty());
    LegacyizeForV3Enrollment(f.wallet->get());
    const auto session=f.wallet->get().AcquireDatabaseLease()->Session();
    ASSERT_EQ(V3PreservedLegacyCoinCount(f.root/"blockchain"/"utxo"),1);
    const auto identity=f.wallet->EnrollActiveLegacyTransparentWalletV3(session);
    EXPECT_EQ(identity.size(),71u);
    auto lease=f.wallet->get().AcquireDatabaseLease();
    EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Pending);
    EXPECT_THROW(lease->RequireLegacyTransparentFinancialReady(),std::runtime_error);
    EXPECT_EQ(V3PreservedLegacyCoinCount(f.root/"blockchain"/"utxo"),1);
    EXPECT_EQ(f.wallet->get().getUTXOIndex(),nullptr);
}
TEST(ChainstateWalletIndexOwner, V3EnrollmentRetainsExistingIdentity) {
    WalletServiceIndexFixture f(true);
    ASSERT_TRUE(f.wallet->Start());
    ASSERT_FALSE(f.wallet->get().getNewAddress("v3-existing-id").empty());
    LegacyizeForV3Enrollment(f.wallet->get());
    auto lease=f.wallet->get().AcquireDatabaseLease();
    const auto session=lease->Session();
    const auto existing=lease->EnsureDeliveryIdentity();
    lease.reset();
    EXPECT_EQ(f.wallet->EnrollActiveLegacyTransparentWalletV3(session),existing);
    lease=f.wallet->get().AcquireDatabaseLease();
    EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Pending);
    EXPECT_THROW(lease->RequireLegacyTransparentFinancialReady(),std::runtime_error);
}
TEST(ChainstateWalletIndexOwner, V3EnrollmentRefusesChangedSessionAndTamperedAddress) {
    WalletServiceIndexFixture f(true,true);
    ASSERT_TRUE(f.wallet->Start());
    ASSERT_FALSE(f.wallet->get().getNewAddress("v3-auth-refusal").empty());
    LegacyizeForV3Enrollment(f.wallet->get());
    const auto session=f.wallet->get().AcquireDatabaseLease()->Session();
    EXPECT_THROW((void)f.wallet->EnrollActiveLegacyTransparentWalletV3(session+1),std::runtime_error);
    {auto lease=f.wallet->get().AcquireDatabaseLease();
     EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);}
    auto* db=f.wallet->get().getCurrentDatabase();
    ASSERT_EQ(sqlite3_exec(db,"UPDATE addresses SET script_pubkey='5120'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)f.wallet->EnrollActiveLegacyTransparentWalletV3(session),std::runtime_error);
    {auto lease=f.wallet->get().AcquireDatabaseLease();
     EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);}
    EXPECT_EQ(V3PreservedLegacyCoinCount(f.root/"blockchain"/"utxo"),1);
}
TEST(ChainstateWalletIndexOwner, V3EnrollmentRefusesLockedSeedAndV2Source) {
    WalletServiceIndexFixture v3(true);
    ASSERT_TRUE(v3.wallet->Start());
    ASSERT_FALSE(v3.wallet->get().getNewAddress("v3-locked-refusal").empty());
    LegacyizeForV3Enrollment(v3.wallet->get());
    const auto session=v3.wallet->get().AcquireDatabaseLease()->Session();
    v3.wallet->get().encryptWallet("fixture-v3-lock");
    if (!v3.wallet->get().isWalletLocked()) v3.wallet->get().lockWallet();
    EXPECT_THROW((void)v3.wallet->EnrollActiveLegacyTransparentWalletV3(session),std::runtime_error);
    {auto lease=v3.wallet->get().AcquireDatabaseLease();
     EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);}
    WalletServiceIndexFixture v2(false);
    ASSERT_TRUE(v2.wallet->Start());
    EXPECT_THROW((void)v2.wallet->EnrollActiveLegacyTransparentWalletV3(),std::runtime_error);
}
TEST(ChainstateWalletIndexOwner, MissingAndStoppedSourcesRefuse) {
    EXPECT_THROW(ChainstateService::AcquireWalletIndexUse({}),std::runtime_error);
    auto empty=std::make_shared<ChainstateService>();
    EXPECT_THROW(ChainstateService::AcquireWalletIndexUse(empty),std::runtime_error);
    WalletIndexOwnerFixture f;
    {auto owned=ChainstateService::AcquireWalletIndexUse(f.source);EXPECT_EQ(&owned->Index(),f.source->utxoIndex());}
    f.source->Stop();EXPECT_THROW(ChainstateService::AcquireWalletIndexUse(f.source),std::runtime_error);
    EXPECT_NO_THROW(f.source->Stop());
}
TEST(ChainstateWalletIndexOwner, NestedOwnershipPreservesExactIndex) {
    WalletIndexOwnerFixture f;auto outer=ChainstateService::AcquireWalletIndexUse(f.source);
    auto inner=ChainstateService::AcquireWalletIndexUse(f.source);
    EXPECT_EQ(&outer->Index(),&inner->Index());EXPECT_FALSE(f.source->Init(f.context));
    EXPECT_THROW(f.source->Stop(),std::logic_error);
    outer->Index().ApplyAtomically([&]{if(!outer->Index().SetMetadata("owner_probe","retained"))throw std::runtime_error("fixture metadata");});
    EXPECT_EQ(inner->Index().GetMetadata("owner_probe"),std::optional<std::string>("retained"));
    inner.reset();EXPECT_THROW(f.source->Stop(),std::logic_error);
    std::weak_ptr<ChainstateService> retained=f.source;f.context.chainstate.reset();f.source.reset();
    ASSERT_FALSE(retained.expired());EXPECT_EQ(outer->Index().GetMetadata("owner_probe"),std::optional<std::string>("retained"));
    f.source=retained.lock();outer.reset();EXPECT_NO_THROW(f.source->Stop());
}
TEST(ChainstateWalletIndexOwner, SelectedOwnershipRefusesShutdownBeforeEffects) {
    WalletIndexOwnerFixture f;
    {auto selected=f.source->AcquireBlockIngressActivationLock();EXPECT_THROW(f.source->Stop(),std::logic_error);}
    auto owner=ChainstateService::AcquireWalletIndexUse(f.source);
    EXPECT_EQ(&owner->Index(),f.source->utxoIndex());
}
TEST(ChainstateWalletIndexOwner, WalletBindingRequiresCurrentOwnedIndex) {
    WalletIndexOwnerFixture f;auto wallet=std::make_shared<WalletService>();
    ASSERT_TRUE(wallet->Init(f.context));f.context.wallet=wallet;
    {auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.source);
     use->Wallet().setUTXOIndex(&index->Index());use->Wallet().create("owned-index");}
    EXPECT_TRUE(wallet->EnsureRuntimeWalletBindings());
    f.source->Stop();EXPECT_FALSE(wallet->EnsureRuntimeWalletBindings());
    std::string error;EXPECT_FALSE(wallet->RecoverActiveWalletFromSnapshotIfNeeded(&error));EXPECT_FALSE(error.empty());
    {auto use=WalletService::AcquireWalletUse(wallet);EXPECT_EQ(use->Wallet().getCurrentWalletName(),"owned-index");}
    wallet->Stop();f.context.wallet.reset();
}
} // namespace dinero
