#pragma once
#include "daemon/runtime_delivery_worker.h"
#include "storage/chain_db_wallet_view.h"
#include "assets/asset_registry.h"
din::Json rpc_context_wallet_listaddresses(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_rescanblockchain(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_importtaprootdescriptor(const ExecutionContext&, const din::Json&);

namespace dinero {

namespace {
struct V3LegacyRescanWalletState {
    sqlite3_int64 changes = 0;
    int addresses = 0;
    int watch_scripts = 0;
    int utxos = 0;
    int sync_rows = 0;
    uint32_t height = 0;
    int next_external = 0;

    bool operator==(const V3LegacyRescanWalletState&) const = default;
};

V3LegacyRescanWalletState CaptureV3LegacyRescanWalletState(WalletManager& wallet) {
    auto lease = wallet.AcquireDatabaseLease();
    auto* db = lease->Database();
    if (!db) throw std::runtime_error("v3 rescan fixture wallet database unavailable");
    const auto count = [&](const char* table) {
        const std::string sql = std::string("SELECT count(*) FROM ") + table;
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
            throw std::runtime_error("v3 rescan fixture count prepare failed");
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        if (sqlite3_step(raw) != SQLITE_ROW)
            throw std::runtime_error("v3 rescan fixture count read failed");
        return sqlite3_column_int(raw, 0);
    };
    return {sqlite3_total_changes64(db), count("addresses"), count("watch_scripts"),
            count("utxos"), count("sync_meta"), wallet.getBlockchainHeight(),
            wallet.getNextAddressIndex()};
}
} // namespace

TEST(WalletV3RescanGate, AllEnrollmentStatesRefuseLegacyRpcAndDirectRescanWithoutWrites) {
    CanonicalRecoveryFixture f(false, true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height, 102u);

    const auto index_path = f.f.path / "v3-rescan-gate-index";
    { UTXOIndex v2(index_path.string()); ASSERT_TRUE(v2.Initialize()); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata = std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use = WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        use->Wallet().DisableLegacyRescansForFullArchiveV3();
        ASSERT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Absent);
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto replacement = ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                source, std::move(original));
            (void)replacement;
        }
    } restore{*f.f.service, ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service, std::move(metadata))};

    ExecutionContext request;
    request.daemon = &f.context;
    request.walletName = "canonical-recovery";
    din::Json range(Json::arrayValue);
    range.append(102);
    range.append(102);
    auto use = WalletService::AcquireWalletUse(f.wallet);
    auto& wallet = use->Wallet();
    auto* chain_db = f.f.service->GetChainDB();
    ASSERT_NE(chain_db, nullptr);
    storage::ChainDbWalletView view(*chain_db, f.context.block_storage.get());
    const auto expect_refusal = [&](WalletManager::LegacyTransparentState state) {
        SCOPED_TRACE(static_cast<int>(state));
        EXPECT_EQ(wallet.AcquireDatabaseLease()->LegacyTransparentState(), state);
        const auto before = CaptureV3LegacyRescanWalletState(wallet);
        const auto refused = rpc_context_wallet_rescanblockchain(request, range);
        EXPECT_EQ(refused["error_code"].asString(), "wallet_v3_legacy_rescan_unavailable")
            << refused.toStyledString();
        EXPECT_TRUE(refused["rescan_blocked"].asBool());
        EXPECT_FALSE(refused.isMember("scanned_blocks"));
        EXPECT_EQ(CaptureV3LegacyRescanWalletState(wallet), before);

        EXPECT_FALSE(wallet.rescanBlockchain(102, 20, &view));
        EXPECT_EQ(CaptureV3LegacyRescanWalletState(wallet), before);

        bool producer_called = false;
        EXPECT_EQ(wallet.rescanUtxoSet([&](const auto&) { producer_called = true; }, 102), -1);
        EXPECT_FALSE(producer_called);
        EXPECT_EQ(CaptureV3LegacyRescanWalletState(wallet), before);
    };
    expect_refusal(WalletManager::LegacyTransparentState::Absent);
    ASSERT_FALSE(f.wallet->EnrollActiveLegacyTransparentWalletV3().empty());
    expect_refusal(WalletManager::LegacyTransparentState::Pending);
    std::string recovery_error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&recovery_error),
        WalletService::CanonicalRecovery::AppliedPrefix) << recovery_error;
    expect_refusal(WalletManager::LegacyTransparentState::Complete);
}

TEST(WalletV3Spend, RecoveredTransparentCoinUsesOwnedChangeAndRetainsReservationAcrossRestart) {
    CanonicalRecoveryFixture f(true, true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height, 102u);

    const auto index_path = f.f.path / "v3-spend-index";
    { UTXOIndex v2(index_path.string()); ASSERT_TRUE(v2.Initialize()); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata = std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use = WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        use->Wallet().DisableLegacyRescansForFullArchiveV3();
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto replacement = ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                source, std::move(original));
            (void)replacement;
        }
    } restore{*f.f.service, ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service, std::move(metadata))};

    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ExecutionContext request;
    request.daemon = &f.context;
    request.walletName = "canonical-recovery";
    const auto ready_balance = rpc_context_wallet_getbalance(request, din::obj());
    ASSERT_FALSE(ready_balance.isMember("error")) << ready_balance.toStyledString();
    EXPECT_TRUE(ready_balance["send_ready"].asBool()) << ready_balance.toStyledString();
    EXPECT_EQ(ready_balance["send_scope"].asString(), "transparent_recovered_v3");
    EXPECT_GT(ready_balance["spendable"].asDouble(), 0.0);
    din::Json payment = din::obj();
    payment["address"] = OrchardMiningPayout;
    payment["amount_una"] = Json::UInt64(100000);
    payment["fee_rate"] = 1;
    payment["preview"] = true;
    ASSERT_NE(f.receive_address, OrchardMiningPayout);

    // A selected-tip change without wallet reconciliation cannot sign from
    // the old receipt, even though the ordinary coin rows still exist.
    f.MineEmpty();
    ASSERT_EQ(f.f.service->GetActiveTip()->height, 103u);
    const auto stale = rpc_context_wallet_sendtoaddress(request, payment);
    EXPECT_EQ(stale["error_code"].asString(), "wallet_balance_not_current")
        << stale.toStyledString();
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;

    // A disconnect also invalidates the selected source. The H103 wallet
    // receipt may not authorize a spend on the selected H102 branch.
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(
        *f.f.service, f.f.service->GetActiveTip()));
    ASSERT_EQ(f.f.service->GetActiveTip()->height, 102u);
    const auto reorged = rpc_context_wallet_sendtoaddress(request, payment);
    EXPECT_EQ(reorged["error_code"].asString(), "wallet_balance_not_current")
        << reorged.toStyledString();
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;

    payment["change_address"] = OrchardMiningPayout;
    const auto override_change = rpc_context_wallet_sendtoaddress(request, payment);
    EXPECT_EQ(override_change["error_code"].asString(),
        "wallet_v3_change_override_unavailable") << override_change.toStyledString();
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());
    payment.removeMember("change_address");

    // A persisted/promoted snapshot base must not acquire the full-archive
    // signing capability merely because its current receipts still compare.
    ShieldedStateStartupTestAccess::SetRetainedSnapshotBaseForPayment(*f.f.service,1);
    const auto snapshot_balance = rpc_context_wallet_getbalance(request, din::obj());
    ASSERT_FALSE(snapshot_balance.isMember("error")) << snapshot_balance.toStyledString();
    EXPECT_FALSE(snapshot_balance["send_ready"].asBool()) << snapshot_balance.toStyledString();
    EXPECT_TRUE(snapshot_balance["spendable"].isNull()) << snapshot_balance.toStyledString();
    for (const bool preview : {true, false}) {
        if (preview) payment["preview"] = true;
        else payment.removeMember("preview");
        const auto snapshot_refusal = rpc_context_wallet_sendtoaddress(request, payment);
        EXPECT_EQ(snapshot_refusal["error_code"].asString(), "wallet_v3_send_source_unavailable")
            << snapshot_refusal.toStyledString();
        EXPECT_FALSE(snapshot_refusal.isMember("txid"));
    }
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());
    ShieldedStateStartupTestAccess::SetRetainedSnapshotBaseForPayment(*f.f.service,0);

    payment["preview"] = true;
    const auto preview = rpc_context_wallet_sendtoaddress(request, payment);
    ASSERT_EQ(preview["status"].asString(), "preview") << preview.toStyledString();
    EXPECT_EQ(preview["change_address"].asString(), f.receive_address);
    EXPECT_GT(preview["available_keys"].asInt(), 0);
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());

    payment.removeMember("preview");
    const auto sent = rpc_context_wallet_sendtoaddress(request, payment);
    ASSERT_EQ(sent["status"].asString(), "signed_and_submitted") << sent.toStyledString();
    ASSERT_TRUE(sent["accepted"].asBool()) << sent.toStyledString();
    EXPECT_EQ(sent["change_address"].asString(), f.receive_address);
    ASSERT_EQ(f.wallet->get().getPendingPayments().size(), 1u);
    const auto retained_txid = sent["txid"].asString();
    ASSERT_EQ(retained_txid.size(), 64u);

    // Reopening both wallet and index owners must preserve the same payment
    // reservation. A second attempt may not reuse the funded input.
    f.wallet->Stop();
    f.context.wallet.reset();
    f.wallet.reset();
    f.wallet = std::make_shared<WalletService>();
    f.context.wallet = f.wallet;
    auto reopened = std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(reopened->InitializeFullArchiveV3MetadataOnly());
    auto closed = ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service, std::move(reopened));
    ASSERT_TRUE(closed);
    closed.reset();
    ASSERT_TRUE(f.wallet->Init(f.context));
    ASSERT_TRUE(f.wallet->Start());
    {
        auto use = WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().unlockWallet("canonical-fixture-pass", 0);
        ASSERT_FALSE(use->Wallet().isWalletLocked());
        const auto pending = use->Wallet().getPendingPayments();
        ASSERT_EQ(pending.size(), 1u);
        EXPECT_EQ(pending.front().txid, retained_txid);
    }
    const auto duplicate = rpc_context_wallet_sendtoaddress(request, payment);
    EXPECT_TRUE(duplicate.isMember("error")) << duplicate.toStyledString();
    EXPECT_FALSE(duplicate.isMember("txid"));
    EXPECT_EQ(f.wallet->get().getPendingPayments().size(), 1u);
}

TEST(WalletV3ImportGuard, MandatoryDescriptorRescanRefusesBeforeAnyWalletWrite) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    const auto path=f.f.path/"v3-descriptor-import-index";
    { UTXOIndex v2(path.string());ASSERT_TRUE(v2.Initialize()); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(path);
    auto v3=std::make_unique<UTXOIndex>(path.string());
    ASSERT_TRUE(v3->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto v3=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                source,std::move(original));
            (void)v3;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(v3))};
    ASSERT_TRUE(f.f.service->HasFullArchiveWalletIndexV3());
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    din::Json params=din::obj();
    params["descriptor"]="tr(0000000000000000000000000000000000000000000000000000000000000001)";
    const auto changes=[&]() {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        return sqlite3_total_changes(lease->Database());
    };
    const auto refuse_without_write=[&](WalletManager::LegacyTransparentState expected) {
        {
            auto use=WalletService::AcquireWalletUse(f.wallet);
            auto lease=use->Wallet().AcquireDatabaseLease();
            EXPECT_EQ(lease->LegacyTransparentState(),expected);
        }
        const int before=changes();
        const auto result=rpc_context_wallet_importtaprootdescriptor(request,params);
        EXPECT_EQ(result["error_code"].asString(),"wallet_v3_descriptor_rescan_unavailable")
            << result.toStyledString();
        EXPECT_FALSE(result.get("success",false).asBool());
        EXPECT_EQ(changes(),before) << "descriptor refusal changed wallet database";
    };
    refuse_without_write(WalletManager::LegacyTransparentState::Absent);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_FALSE(lease->BeginLegacyTransparentMigrationV3(index->Index(),lease->Session()).empty());
    }
    refuse_without_write(WalletManager::LegacyTransparentState::Pending);
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    refuse_without_write(WalletManager::LegacyTransparentState::Complete);
    f.wallet->Stop();
}

// A full archive v3 index is a metadata/receipt handle, not a source of
// unscoped wallet coins. Capture the H102 source with an enrolled named wallet,
// advance the selected tip, and verify that the already checked H102 prefix
// remains independently readable while a fresh capture follows H103.
TEST(WalletV3RecoveryRoute, FullArchiveSourceCapturesNamedWalletAtFixedPrefix) {
    CanonicalRecoveryFixture f(false,true);
    const auto [historical_amount,historical_total]=f.PrepareFundedHistoricalWallet();
    (void)historical_amount;(void)historical_total;
    f.DowngradeToPreIdentityTransparentWallet();
    const auto first_reward=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);

    const auto index_path=f.f.path/"v3-source-capture-index";
    { UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize()); }
    const auto upgraded=UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    ASSERT_TRUE(std::filesystem::is_regular_file(upgraded.rollback_backup));
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
        ASSERT_FALSE(lease->BeginLegacyTransparentMigration().empty());
        ASSERT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Pending);
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto replacement=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                source,std::move(original));
            (void)replacement;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(metadata))};

    std::shared_ptr<const RuntimeWalletCoverageProjection> h102;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto captured=f.f.service->getRuntimeWalletCoverageV3(
            use->Wallet(),session,index->Index());
        ASSERT_TRUE(captured.ok()) << static_cast<int>(captured.status());
        h102=*captured;
    }
    ASSERT_EQ(h102->TipHeight(),102u);
    ASSERT_EQ(h102->TipHash(),f.f.service->GetActiveTip()->hash);
    const auto address=DecodeWitnessAddress(f.receive_address,"rdin");
    ASSERT_TRUE(address.is_valid);
    size_t historical=0,rewards=0;
    for(const auto& [point,coin]:h102->Coins()) {
        (void)point;
        if(coin.output.scriptPubKey!=address.script_pubkey)continue;
        if(coin.height==101 && !coin.coinbase)++historical;
        if(coin.height==102 && coin.coinbase) {
            EXPECT_EQ(coin.output.value.GetUna(),first_reward);
            ++rewards;
        }
    }
    EXPECT_EQ(historical,1u);
    EXPECT_EQ(rewards,1u);

    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    ASSERT_EQ(h102->TipHeight(),102u); // owned projection cannot drift with tip
    const auto checked_h102=ShieldedStateStartupTestAccess::ReadDeliveryPrefix(
        *f.f.service,h102->Head());
    ASSERT_TRUE(checked_h102.ok()) << static_cast<int>(checked_h102.status());
    ASSERT_TRUE((*checked_h102)->after_tip);
    EXPECT_EQ((*checked_h102)->after_tip->first,h102->TipHash());
    EXPECT_EQ((*checked_h102)->after_tip->second,102u);
    EXPECT_TRUE((*checked_h102)->events.empty());

    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    const auto next=f.f.service->getRuntimeWalletCoverageV3(
        use->Wallet(),session,index->Index());
    ASSERT_TRUE(next.ok()) << static_cast<int>(next.status());
    EXPECT_EQ((*next)->TipHeight(),103u);
    EXPECT_EQ((*next)->TipHash(),f.f.service->GetActiveTip()->hash);
    EXPECT_NE((*next)->Head(),h102->Head());
}

TEST(WalletV3RecoveryWorker, MetadataOnlyPassDoesNotOpenLegacyShieldedSidecar) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().create("v3-worker-sidecar-probe");
        use->Wallet().setUTXOIndex(nullptr);
    }
    const auto shielded_path=std::filesystem::path(
        f.wallet->get().GetV7P2MRStorePath()).parent_path()/
        "shielded_notes_v3-worker-sidecar-probe.sqlite";
    ASSERT_FALSE(std::filesystem::exists(shielded_path));
    const auto index_path=f.f.path/"v3-worker-sidecar-index";
    { UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize()); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto replacement=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                source,std::move(original));
            (void)replacement;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(metadata))};
    RuntimeDeliveryWorker worker(f.f.service,f.wallet,
        RuntimeDeliveryWorker::Limits{1,std::chrono::minutes(1)});
    worker.Start();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(worker.Snapshot().slices==0 && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    worker.Stop();
    ASSERT_GT(worker.Snapshot().slices,0u);
    EXPECT_FALSE(std::filesystem::exists(shielded_path));
}

TEST(WalletV3RecoveryWorker, UnlockStartsOwnerScopedRecoveryWithoutLegacyWriter) {
    CanonicalRecoveryFixture f(true,true);
    const auto [historical_amount,historical_total]=f.PrepareFundedHistoricalWallet();
    (void)historical_amount;
    f.DowngradeToPreIdentityTransparentWallet();
    const auto mined_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);

    const auto index_path=f.f.path/"v3-worker-index";
    { UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize()); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        ASSERT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Absent);
        use->Wallet().lockWallet();
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto replacement=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                source,std::move(original));
            (void)replacement;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(metadata))};

    RuntimeDeliveryWorker worker(f.f.service,f.wallet,
        RuntimeDeliveryWorker::Limits{1,std::chrono::minutes(1)});
    const auto wait=[&](const auto& predicate) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        for(;;) {
            const auto report=worker.Snapshot();
            if(predicate(report))return report;
            if(std::chrono::steady_clock::now()>=deadline)
                throw std::runtime_error("v3 wallet worker did not reach expected state");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    worker.Start();
    const auto locked=wait([](const auto& report){return report.slices>0;});
    worker.Stop();
    EXPECT_EQ(locked.wallet,RuntimeDeliveryWorker::WalletOutcome::Deferred);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),101u);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Absent);
    }
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    din::Json unlock(Json::arrayValue);unlock.append("canonical-fixture-pass");unlock.append(0);
    const auto response=rpc_context_wallet_unlock(request,unlock);
    ASSERT_TRUE(response["success"].asBool()) << response.toStyledString();
    EXPECT_FALSE(response.isMember("recovery_warning")) << response.toStyledString();
    EXPECT_EQ(response["recovery_mode"].asString(),"owner_scoped_async") << response.toStyledString();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Absent);
    }
    worker.Start();
    const auto unlocked=wait([](const auto& report) {
        return report.slices>0 && report.wallet==RuntimeDeliveryWorker::WalletOutcome::AppliedPrefix;
    });
    worker.Stop();
    ASSERT_TRUE(unlocked.wallet_head);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(
        use->Wallet(),index->Index(),session);
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
    ASSERT_TRUE(indexed);ASSERT_TRUE(ordinary);
    EXPECT_EQ(indexed->cursor,ordinary->cursor);
    EXPECT_EQ(indexed->cursor,*unlocked.wallet_head);
    EXPECT_EQ(indexed->tip_height,102u);
    EXPECT_EQ(use->Wallet().getBlockchainHeight(),102u);
    EXPECT_NEAR(use->Wallet().getBalance().total,
        historical_total+double(mined_amount)/100000000.0,0.00000001);
}

TEST(WalletV3ColdRestart, NamedWalletReceiptsSurviveFreshServiceAndIndexHandles) {
    CanonicalRecoveryFixture f(true,true);
    const auto [historical_amount,historical_total]=f.PrepareFundedHistoricalWallet();
    (void)historical_amount;
    f.DowngradeToPreIdentityTransparentWallet();
    const auto first_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);

    const auto index_path=f.f.path/"v3-cold-restart-index";
    { UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize()); }
    const auto upgraded=UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    ASSERT_TRUE(std::filesystem::is_regular_file(upgraded.rollback_backup));
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto replacement=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                source,std::move(original));
            (void)replacement;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(metadata))};

    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    RuntimeOutboxCursor h102_cursor;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(
            use->Wallet(),index->Index(),session);
        const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
        ASSERT_TRUE(indexed);ASSERT_TRUE(ordinary);
        EXPECT_EQ(indexed->cursor,ordinary->cursor);
        EXPECT_EQ(indexed->tip_height,102u);
        h102_cursor=indexed->cursor;
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Complete);
        EXPECT_NEAR(use->Wallet().getBalance().total,
            historical_total+double(first_amount)/100000000.0,0.00000001);
    }

    // Destroy both process-local owners and reopen the same named wallet and
    // metadata index from disk. Its authenticated H102 receipts must survive
    // without relying on the old WalletService or SQLite connection.
    f.wallet->Stop();
    f.context.wallet.reset();
    f.wallet.reset();
    // Keep the fixture owner valid even if reopening the index fails below.
    f.wallet=std::make_shared<WalletService>();
    f.context.wallet=f.wallet;
    auto reopened=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(reopened->InitializeFullArchiveV3MetadataOnly());
    auto closed=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(reopened));
    ASSERT_TRUE(closed);
    EXPECT_TRUE(closed->IsFullArchiveV3MetadataOnly());
    closed.reset();
    ASSERT_TRUE(f.wallet->Init(f.context));
    ASSERT_TRUE(f.wallet->Start());
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        EXPECT_EQ(use->Wallet().getCurrentWalletName(),"canonical-recovery");
        EXPECT_TRUE(use->Wallet().isLocked());
        EXPECT_EQ(use->Wallet().getUTXOIndex(),nullptr);
        EXPECT_TRUE(use->Wallet().LegacyRescansDisabledForFullArchiveV3());
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(
            use->Wallet(),index->Index(),session);
        const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
        ASSERT_TRUE(indexed);ASSERT_TRUE(ordinary);
        EXPECT_EQ(indexed->cursor,h102_cursor);
        EXPECT_EQ(ordinary->cursor,h102_cursor);
        EXPECT_EQ(indexed->tip_height,102u);
        EXPECT_EQ(ordinary->tip_height,102u);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),102u);
        EXPECT_NEAR(use->Wallet().getBalance().total,
            historical_total+double(first_amount)/100000000.0,0.00000001);
        const auto rpc_balance=rpc_context_wallet_getbalance(request,din::obj());
        EXPECT_FALSE(rpc_balance.isMember("error")) << rpc_balance.toStyledString();
        EXPECT_NEAR(rpc_balance["total"].asDouble(),
            historical_total+double(first_amount)/100000000.0,0.00000001);
        din::Json mined_request=din::obj();mined_request["type"]="mined";
        const auto persisted=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(persisted.isArray()) << persisted.toStyledString();
        ASSERT_EQ(persisted.size(),1u);
        EXPECT_EQ(persisted[0]["height"].asUInt(),102u);
        use->Wallet().unlockWallet("canonical-fixture-pass",0);
    }
    const auto next_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(
            use->Wallet(),index->Index(),session);
        const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
        ASSERT_TRUE(indexed);ASSERT_TRUE(ordinary);
        EXPECT_EQ(indexed->cursor,ordinary->cursor);
        EXPECT_EQ(indexed->tip_height,103u);
        EXPECT_EQ(ordinary->tip_height,103u);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),103u);
        EXPECT_NEAR(use->Wallet().getBalance().total,
            historical_total+double(first_amount+next_amount)/100000000.0,0.00000001);
        din::Json mined_request=din::obj();mined_request["type"]="mined";
        const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(mined.isArray()) << mined.toStyledString();
        ASSERT_EQ(mined.size(),2u);
    }
    f.wallet->Stop(); // Stop before restoring the fixture's original v2 handle.
}

// The full-archive wallet route must recover through the authenticated v3
// metadata handle without binding that handle to legacy wallet writers. The
// existing v2 canonical-recovery fixture supplies real preactivation history,
// a typed Orchard block and the selected outbox. This test stays RED until the
// production WalletService/ChainstateService route composes those sources.
TEST(WalletV3RecoveryRoute, LegacyNamedWalletRecoversTypedCoinbaseWithoutV2IndexBinding) {
    CanonicalRecoveryFixture f(false,true);
    const auto [historical_amount,historical_total]=f.PrepareFundedHistoricalWallet();
    (void)historical_amount;
    f.DowngradeToPreIdentityTransparentWallet();
    const auto first_address=f.receive_address;
    const auto mined_amount=f.MineToWallet();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().create("second-legacy-wallet");
        f.receive_address=use->Wallet().getNewAddress();
        ASSERT_NE(f.receive_address,first_address);
    }
    f.DowngradeToPreIdentityTransparentWallet();
    const auto second_mined_amount=f.MineToWallet();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().open("canonical-recovery");
    }
    f.receive_address=first_address;
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);

    const auto index_path=f.f.path/"v3-route-legacy-wallet-index";
    { UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize()); }
    const auto old_script=DecodeWitnessAddress(first_address,"rdin");
    ASSERT_TRUE(old_script.is_valid);
    sqlite3* raw=nullptr;
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
    sqlite3_stmt* stale=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,
        "INSERT INTO wallet_utxos(txid,vout,value,spk,path,height,is_coinbase) "
        "VALUES(?,0,1,?,'m/86/1448/0/0/0',101,0)",-1,&stale,nullptr),SQLITE_OK);
    const std::string stale_txid(64,'e');
    ASSERT_EQ(sqlite3_bind_text(stale,1,stale_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(stale,2,old_script.script_pubkey.data(),
        int(old_script.script_pubkey.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stale),SQLITE_DONE);
    sqlite3_finalize(stale);
    ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    const auto upgraded=UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    ASSERT_TRUE(std::filesystem::is_regular_file(upgraded.rollback_backup));
    auto v3=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(v3->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        // A v3 handle is metadata-only; binding it as the old address/UTXO
        // writer is exactly the route this regression must prevent.
        use->Wallet().setUTXOIndex(nullptr);
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
        ASSERT_EQ(use->Wallet().getBlockchainHeight(),101u);
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto v3=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(source,std::move(original));
            (void)v3;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(v3))};

    // Both old named wallets are Absent here. Production recovery must first
    // enroll each with its own authenticated identity, then capture through
    // the v3 route. A test-side BeginLegacyTransparentMigration would mask
    // precisely the route this regression is intended to exercise.
    std::string error;
    const auto result=f.wallet->RecoverActiveWalletFromCanonicalSource(&error);
    ASSERT_EQ(result,WalletService::CanonicalRecovery::AppliedPrefix) << error;
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),index->Index(),session);
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
    ASSERT_TRUE(indexed);ASSERT_TRUE(ordinary);
    EXPECT_EQ(indexed->cursor,ordinary->cursor);
    EXPECT_EQ(indexed->tip_height,103u);
    EXPECT_EQ(ordinary->tip_height,103u);
    EXPECT_EQ(ordinary->tip_hash,f.f.service->GetActiveTip()->hash);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
        WalletManager::LegacyTransparentState::Complete);
    EXPECT_EQ(use->Wallet().getBlockchainHeight(),103u);
    const auto balance=use->Wallet().getBalance();
    EXPECT_NEAR(balance.total,historical_total+double(mined_amount)/100000000.0,0.00000001);
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    // A Complete v3 receipt authenticates the transparent address inventory,
    // not the legacy P2MR sidecar. Open() creates its SQLite file on reads.
    const auto p2mr_path=std::filesystem::path(
        f.wallet->get().GetV7P2MRStorePath());
    ASSERT_FALSE(std::filesystem::exists(p2mr_path));
    const auto listed=rpc_context_wallet_listaddresses(request,din::obj());
    ASSERT_TRUE(listed.isArray()) << listed.toStyledString();
    EXPECT_FALSE(std::filesystem::exists(p2mr_path));
    for(const auto& row:listed)EXPECT_NE(row["type"].asString(),"p2mr");

    // A global asset registry can contain rows that have no v3 owner receipt.
    // Even a matching wallet address must not turn those rows into a reported
    // wallet balance in this transparent-only scope.
    struct AssetRegistryCleanup { ~AssetRegistryCleanup(){assets::ShutdownAssetRegistry();} } asset_cleanup;
    assets::InitAssetRegistry((f.f.path/"v3-unreceipted-assets.sqlite").string());
    assets::AssetUTXO unreceipted{};
    unreceipted.txid=std::string(64,'a');
    unreceipted.asset_id.fill(0x42);
    unreceipted.amount=123;
    unreceipted.owner_address=f.receive_address;
    unreceipted.height=102;
    ASSERT_TRUE(assets::GetAssetRegistry().addUTXO(unreceipted));
    ASSERT_FALSE(assets::GetAssetRegistry().getAddressBalances(f.receive_address).empty());
    const auto scoped_balance=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(scoped_balance.isMember("error")) << scoped_balance.toStyledString();
    EXPECT_EQ(scoped_balance["wallet_scope"].asString(),"legacy_transparent_only");
    EXPECT_FALSE(scoped_balance.isMember("assets")) << scoped_balance.toStyledString();
    EXPECT_FALSE(scoped_balance.isMember("asset_count")) << scoped_balance.toStyledString();
    din::Json mined_request=din::obj();mined_request["type"]="mined";
    const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
    ASSERT_TRUE(mined.isArray()) << mined.toStyledString();
    ASSERT_EQ(mined.size(),1u);
    EXPECT_EQ(mined[0]["height"].asUInt(),102u);
    EXPECT_TRUE(mined[0]["is_coinbase"].asBool());
    index.reset();use.reset();

    // Once the first wallet has a complete v3 baseline, a new typed Orchard
    // block must extend both receipts and the mined transaction view. This is
    // the normal post-recovery path, separate from the initial H0 scan.
    const auto later_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,104u);
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    RuntimeOutboxCursor h104_cursor;
    {
        auto current=WalletService::AcquireWalletUse(f.wallet);
        auto current_index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto current_session=current->Wallet().AcquireDatabaseLease()->Session();
        const auto current_indexed=RuntimeIndexDelivery::ReadForWalletV3(
            current->Wallet(),current_index->Index(),current_session);
        const auto current_ordinary=RuntimeOrdinaryDelivery::ReadForWallet(
            current->Wallet(),current_session);
        ASSERT_TRUE(current_indexed);ASSERT_TRUE(current_ordinary);
        EXPECT_EQ(current_indexed->cursor,current_ordinary->cursor);
        EXPECT_EQ(current_indexed->tip_height,104u);
        EXPECT_EQ(current_ordinary->tip_height,104u);
        EXPECT_EQ(current->Wallet().getBlockchainHeight(),104u);
        EXPECT_NEAR(current->Wallet().getBalance().total,
            historical_total+double(mined_amount+later_amount)/100000000.0,0.00000001);
        h104_cursor=current_indexed->cursor;
        const auto later_history=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(later_history.isArray()) << later_history.toStyledString();
        ASSERT_EQ(later_history.size(),2u);
        bool h102=false,h104=false;
        for(const auto& row:later_history) {
            EXPECT_TRUE(row["is_coinbase"].asBool());
            h102|=row["height"].asUInt()==102u;
            h104|=row["height"].asUInt()==104u;
        }
        EXPECT_TRUE(h102);EXPECT_TRUE(h104);
    }

    // Model an interruption between the index commit and ordinary commit for
    // the next event. The canonical retry must recognize the owner-scoped
    // index receipt at H105 and finish only the missing ordinary side.
    BlockAssembler interrupted_assembler(&f.f.db);
    WireOrchardAssembler(interrupted_assembler,f.f);
    const auto interrupted_block=interrupted_assembler.CreateOrchardBlock(f.receive_address);
    ASSERT_TRUE(interrupted_block);
    const auto included=f.Submit(interrupted_block->WireBytes());
    ASSERT_TRUE(included.accepted()&&included.connected) << included.reason;
    const auto interrupted_coin=f.f.db.getCoin(
        interrupted_block->Transactions().front().GetTxid().AsUint256(),0);
    ASSERT_TRUE(interrupted_coin.ok());
    const auto interrupted_amount=interrupted_coin->amount;
    ASSERT_EQ(f.f.service->GetActiveTip()->height,105u);
    const auto pending_page=f.f.service->getRuntimeDeliveryPage(h104_cursor,1);
    ASSERT_TRUE(pending_page.ok()) << static_cast<int>(pending_page.status());
    ASSERT_EQ((*pending_page)->events.size(),1u);
    const auto& interrupted=(*pending_page)->events.front();
    ASSERT_EQ(interrupted.context.height,105u);
    ASSERT_EQ(interrupted.context.block_hash,f.f.service->GetActiveTip()->hash);
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        auto current=WalletService::AcquireWalletUse(f.wallet);
        auto current_index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto current_session=current->Wallet().AcquireDatabaseLease()->Session();
        const auto staged=RuntimeIndexDelivery::ApplyForWalletV3(
            current->Wallet(),current_index->Index(),current_session,interrupted,
            [&](const auto& commit) {
                const auto checked=ShieldedStateStartupTestAccess::ReadDeliveryPrefix(
                    *f.f.service,(*pending_page)->head);
                if(!checked.ok() || !(*checked)->after_tip ||
                   (*checked)->after_tip->first!=interrupted.context.block_hash ||
                   (*checked)->after_tip->second!=105u ||
                   f.f.service->GetActiveTip()->hash!=interrupted.context.block_hash)
                    throw std::runtime_error("v3 interrupted source changed");
                commit();
            });
        EXPECT_EQ(staged.tip_height,105u);
        const auto staged_index=RuntimeIndexDelivery::ReadForWalletV3(
            current->Wallet(),current_index->Index(),current_session);
        const auto old_ordinary=RuntimeOrdinaryDelivery::ReadForWallet(
            current->Wallet(),current_session);
        ASSERT_TRUE(staged_index);ASSERT_TRUE(old_ordinary);
        EXPECT_EQ(staged_index->tip_height,105u);
        EXPECT_EQ(old_ordinary->tip_height,104u);
        const auto fenced_balance=rpc_context_wallet_getbalance(request,din::obj());
        EXPECT_EQ(fenced_balance["error_code"].asString(),"wallet_balance_not_current")
            << fenced_balance.toStyledString();
    }
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto current=WalletService::AcquireWalletUse(f.wallet);
        auto current_index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto current_session=current->Wallet().AcquireDatabaseLease()->Session();
        const auto completed_index=RuntimeIndexDelivery::ReadForWalletV3(
            current->Wallet(),current_index->Index(),current_session);
        const auto completed_ordinary=RuntimeOrdinaryDelivery::ReadForWallet(
            current->Wallet(),current_session);
        ASSERT_TRUE(completed_index);ASSERT_TRUE(completed_ordinary);
        EXPECT_EQ(completed_index->cursor,completed_ordinary->cursor);
        EXPECT_EQ(completed_index->tip_height,105u);
        EXPECT_EQ(completed_ordinary->tip_height,105u);
        EXPECT_EQ(current->Wallet().getBlockchainHeight(),105u);
        EXPECT_NEAR(current->Wallet().getBalance().total,
            historical_total+double(mined_amount+later_amount+interrupted_amount)/100000000.0,0.00000001);
        const auto completed_history=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(completed_history.isArray()) << completed_history.toStyledString();
        ASSERT_EQ(completed_history.size(),3u);
    }

    // The same typed body is then disconnected and reconnected. Neither the
    // v3 index nor ordinary history may retain the disconnected reward, and
    // replaying the retained body must restore it exactly once.
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(
        *f.f.service,f.f.service->GetActiveTip()));
    ASSERT_EQ(f.f.service->GetActiveTip()->height,104u);
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto current=WalletService::AcquireWalletUse(f.wallet);
        auto current_index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto current_session=current->Wallet().AcquireDatabaseLease()->Session();
        const auto rolled_index=RuntimeIndexDelivery::ReadForWalletV3(
            current->Wallet(),current_index->Index(),current_session);
        const auto rolled_ordinary=RuntimeOrdinaryDelivery::ReadForWallet(
            current->Wallet(),current_session);
        ASSERT_TRUE(rolled_index);ASSERT_TRUE(rolled_ordinary);
        EXPECT_EQ(rolled_index->cursor,rolled_ordinary->cursor);
        EXPECT_EQ(rolled_index->tip_height,104u);
        EXPECT_EQ(rolled_ordinary->tip_height,104u);
        EXPECT_NEAR(current->Wallet().getBalance().total,
            historical_total+double(mined_amount+later_amount)/100000000.0,0.00000001);
        const auto rolled_history=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(rolled_history.isArray()) << rolled_history.toStyledString();
        EXPECT_EQ(rolled_history.size(),2u);
    }
    const auto reconnected=f.Submit(interrupted_block->WireBytes());
    ASSERT_TRUE(reconnected.accepted()&&reconnected.connected) << reconnected.reason;
    ASSERT_EQ(f.f.service->GetActiveTip()->height,105u);
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto current=WalletService::AcquireWalletUse(f.wallet);
        auto current_index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto current_session=current->Wallet().AcquireDatabaseLease()->Session();
        const auto restored_index=RuntimeIndexDelivery::ReadForWalletV3(
            current->Wallet(),current_index->Index(),current_session);
        const auto restored_ordinary=RuntimeOrdinaryDelivery::ReadForWallet(
            current->Wallet(),current_session);
        ASSERT_TRUE(restored_index);ASSERT_TRUE(restored_ordinary);
        EXPECT_EQ(restored_index->cursor,restored_ordinary->cursor);
        EXPECT_EQ(restored_index->tip_height,105u);
        EXPECT_EQ(restored_ordinary->tip_height,105u);
        EXPECT_NEAR(current->Wallet().getBalance().total,
            historical_total+double(mined_amount+later_amount+interrupted_amount)/100000000.0,0.00000001);
        const auto restored_history=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(restored_history.isArray()) << restored_history.toStyledString();
        EXPECT_EQ(restored_history.size(),3u);
    }

    {
        auto second=WalletService::AcquireWalletUse(f.wallet);
        second->Wallet().open("second-legacy-wallet");
        ASSERT_EQ(second->Wallet().getUTXOIndex(),nullptr);
        ASSERT_EQ(second->Wallet().getBlockchainHeight(),101u);
    }
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    auto second=WalletService::AcquireWalletUse(f.wallet);
    auto second_index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto second_session=second->Wallet().AcquireDatabaseLease()->Session();
    const auto second_indexed=RuntimeIndexDelivery::ReadForWalletV3(
        second->Wallet(),second_index->Index(),second_session);
    const auto second_ordinary=RuntimeOrdinaryDelivery::ReadForWallet(
        second->Wallet(),second_session);
    ASSERT_TRUE(second_indexed);ASSERT_TRUE(second_ordinary);
    EXPECT_EQ(second_indexed->cursor,second_ordinary->cursor);
    EXPECT_EQ(second_indexed->tip_height,105u);
    EXPECT_EQ(second_ordinary->tip_hash,f.f.service->GetActiveTip()->hash);
    EXPECT_EQ(second->Wallet().getBlockchainHeight(),105u);
    const auto second_balance=second->Wallet().getBalance();
    EXPECT_NEAR(second_balance.total,double(second_mined_amount)/100000000.0,0.00000001);
    request.walletName="second-legacy-wallet";
    const auto second_history=rpc_context_wallet_listtransactions(request,mined_request);
    ASSERT_TRUE(second_history.isArray()) << second_history.toStyledString();
    ASSERT_EQ(second_history.size(),1u);
    EXPECT_EQ(second_history[0]["height"].asUInt(),103u);
    EXPECT_TRUE(second_history[0]["is_coinbase"].asBool());
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr),SQLITE_OK);
    sqlite3_stmt* quarantined=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,
        "SELECT value FROM wallet_utxos_legacy_v2 WHERE txid=? AND vout=0",
        -1,&quarantined,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(quarantined,1,stale_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(quarantined),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(quarantined,0),1);
    EXPECT_EQ(sqlite3_step(quarantined),SQLITE_DONE);
    sqlite3_finalize(quarantined);
    ASSERT_EQ(sqlite3_prepare_v2(raw,
        "SELECT count(*) FROM wallet_utxos WHERE txid=? AND vout=0",
        -1,&quarantined,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(quarantined,1,stale_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(quarantined),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(quarantined,0),0);
    sqlite3_finalize(quarantined);
    ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
}

} // namespace dinero
