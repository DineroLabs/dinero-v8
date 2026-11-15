#pragma once

#include "wallet/wallet_index_v3_schema.h"

namespace dinero {

// Direct service exercise: no daemon startup or alternate reset/verify logic.
struct WalletV3MaintenanceTestAccess {
    static void InstallIndex(ChainstateService& service, std::unique_ptr<UTXOIndex> index) {
        service.utxo_index_ = std::move(index);
        service.logger_ = std::make_shared<LoggerService>("");
    }
    static bool Verify(ChainstateService& service) { return service.VerifyUTXOSetMatch(); }
};

namespace {
struct WalletV3MaintenanceFixture {
    std::filesystem::path dir;
    std::filesystem::path index_path;

    WalletV3MaintenanceFixture() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = std::filesystem::temp_directory_path() /
            ("dinero-v3-maintenance-" + std::to_string(unique));
        std::filesystem::create_directories(dir);
        index_path = dir / "wallet.db";
    }
    ~WalletV3MaintenanceFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
    void CreateIndex(bool v3) const {
        {
            UTXOIndex legacy(index_path.string());
            ASSERT_TRUE(legacy.Initialize());
            assumeutxo::AssumeUtxoLifecycle lifecycle(&legacy, nullptr);
            lifecycle.ForceFatal("v3 maintenance fixture fatal marker");
            ASSERT_TRUE(legacy.SetMetadata("assumeutxo_coins_loaded", "0"));
        }
        if (v3) {
            const auto upgraded = UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
            ASSERT_TRUE(std::filesystem::is_regular_file(upgraded.rollback_backup));
        }
    }
    std::unique_ptr<UTXOIndex> OpenIndex(bool v3) const {
        auto index = std::make_unique<UTXOIndex>(index_path.string());
        EXPECT_TRUE(v3 ? index->InitializeFullArchiveV3MetadataOnly() : index->Initialize());
        return index;
    }
};
} // namespace

TEST(WalletV3Maintenance, FatalResetRefusesBeforeMarkerMutation) {
    WalletV3MaintenanceFixture fixture;
    fixture.CreateIndex(true);
    ChainstateService service;
    auto index = fixture.OpenIndex(true);
    ASSERT_TRUE(index->IsFullArchiveV3MetadataOnly());
    const auto before_state = index->GetMetadata(assumeutxo::kLifecycleStateKey);
    const auto before_reason = index->GetMetadata(assumeutxo::kFatalReasonKey);
    ASSERT_EQ(before_state, "fatal_mismatch");
    ASSERT_TRUE(before_reason.has_value());
    WalletV3MaintenanceTestAccess::InstallIndex(service, std::move(index));
    auto* lifecycle = service.GetAssumeUtxoLifecycle();
    lifecycle->RestoreFromPersistence(true);
    ASSERT_EQ(lifecycle->GetState(), assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch);

    EXPECT_NO_THROW(EXPECT_FALSE(service.ResetAssumeUtxoFatalState(assumeutxo::kResetToken)));
    EXPECT_EQ(lifecycle->GetState(), assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch);
    EXPECT_EQ(service.utxoIndex()->GetMetadata(assumeutxo::kLifecycleStateKey), before_state);
    EXPECT_EQ(service.utxoIndex()->GetMetadata(assumeutxo::kFatalReasonKey), before_reason);
    EXPECT_EQ(service.utxoIndex()->GetMetadata("assumeutxo_coins_loaded"), "0");
}

TEST(WalletV3Maintenance, VerificationRefusesOwnerScopedWalletRows) {
    WalletV3MaintenanceFixture fixture;
    fixture.CreateIndex(true);
    ChainstateService service;
    auto index = fixture.OpenIndex(true);
    ASSERT_TRUE(index->IsFullArchiveV3MetadataOnly());
    WalletV3MaintenanceTestAccess::InstallIndex(service, std::move(index));
    // A zero wallet count would accidentally equal the seeded snapshot count.
    // This cannot certify the consensus-owned UTXO set on v3.
    EXPECT_NO_THROW(EXPECT_FALSE(WalletV3MaintenanceTestAccess::Verify(service)));
    EXPECT_EQ(service.utxoIndex()->GetMetadata(assumeutxo::kLifecycleStateKey), "fatal_mismatch");
}

TEST(WalletV3Maintenance, VerificationRefusesMissingLegacyCountMarker) {
    WalletV3MaintenanceFixture fixture;
    fixture.CreateIndex(true);
    ChainstateService service;
    auto index = fixture.OpenIndex(true);
    ASSERT_TRUE(index->IsFullArchiveV3MetadataOnly());
    ASSERT_TRUE(index->DeleteMetadata("assumeutxo_coins_loaded"));
    ASSERT_FALSE(index->GetMetadata("assumeutxo_coins_loaded"));
    WalletV3MaintenanceTestAccess::InstallIndex(service, std::move(index));
    // The legacy implementation skipped its count check and reported success.
    EXPECT_FALSE(WalletV3MaintenanceTestAccess::Verify(service));
}

TEST(WalletV3Maintenance, LegacyV2ResetAndVerificationRemainAvailable) {
    WalletV3MaintenanceFixture fixture;
    fixture.CreateIndex(false);
    ChainstateService service;
    WalletV3MaintenanceTestAccess::InstallIndex(service, fixture.OpenIndex(false));
    auto* lifecycle = service.GetAssumeUtxoLifecycle();
    lifecycle->RestoreFromPersistence(true);
    ASSERT_EQ(lifecycle->GetState(), assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch);
    EXPECT_TRUE(WalletV3MaintenanceTestAccess::Verify(service));
    EXPECT_TRUE(service.ResetAssumeUtxoFatalState(assumeutxo::kResetToken));
    EXPECT_EQ(lifecycle->GetState(), assumeutxo::AssumeUtxoLifecycle::State::Disabled);
    EXPECT_FALSE(service.utxoIndex()->GetMetadata(assumeutxo::kFatalReasonKey));
}

} // namespace dinero
