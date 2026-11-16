#pragma once
#include "daemon/runtime_delivery_worker.h"
#include "storage/chain_db_wallet_view.h"
#include "assets/asset_registry.h"
din::Json rpc_context_wallet_listaddresses(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_rescanblockchain(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_importtaprootdescriptor(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_setlabel(const ExecutionContext&, const din::Json&);
#include "rpc/orchard_account_rpc.h"

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
    // HTTP requests do not name the selected wallet. The status handler must
    // pin that selection before it delegates to the balance reader.
    auto http_request = request;
    http_request.walletName.clear();
    const auto ready_status = rpc_context_wallet_getstatus(http_request, din::obj());
    ASSERT_FALSE(ready_status.isMember("error")) << ready_status.toStyledString();
    EXPECT_TRUE(ready_status["send_ready"].asBool()) << ready_status.toStyledString();
    EXPECT_EQ(ready_status["send_scope"].asString(), "transparent_recovered_v3");
    EXPECT_DOUBLE_EQ(ready_status["spendable_balance"].asDouble(),
                     ready_balance["spendable"].asDouble());
    // The status-only count is one immature mined coin in this fixture.
    // getbalance's frozen schema does not expose this diagnostic field.
    EXPECT_EQ(ready_status["immature_utxo_count"].asUInt(), 1u);
    EXPECT_FALSE(ready_balance.isMember("immature_utxo_count"));
    EXPECT_FALSE(ready_status.isMember("send_unavailable_reason"));
    auto wrong_wallet = request;
    wrong_wallet.walletName = "different-wallet";
    const auto wrong_status = rpc_context_wallet_getstatus(wrong_wallet, din::obj());
    EXPECT_EQ(wrong_status["error_code"].asString(), "wallet_balance_not_current")
        << wrong_status.toStyledString();
    EXPECT_FALSE(wrong_status.isMember("spendable_balance"));
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
    const auto snapshot_status = rpc_context_wallet_getstatus(request, din::obj());
    ASSERT_FALSE(snapshot_status.isMember("error")) << snapshot_status.toStyledString();
    EXPECT_FALSE(snapshot_status["send_ready"].asBool()) << snapshot_status.toStyledString();
    EXPECT_TRUE(snapshot_status["spendable_balance"].isNull()) << snapshot_status.toStyledString();
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
        RuntimeDeliveryWorker::Limits{1,std::chrono::milliseconds(20)});
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
    EXPECT_EQ(locked.wallet_recovery_attempts,0u)
        << "a locked wallet must not start an expensive v3 origin replay";
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

TEST(WalletV3RecoveryWorker, LockedPendingDoesNotReplayUntilUnlock) {
    CanonicalRecoveryFixture f(true,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);

    const auto index_path=f.f.path/"v3-worker-locked-pending-index";
    { UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize()); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
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
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_FALSE(lease->BeginLegacyTransparentMigrationV3(index->Index(),lease->Session()).empty());
    }
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().lockWallet();
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Pending);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto direct=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
            use->Wallet(),index->Index(),session);
        EXPECT_EQ(direct.status(),Status::Invalid)
            << "direct startup recovery must defer before origin replay while locked";
    }

    RuntimeDeliveryWorker worker(f.f.service,f.wallet,
        RuntimeDeliveryWorker::Limits{1,std::chrono::milliseconds(20)});
    worker.Start();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(worker.Snapshot().slices<3 && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    worker.Stop();
    const auto locked=worker.Snapshot();
    ASSERT_GE(locked.slices,3u);
    EXPECT_EQ(locked.wallet,RuntimeDeliveryWorker::WalletOutcome::Deferred);
    EXPECT_EQ(locked.wallet_recovery_attempts,0u)
        << "a locked pending wallet must not repeat full v3 origin replay";
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Pending);
        use->Wallet().unlockWallet("canonical-fixture-pass",0);
    }
    worker.Start();
    const auto ready_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(worker.Snapshot().wallet!=RuntimeDeliveryWorker::WalletOutcome::AppliedPrefix &&
          std::chrono::steady_clock::now()<ready_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    worker.Stop();
    EXPECT_EQ(worker.Snapshot().wallet,RuntimeDeliveryWorker::WalletOutcome::AppliedPrefix);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
        WalletManager::LegacyTransparentState::Complete);
}

// Issue #939: a wallet a full-archive v3 node has not enrolled yet, while
// locked, was reported as a retryable "catching up". It can only proceed after
// an unlock, so every balance view must say exactly that.
TEST(WalletV3RecoveryRoute, LockedUnenrolledWalletReportsUnlockRequired) {
    CanonicalRecoveryFixture f(true,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    const auto index_path=f.f.path/"v3-unlock-required-index";
    { UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize()); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        use->Wallet().DisableLegacyRescansForFullArchiveV3();
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
    ExecutionContext request;
    request.daemon=&f.context;
    request.walletName="canonical-recovery";
    const auto expect_unlock_required=[&](const char* phase) {
        const auto balance=rpc_context_wallet_getbalance(request,din::obj());
        EXPECT_EQ(balance["error_code"].asString(),"wallet_unlock_required")
            << phase << "\n" << balance.toStyledString();
        EXPECT_EQ(balance["error"].asString(),WalletManager::kWalletUnlockRequiredForV3Enrollment);
        EXPECT_FALSE(balance["retryable"].asBool()) << phase;
        const auto info=rpc_context_wallet_getwalletinfo(request,din::obj());
        EXPECT_EQ(info["error_code"].asString(),"wallet_unlock_required")
            << phase << "\n" << info.toStyledString();
        EXPECT_EQ(info["balance_status"].asString(),"unlock_required") << phase;
    };
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().lockWallet();
        ASSERT_TRUE(use->Wallet().isWalletLocked());
        ASSERT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Absent);
    }
    expect_unlock_required("absent");
    {
        // Enrollment begins only with the seed; re-lock once it is Pending.
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().unlockWallet("canonical-fixture-pass",0);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        {
            auto lease=use->Wallet().AcquireDatabaseLease();
            ASSERT_FALSE(lease->BeginLegacyTransparentMigrationV3(index->Index(),lease->Session()).empty());
        }
        use->Wallet().lockWallet();
        ASSERT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Pending);
    }
    expect_unlock_required("pending");
    // Once unlocked, the remaining wait is real enrollment work: the ordinary
    // retryable "catching up" state, not an unlock request.
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().unlockWallet("canonical-fixture-pass",0);
    }
    const auto unlocked=rpc_context_wallet_getbalance(request,din::obj());
    EXPECT_EQ(unlocked["error_code"].asString(),"wallet_balance_not_current") << unlocked.toStyledString();
    EXPECT_TRUE(unlocked["retryable"].asBool());
}

namespace {
// Installs a full-archive v3 metadata index for the enclosing scope and puts
// the active wallet in the v3 configuration the daemon uses.
struct NativeV3IndexScope {
    ChainstateService& source;
    std::filesystem::path path;
    std::unique_ptr<UTXOIndex> original;
    NativeV3IndexScope(CanonicalRecoveryFixture& f,const char* name):source(*f.f.service),path(f.f.path/name) {
        { UTXOIndex v2(path.string());OrchardAdmissionFixture::Require(v2.Initialize()); }
        (void)UpgradeFullArchiveWalletIndexSchemaToV3(path);
        auto metadata=std::make_unique<UTXOIndex>(path.string());
        OrchardAdmissionFixture::Require(metadata->InitializeFullArchiveV3MetadataOnly());
        {
            auto use=WalletService::AcquireWalletUse(f.wallet);
            use->Wallet().setUTXOIndex(nullptr);
            use->Wallet().DisableLegacyRescansForFullArchiveV3();
        }
        original=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(source,std::move(metadata));
    }
    ~NativeV3IndexScope() {
        auto replacement=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(source,std::move(original));
        (void)replacement;
    }
};
int NativeTestCount(WalletManager& wallet,const std::string& sql) {
    auto lease=wallet.AcquireDatabaseLease();sqlite3_stmt* q=nullptr;
    OrchardAdmissionFixture::Require(sqlite3_prepare_v2(lease->Database(),sql.c_str(),-1,&q,nullptr)==SQLITE_OK);
    OrchardAdmissionFixture::Require(sqlite3_step(q)==SQLITE_ROW);
    const int value=sqlite3_column_int(q,0);sqlite3_finalize(q);return value;
}
std::string NativePoolAddress(WalletManager& wallet,int index) {
    auto lease=wallet.AcquireDatabaseLease();sqlite3_stmt* q=nullptr;
    OrchardAdmissionFixture::Require(sqlite3_prepare_v2(lease->Database(),
        "SELECT address FROM addresses WHERE account=0 AND change=0 AND idx=?",-1,&q,nullptr)==SQLITE_OK);
    sqlite3_bind_int(q,1,index);
    OrchardAdmissionFixture::Require(sqlite3_step(q)==SQLITE_ROW);
    std::string address(reinterpret_cast<const char*>(sqlite3_column_text(q,0)));
    sqlite3_finalize(q);return address;
}
bool HasNativeStateColumn(WalletManager& wallet) {
    return NativeTestCount(wallet,
        "SELECT count(*) FROM pragma_table_info('wallet_meta') WHERE name='runtime_native_v3_state_v1'")==1;
}
} // namespace

// Issue #939 phase 1 (replaces the interim "unsupported" contract): a native
// wallet (Orchard owner, created or restored by 8.2.x) is now enrollable on a
// v3 node. Until it is native-complete its reads refuse honestly: "unlock
// required" while locked (enrollment needs the seed), the retryable catch-up
// once unlocked. Pending also refuses new addresses.
TEST(WalletV3NativeInterim, UnenrolledNativeWalletReportsUnlockRequiredThenCatchesUp) {
    CanonicalRecoveryFixture f(true,false);   // create(): native, Orchard owner
    const auto first=f.MineToWallet();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_TRUE(lease->HasOrchardOwnerState());
        ASSERT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
    }
    NativeV3IndexScope v3(f,"v3-native-interim-index");
    ExecutionContext request;
    request.daemon=&f.context;
    request.walletName="canonical-recovery";
    const auto expect_unlock_required=[&](const char* phase) {
        const auto balance=rpc_context_wallet_getbalance(request,din::obj());
        EXPECT_EQ(balance["error_code"].asString(),"wallet_unlock_required")
            << phase << "\n" << balance.toStyledString();
        EXPECT_EQ(balance["error"].asString(),WalletManager::kWalletUnlockRequiredForV3Enrollment);
        EXPECT_FALSE(balance["retryable"].asBool()) << phase;
        const auto info=rpc_context_wallet_getwalletinfo(request,din::obj());
        EXPECT_EQ(info["balance_status"].asString(),"unlock_required") << phase;
    };
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().lockWallet();
    }
    expect_unlock_required("absent");
    std::string error;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::Deferred) << "a locked native wallet cannot enroll";
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Absent);
        use->Wallet().unlockWallet("canonical-fixture-pass",0);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        {
            auto lease=use->Wallet().AcquireDatabaseLease();
            ASSERT_FALSE(lease->BeginNativeTransparentMigrationV3(index->Index(),lease->Session()).empty());
        }
        use->Wallet().lockWallet();
    }
    expect_unlock_required("pending");
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().unlockWallet("canonical-fixture-pass",0);
    }
    const auto unlocked=rpc_context_wallet_getbalance(request,din::obj());
    EXPECT_EQ(unlocked["error_code"].asString(),"wallet_balance_not_current") << unlocked.toStyledString();
    EXPECT_TRUE(unlocked["retryable"].asBool());
    const auto address=rpc_context_wallet_getnewaddress(request,din::obj());
    EXPECT_EQ(address["error_code"].asString(),"wallet_native_address_unavailable_on_v3")
        << address.toStyledString();
    EXPECT_FALSE(address.isMember("address"));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    const auto complete=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(complete.isMember("error")) << complete.toStyledString();
    EXPECT_NEAR(complete["total"].asDouble(),double(first)/1e8,1e-8);
}

// Issue #939 phase 1: an Orchard-owner wallet the native route cannot serve
// (here: no Orchard account catalog to authenticate) keeps the interim honest
// refusal, accepts no new address, and is never enrolled in either class.
TEST(WalletV3NativeInterim, UnservableNativeWalletKeepsUnsupportedRefusal) {
    CanonicalRecoveryFixture f(false,false);
    (void)f.MineToWallet();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        ASSERT_EQ(sqlite3_exec(use->Wallet().getCurrentDatabase(),
            "DELETE FROM settings WHERE key='orchard_account_catalog_v1'",nullptr,nullptr,nullptr),SQLITE_OK);
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_TRUE(lease->HasOrchardOwnerState());
        ASSERT_FALSE(lease->NativeV3Enrollable());
    }
    NativeV3IndexScope v3(f,"v3-native-unservable-index");
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    const auto balance=rpc_context_wallet_getbalance(request,din::obj());
    EXPECT_EQ(balance["error_code"].asString(),"wallet_native_unsupported_on_v3") << balance.toStyledString();
    EXPECT_FALSE(balance["retryable"].asBool());
    const auto address=rpc_context_wallet_getnewaddress(request,din::obj());
    EXPECT_EQ(address["error_code"].asString(),"wallet_native_unsupported_on_v3") << address.toStyledString();
    std::string error;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),WalletService::CanonicalRecovery::Deferred);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_FALSE(HasNativeStateColumn(use->Wallet()));
    EXPECT_FALSE(HasLegacyStateColumn(use->Wallet()));
}

// Issue #939 phase 1: a v3 node creates wallets it can serve. A freshly
// generated identity has no history the native route cannot see, so creation
// is allowed and its sealed birth enrolls it without a genesis replay. A
// restored seed may own Orchard history that v3 cannot deliver before phase
// 3, so restoring stays refused.
TEST(WalletV3NativeInterim, V3CreatesGeneratedWalletsAndRefusesRestores) {
    CanonicalRecoveryFixture f(false,false);
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-create-index");
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_THROW(use->Wallet().createFromBip39("native-restored",
            "legal winner thank year wave sausage worth useful legal winner thank yellow",""),
            std::runtime_error);
        EXPECT_FALSE(use->Wallet().exists("native-restored"));
    }
    ExecutionContext request;
    request.daemon=&f.context;
    din::Json create(Json::arrayValue);create.append("native-rpc");
    const auto created=rpc_context_wallet_createhd(request,create);
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    EXPECT_TRUE(created.isMember("mnemonic"));
    EXPECT_FALSE(created.isMember("first_orchard_address"))
        << "no Orchard receiver before v3 can deliver Orchard notes";
    EXPECT_EQ(created["orchard_error"].asString(),WalletManager::kNativeWalletShieldedUnavailableOnV3);
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    auto use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_EQ(use->Wallet().getCurrentWalletName(),"native-rpc");
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_EQ(lease->NativeV3TransparentState(),WalletManager::NativeV3State::Complete);
        EXPECT_TRUE(lease->SealedFreshBirthHeight(lease->Session()).has_value());
    }
    // The creation-time Taproot receiver (index 0) and the pool are enrolled.
    EXPECT_EQ(NativeTestCount(use->Wallet(),"SELECT count(*) FROM addresses WHERE account=0 AND change=0"),
        WalletManager::kNativeV3ReceivePool);
}

// Issue #939: a freshly GENERATED wallet carries a sealed birth height; a
// restored one never does, and a tampered record fails closed.
TEST(WalletFreshBirth, SealedOnlyForGeneratedIdentityAndAuthenticated) {
    CanonicalRecoveryFixture f(false,false);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto& wallet=use->Wallet();
    wallet.createFromGeneratedBip39("fresh-birth",WalletManager::GenerateBip39Identity(12,""),"",155669u);
    {
        auto lease=wallet.AcquireDatabaseLease();
        EXPECT_EQ(lease->SealedFreshBirthHeight(lease->Session()),std::optional<uint32_t>(155669u));
    }
    // Tamper: any change to the sealed value must refuse, never pass as fresh.
    auto* db=wallet.getCurrentDatabase();
    ASSERT_EQ(sqlite3_exec(db,"UPDATE settings SET value=substr(value,1,length(value)-2)||'00' "
        "WHERE key='wallet_fresh_birth_v1'",nullptr,nullptr,nullptr),SQLITE_OK);
    {
        auto lease=wallet.AcquireDatabaseLease();
        EXPECT_THROW((void)lease->SealedFreshBirthHeight(lease->Session()),std::exception);
    }
    // Locked: the record cannot be read without the seed.
    wallet.createFromGeneratedBip39("fresh-birth-locked",WalletManager::GenerateBip39Identity(12,""),"",155669u);
    wallet.encryptWallet("fresh-birth-pass");
    if(!wallet.isWalletLocked())wallet.lockWallet();
    {
        auto lease=wallet.AcquireDatabaseLease();
        EXPECT_THROW((void)lease->SealedFreshBirthHeight(lease->Session()),std::exception);
    }
    wallet.unlockWallet("fresh-birth-pass",0);
    {
        auto lease=wallet.AcquireDatabaseLease();
        EXPECT_EQ(lease->SealedFreshBirthHeight(lease->Session()),std::optional<uint32_t>(155669u));
    }
    // A restore never carries the record.
    wallet.createFromBip39("restored-birth",
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","");
    {
        auto lease=wallet.AcquireDatabaseLease();
        EXPECT_EQ(lease->SealedFreshBirthHeight(lease->Session()),std::nullopt);
    }
}

namespace {
// Enroll the active wallet on a full-archive v3 metadata index after block
// 50's body was deleted. Any genesis replay must read it and defer.
WalletService::CanonicalRecovery RecoverActiveOnV3WithoutPreactivationBody(
        CanonicalRecoveryFixture& f,const char* index_name,std::string& error) {
    const auto index_path=f.f.path/index_name;
    { UTXOIndex v2(index_path.string());if(!v2.Initialize())throw std::runtime_error("v2 index"); }
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    if(!metadata->InitializeFullArchiveV3MetadataOnly())throw std::runtime_error("v3 index");
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        use->Wallet().DisableLegacyRescansForFullArchiveV3();
    }
    struct RestoreIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreIndex() {
            auto replacement=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(source,std::move(original));
            (void)replacement;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(metadata))};
    return f.wallet->RecoverActiveWalletFromCanonicalSource(&error);
}
} // namespace

// Issue #939: a fresh wallet born at/after activation enrolls on a full-archive
// v3 node through the native route without the genesis replay (proved by a
// missing pre-activation body that any replay must read). Restored and
// pre-activation-born wallets still replay, so they begin native enrollment
// and then defer on that missing body. With the body present the same
// restored wallet enrolls (positive control), and a tampered birth record
// fails closed even with every body present.
TEST(WalletFreshBirth, FreshWalletEnrollsWithoutGenesisReplay) {
    enum class Kind { FreshAtActivation, Restored, FreshBelowActivation, TamperedFresh };
    struct Case { const char* name; Kind kind; bool delete_body; bool enrolls; };
    for(const auto& c:{Case{"fresh at activation, body deleted",Kind::FreshAtActivation,true,true},
                       Case{"restored, body deleted",Kind::Restored,true,false},
                       Case{"fresh below activation, body deleted",Kind::FreshBelowActivation,true,false},
                       Case{"restored, body present (positive control)",Kind::Restored,false,true},
                       Case{"tampered fresh record, body present",Kind::TamperedFresh,false,false}}) {
        SCOPED_TRACE(c.name);
        CanonicalRecoveryFixture f(false,false);
        (void)f.MineToWallet();
        ASSERT_GE(f.f.service->GetActiveTip()->height,Params().orchard_activation_height);
        if(c.delete_body)ASSERT_EQ(f.f.db.deleteBlock(f.f.token,f.f.blocks[50].GetHash()),Status::Ok);
        {
            auto use=WalletService::AcquireWalletUse(f.wallet);
            const auto activation=Params().orchard_activation_height;
            if(c.kind==Kind::FreshAtActivation || c.kind==Kind::TamperedFresh)use->Wallet().createFromGeneratedBip39(
                "fresh-v3",WalletManager::GenerateBip39Identity(12,""),"",activation);
            else if(c.kind==Kind::Restored)use->Wallet().createFromBip39("fresh-v3",
                "legal winner thank year wave sausage worth useful legal winner thank yellow","");
            else use->Wallet().createFromGeneratedBip39("fresh-v3",
                WalletManager::GenerateBip39Identity(12,""),"",activation-1);
            ASSERT_FALSE(use->Wallet().getNewAddress().empty());
            if(c.kind==Kind::TamperedFresh)
                ASSERT_EQ(sqlite3_exec(use->Wallet().getCurrentDatabase(),
                    "UPDATE settings SET value=substr(value,1,length(value)-2)||'00' "
                    "WHERE key='wallet_fresh_birth_v1'",nullptr,nullptr,nullptr),SQLITE_OK);
        }
        std::string error;
        const auto outcome=RecoverActiveOnV3WithoutPreactivationBody(f,"fresh-v3-index",error);
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        // Native wallets never take (or write) the legacy class marker.
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
        if(c.enrolls) {
            EXPECT_EQ(outcome,WalletService::CanonicalRecovery::AppliedPrefix) << error;
            EXPECT_EQ(lease->NativeV3TransparentState(),WalletManager::NativeV3State::Complete);
            EXPECT_NO_THROW(lease->RequireOrchardWalletReady());
        } else {
            EXPECT_EQ(outcome,WalletService::CanonicalRecovery::Deferred)
                << "a wallet without verified fresh proof must still replay from genesis";
            // Begin ran (the Orchard owner is no longer a refusal); only the
            // replay deferred, so the wallet stays fail-closed Pending.
            EXPECT_EQ(lease->NativeV3TransparentState(),WalletManager::NativeV3State::Pending);
        }
        lease.reset();
        EXPECT_FALSE(HasLegacyStateColumn(use->Wallet()));
    }
}


// #939 phase 1: a native (Orchard-owner) wallet enrolls on a full-archive v3
// node in its own durable class. It keeps Orchard readiness and its full
// wallet scope, cannot grow its guarded script domain after Complete, and
// receives on a pre-derived pool address through per-block v3 delivery.
TEST(WalletV3NativeRoute, GeneratedWalletEnrollsNativeAndReceivesOnPool) {
    CanonicalRecoveryFixture f(false,false);   // create(): Orchard owner
    const auto first=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);
    NativeV3IndexScope v3(f,"v3-native-route-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    std::string pooled;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto& wallet=use->Wallet();
        {
            auto lease=wallet.AcquireDatabaseLease();
            EXPECT_EQ(lease->NativeV3TransparentState(),WalletManager::NativeV3State::Complete);
            EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
            EXPECT_NO_THROW(lease->RequireOrchardWalletReady());
        }
        EXPECT_FALSE(HasLegacyStateColumn(wallet));
        // Pool: Taproot external 0..19, each with its exact issuance path.
        EXPECT_EQ(NativeTestCount(wallet,"SELECT count(DISTINCT idx) FROM addresses "
            "WHERE account=0 AND change=0 AND idx BETWEEN 0 AND 19"),20);
        EXPECT_EQ(NativeTestCount(wallet,"SELECT count(*) FROM addresses a JOIN watch_scripts w "
            "ON lower(hex(w.script_pubkey))=lower(a.script_pubkey) AND w.path=('m/86''/1448''/0''/0/'||a.idx) "
            "WHERE a.account=0 AND a.change=0 AND a.idx BETWEEN 0 AND 19"),20);
        pooled=NativePoolAddress(wallet,7);
        EXPECT_EQ(wallet.getBlockchainHeight(),102u);
        // Complete freezes the guarded script domain.
        EXPECT_TRUE(wallet.getNewAddress().empty());
        EXPECT_TRUE(wallet.getNewChangeAddress().empty());
    }
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    const auto balance=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(balance.isMember("error")) << balance.toStyledString();
    EXPECT_FALSE(balance.isMember("wallet_scope")) << balance.toStyledString();
    EXPECT_NEAR(balance["total"].asDouble(),double(first)/1e8,1e-8);
    const auto address=rpc_context_wallet_getnewaddress(request,din::obj());
    EXPECT_EQ(address["error_code"].asString(),"wallet_native_address_unavailable_on_v3")
        << address.toStyledString();
    EXPECT_FALSE(address.isMember("address"));
    // A label is an UPDATE of the guarded addresses table: refused.
    din::Json label(Json::arrayValue);label.append(pooled);label.append("savings");
    const auto labelled=rpc_context_wallet_setlabel(request,label);
    EXPECT_TRUE(labelled.isMember("error")) << labelled.toStyledString();
    EXPECT_FALSE(labelled.isMember("success"));
    // Orchard effects need a wallet binding; v3 cannot deliver Orchard yet.
    din::Json binding;binding["wallet_name"]="canonical-recovery";
    const auto bound=rpc_context_wallet_orchard_getwalletbinding(request,binding);
    EXPECT_EQ(bound["error_code"].asString(),"wallet_native_shielded_unavailable_on_v3") << bound.toStyledString();
    EXPECT_FALSE(bound.isMember("wallet_binding"));
    // Both receipts survived the refused issuance, label and binding.
    const auto still=rpc_context_wallet_getbalance(request,din::obj());
    EXPECT_FALSE(still.isMember("error")) << still.toStyledString();

    f.receive_address=pooled;
    const auto second=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),index->Index(),session);
        const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
        ASSERT_TRUE(indexed);ASSERT_TRUE(ordinary);
        EXPECT_EQ(indexed->cursor,ordinary->cursor);
        EXPECT_EQ(indexed->tip_height,103u);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),103u);
        EXPECT_EQ(NativeTestCount(use->Wallet(),"SELECT count(*) FROM utxos WHERE address='"+pooled+"'"),1);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Complete);
    }
    const auto after=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(after.isMember("error")) << after.toStyledString();
    EXPECT_NEAR(after["total"].asDouble(),double(first+second)/1e8,1e-8);
}

// #939 phase 1: native receipts survive a full service/index restart, and a
// LOCKED native-complete wallet keeps receiving per-block delivery (only
// enrollment needs the seed).
TEST(WalletV3NativeRoute, NativeCompleteSurvivesRestartAndDeliversWhileLocked) {
    CanonicalRecoveryFixture f(true,false);
    const auto first=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);
    NativeV3IndexScope v3(f,"v3-native-restart-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    RuntimeOutboxCursor h102;
    std::string pooled;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),index->Index(),session);
        ASSERT_TRUE(indexed);h102=indexed->cursor;
        pooled=NativePoolAddress(use->Wallet(),3);
    }
    f.wallet->Stop();
    f.context.wallet.reset();
    f.wallet.reset();
    f.wallet=std::make_shared<WalletService>();
    f.context.wallet=f.wallet;
    auto reopened=std::make_unique<UTXOIndex>(v3.path.string());
    ASSERT_TRUE(reopened->InitializeFullArchiveV3MetadataOnly());
    auto closed=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(*f.f.service,std::move(reopened));
    ASSERT_TRUE(closed);closed.reset();
    ASSERT_TRUE(f.wallet->Init(f.context));
    ASSERT_TRUE(f.wallet->Start());
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        ASSERT_EQ(use->Wallet().getCurrentWalletName(),"canonical-recovery");
        EXPECT_TRUE(use->Wallet().isLocked());
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Complete);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto indexed=RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),index->Index(),session);
        const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
        ASSERT_TRUE(indexed);ASSERT_TRUE(ordinary);
        EXPECT_EQ(indexed->cursor,h102);
        EXPECT_EQ(ordinary->cursor,h102);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),102u);
    }
    const auto reopened_balance=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(reopened_balance.isMember("error")) << reopened_balance.toStyledString();
    EXPECT_FALSE(reopened_balance.isMember("wallet_scope"));
    EXPECT_NEAR(reopened_balance["total"].asDouble(),double(first)/1e8,1e-8);

    f.receive_address=pooled;
    const auto second=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_TRUE(use->Wallet().isLocked());
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),103u);
    }
    const auto delivered=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(delivered.isMember("error")) << delivered.toStyledString();
    EXPECT_NEAR(delivered["total"].asDouble(),double(first+second)/1e8,1e-8);
    f.wallet->Stop();
}

// #939 phase 1: an interrupted native reconcile (index receipt committed,
// ordinary receipt not) stays fail-closed Pending, refuses reads, and the
// next pass resumes from the proved index receipt to Complete.
TEST(WalletV3NativeRoute, InterruptedNativeReconcileStaysPendingThenResumes) {
    CanonicalRecoveryFixture f(false,false);
    const auto first=f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-interrupted-index");
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        std::string identity;
        {
            auto lease=use->Wallet().AcquireDatabaseLease();
            identity=lease->EnsureDeliveryIdentity();
            EXPECT_EQ(lease->BeginNativeTransparentMigrationV3(index->Index(),session),identity)
                << "native enrollment keeps the existing delivery identity";
            EXPECT_EQ(lease->NativeV3TransparentState(),WalletManager::NativeV3State::Pending);
            EXPECT_THROW((void)lease->BeginNativeTransparentMigrationV3(index->Index(),session),std::exception);
            EXPECT_THROW((void)lease->BeginLegacyTransparentMigrationV3(index->Index(),session),std::exception);
        }
        EXPECT_FALSE(HasLegacyStateColumn(use->Wallet()));
        EXPECT_TRUE(HasNativeStateColumn(use->Wallet()));
        EXPECT_TRUE(use->Wallet().getNewAddress().empty()) << "Pending freezes the script domain";
        const auto coverage=f.f.service->getRuntimeWalletCoverageV3(use->Wallet(),session,index->Index());
        ASSERT_TRUE(coverage.ok()) << static_cast<int>(coverage.status());
        int commits=0;
        EXPECT_THROW(RuntimeOrdinaryDelivery::ReconcileNativeTransparentCoverageV3(
            use->Wallet(),index->Index(),**coverage,[&](const auto& commit) {
                auto selected=f.f.service->AcquireBlockIngressActivationLock();
                if(++commits==2)throw std::runtime_error("injected ordinary commit failure");
                commit();
            }),std::runtime_error);
        EXPECT_EQ(commits,2);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Pending);
        EXPECT_TRUE(RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),index->Index(),session));
        EXPECT_FALSE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session));
    }
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    const auto pending=rpc_context_wallet_getbalance(request,din::obj());
    EXPECT_EQ(pending["error_code"].asString(),"wallet_balance_not_current") << pending.toStyledString();
    EXPECT_FALSE(pending.isMember("total"));
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    EXPECT_EQ(WalletService::AcquireWalletUse(f.wallet)->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
        WalletManager::NativeV3State::Complete);
    const auto complete=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(complete.isMember("error")) << complete.toStyledString();
    EXPECT_NEAR(complete["total"].asDouble(),double(first)/1e8,1e-8);
}

// #939 phase 1: the native reconcile captures the Orchard ownership inventory
// before its index commit and re-authenticates it in the ordinary commit. A
// change in between (here the catalog row vanishes inside the index phase's
// wallet transaction) refuses before the ordinary commit; the wallet stays
// fail-closed Pending.
TEST(WalletV3NativeRoute, OrchardOwnershipChangeDuringReconcileStaysPending) {
    CanonicalRecoveryFixture f(false,false);
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-orchard-change-index");
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_FALSE(lease->BeginNativeTransparentMigrationV3(index->Index(),session).empty());
    }
    const auto coverage=f.f.service->getRuntimeWalletCoverageV3(use->Wallet(),session,index->Index());
    ASSERT_TRUE(coverage.ok()) << static_cast<int>(coverage.status());
    int commits=0;
    EXPECT_THROW(RuntimeOrdinaryDelivery::ReconcileNativeTransparentCoverageV3(
        use->Wallet(),index->Index(),**coverage,[&](const auto& commit) {
            auto selected=f.f.service->AcquireBlockIngressActivationLock();
            if(++commits==1)
                OrchardAdmissionFixture::Require(sqlite3_exec(use->Wallet().getCurrentDatabase(),
                    "DELETE FROM settings WHERE key='orchard_account_catalog_v1'",nullptr,nullptr,nullptr)==SQLITE_OK);
            commit();
        }),std::runtime_error);
    EXPECT_EQ(commits,1) << "the ordinary commit must refuse before its source commit";
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
        WalletManager::NativeV3State::Pending);
    EXPECT_FALSE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session));
}

// #939 phase 1: a legacy (no Orchard owner) wallet keeps the legacy route,
// its legacy-only scope, and never acquires the native class column.
TEST(WalletV3NativeRoute, LegacyWalletKeepsLegacyRoute) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-legacy-still-legacy-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Complete);
        EXPECT_EQ(lease->NativeV3TransparentState(),WalletManager::NativeV3State::Absent);
        lease.reset();
        EXPECT_FALSE(HasNativeStateColumn(use->Wallet()));
    }
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    const auto balance=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(balance.isMember("error")) << balance.toStyledString();
    EXPECT_EQ(balance["wallet_scope"].asString(),"legacy_transparent_only");
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

// Older wallet writers marked spent coins without recording the spender. A
// full-archive v3 recovery can fill that pair only after its checked replay
// proves the creation and a single spending version. Explicit but incorrect
// metadata must still refuse rather than be silently replaced.
TEST(WalletV3RecoveryRoute, LegacySpentCoinWithoutSpenderRecoversOnlyFromProvedReplay) {
    CanonicalRecoveryFixture f(false,true);
    const auto [historical_amount,historical_total]=f.PrepareFundedHistoricalWallet();
    (void)historical_amount;
    f.DowngradeToPreIdentityTransparentWallet();
    const auto receiver=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{53},0);
    const auto shield=f.ShieldHistoricalWalletCoin(receiver);
    const auto spender=shield.GetTxid().AsUint256().GetHex();
    (void)f.Mine(shield);
    const auto mined_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    const auto spent_txid=f.f.blocks.at(101).vtx.at(1).GetTxid().AsUint256().GetHex();

    // Reproduce the historical WalletManager::spendUTXO writer, which set
    // is_spent but left both later-added spender columns NULL.
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        sqlite3_stmt* reset=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
            "UPDATE utxos SET is_spent=0,spent_txid=NULL,spent_height=NULL WHERE txid=? AND vout=0",
            -1,&reset,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(reset,1,spent_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(reset),SQLITE_DONE);
        EXPECT_EQ(sqlite3_changes(lease->Database()),1);
        sqlite3_finalize(reset);
        ASSERT_TRUE(use->Wallet().spendUTXO(spent_txid,0));
        sqlite3_stmt* old_row=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
            "SELECT is_spent,spent_txid,spent_height FROM utxos WHERE txid=? AND vout=0",
            -1,&old_row,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(old_row,1,spent_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(old_row),SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(old_row,0),1);
        EXPECT_EQ(sqlite3_column_type(old_row,1),SQLITE_NULL);
        EXPECT_EQ(sqlite3_column_type(old_row,2),SQLITE_NULL);
        sqlite3_finalize(old_row);
    }

    const auto index_path=f.f.path/"v3-legacy-spenderless-index";
    {UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize());}
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        use->Wallet().DisableLegacyRescansForFullArchiveV3();
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

    const auto set_spender=[&](const char* sql) {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        sqlite3_stmt* change=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),sql,-1,&change,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(change,1,spent_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(change),SQLITE_DONE);
        EXPECT_EQ(sqlite3_changes(lease->Database()),1);
        sqlite3_finalize(change);
    };
    const auto refuse=[&] {
        std::string error;
        EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::Deferred);
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Pending);
        EXPECT_FALSE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),
            use->Wallet().AcquireDatabaseLease()->Session()));
    };

    set_spender("UPDATE utxos SET spent_txid='eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',"
        "spent_height=102 WHERE txid=? AND vout=0");
    refuse(); // A populated but forged spender pair cannot be repaired.
    set_spender("UPDATE utxos SET spent_txid=NULL,spent_height=102 WHERE txid=? AND vout=0");
    refuse(); // One missing member is not the historical NULL/NULL form.
    set_spender("UPDATE utxos SET spent_txid=NULL,spent_height=NULL WHERE txid=? AND vout=0");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto lease=use->Wallet().AcquireDatabaseLease();
    EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Complete);
    const auto session=lease->Session();
    lease.reset();
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    EXPECT_TRUE(RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),index->Index(),session));
    EXPECT_TRUE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session));
    lease=use->Wallet().AcquireDatabaseLease();
    sqlite3_stmt* restored=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
        "SELECT is_spent,spent_txid,spent_height FROM utxos WHERE txid=? AND vout=0",
        -1,&restored,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(restored,1,spent_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(restored),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(restored,0),1);
    EXPECT_EQ(reinterpret_cast<const char*>(sqlite3_column_text(restored,1)),spender);
    EXPECT_EQ(sqlite3_column_int(restored,2),102);
    sqlite3_finalize(restored);
    lease.reset();
    EXPECT_NEAR(use->Wallet().getBalance().total,
        historical_total-0.006+double(mined_amount)/100000000.0,0.00000001);
    index.reset();use.reset();
    error.clear();
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_NEAR(use->Wallet().getBalance().total,
        historical_total-0.006+double(mined_amount)/100000000.0,0.00000001);
}

// Older wallets can retain a proved spent coin while lacking its historical
// transaction row. The full-archive v3 source has the signed block body and
// all of its owned inputs; restore the on-chain net effect without claiming a
// recipient, fee or original local payment intent. A forged coin still fails.
TEST(WalletV3RecoveryRoute, MissingProvedHistoricalSpendRecoversWithoutInventingLocalIntent) {
    CanonicalRecoveryFixture f(false,true,true);
    const auto [historical_amount,historical_total]=f.PrepareFundedHistoricalWallet();
    ASSERT_EQ(f.f.blocks.at(101).vtx.size(),3u);
    const auto retained_id=f.f.blocks.at(101).vtx.at(1).GetTxid().AsUint256().GetHex();
    const auto spend_id=f.f.blocks.at(101).vtx.at(2).GetTxid().AsUint256().GetHex();
    double original_net=0;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        sqlite3_stmt* original=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
            "SELECT amount,category FROM transactions WHERE txid=?",-1,&original,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(original,1,spend_id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(original),SQLITE_ROW);
        original_net=sqlite3_column_double(original,0);
        EXPECT_LT(original_net,0.0);
        EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(original,1)),"send");
        sqlite3_finalize(original);
        sqlite3_stmt* keep=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
            "UPDATE transactions SET label='legacy-kept' WHERE txid=?",-1,&keep,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(keep,1,retained_id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(keep),SQLITE_DONE);
        EXPECT_EQ(sqlite3_changes(lease->Database()),1);
        sqlite3_finalize(keep);
        sqlite3_stmt* remove=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
            "DELETE FROM transactions WHERE txid=?",-1,&remove,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(remove,1,spend_id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(remove),SQLITE_DONE);
        EXPECT_EQ(sqlite3_changes(lease->Database()),1);
        sqlite3_finalize(remove);
    }
    f.DowngradeToPreIdentityTransparentWallet();
    const auto mined_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);

    const auto index_path=f.f.path/"v3-missing-historical-spend-index";
    {UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize());}
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().setUTXOIndex(nullptr);
        use->Wallet().DisableLegacyRescansForFullArchiveV3();
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

    // This is a signed same-block child of an owned H101 output, not the
    // post-origin H102 shield spend already handled by the old recovery path.
    // Require the selected checked source to contain that input and the v3
    // origin to be H101 before asserting the old path's RED refusal.
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        uint64_t session=0;
        {
            auto lease=use->Wallet().AcquireDatabaseLease();
            session=lease->Session();
            ASSERT_FALSE(lease->BeginLegacyTransparentMigrationV3(index->Index(),session).empty());
        }
        const auto captured=f.f.service->getRuntimeWalletCoverageV3(
            use->Wallet(),session,index->Index());
        ASSERT_TRUE(captured.ok()) << static_cast<int>(captured.status());
        EXPECT_EQ((*captured)->Origin().OriginHeight(),101u);
        const auto id=TxId(uint256::FromHexUnsafe(spend_id));
        const auto selected=(*captured)->Transactions().find(id);
        ASSERT_NE(selected,(*captured)->Transactions().end());
        EXPECT_EQ(selected->second.height,101u);
        const auto parent=TxOutPoint{f.f.blocks.at(101).vtx.at(1).GetTxid(),0};
        EXPECT_TRUE(std::find(selected->second.inputs.begin(),selected->second.inputs.end(),parent)
            != selected->second.inputs.end());
        EXPECT_TRUE((*captured)->ObservedCoins().contains(parent));
    }

    // A row whose amount disagrees with the authenticated historical body is
    // still refused, even though the missing transaction could be rebuilt.
    const auto change_id=spend_id;
    const auto set_change_amount=[&](int64_t amount) {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        sqlite3_stmt* row=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
            "UPDATE utxos SET amount=? WHERE txid=? AND vout=0",-1,&row,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_int64(row,1,amount),SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(row,2,change_id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(row),SQLITE_DONE);
        EXPECT_EQ(sqlite3_changes(lease->Database()),1);
        sqlite3_finalize(row);
    };
    set_change_amount(int64_t(historical_amount)+1);
    std::string error;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::Deferred);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Pending);
    }
    set_change_amount(int64_t(historical_amount));
    error.clear();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;

    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto lease=use->Wallet().AcquireDatabaseLease();
    EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Complete);
    sqlite3_stmt* spend=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
        "SELECT category,label,amount,height,is_coinbase FROM transactions WHERE txid=?",
        -1,&spend,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(spend,1,spend_id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(spend),SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(spend,0)),"send");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(spend,1)),"Recovered on-chain spend");
    EXPECT_NEAR(sqlite3_column_double(spend,2),original_net,0.00000001);
    EXPECT_EQ(sqlite3_column_int(spend,3),101);
    EXPECT_EQ(sqlite3_column_int(spend,4),0);
    EXPECT_EQ(sqlite3_step(spend),SQLITE_DONE);
    sqlite3_finalize(spend);
    sqlite3_stmt* retained=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
        "SELECT label FROM transactions WHERE txid=?",-1,&retained,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(retained,1,retained_id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(retained),SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(retained,0)),"legacy-kept");
    sqlite3_finalize(retained);
    lease.reset();
    EXPECT_NEAR(use->Wallet().getBalance().total,
        historical_total+double(mined_amount)/100000000.0,0.00000001);
}

} // namespace dinero
