#include "wallet/covenant_profile.h"
#include "wallet/wallet_manager.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

using namespace dinero;
using namespace dinero::wallet::covenant;

class CovenantWalletRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
            ("dinero_covenant_wallet_recovery_" +
             std::to_string(stamp));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    std::filesystem::path root_;
};

CovenantDescriptorRecord Record(
    const CTVPlan& plan,
    const std::string& label = {}) {
    CovenantDescriptorRecord result;
    result.descriptor_id = plan.descriptorId;
    result.profile = "ctv";
    result.descriptor = plan.recoveryDescriptor;
    result.script_pubkey = plan.taproot.scriptPubKey;
    result.label = label;
    return result;
}

CovenantDescriptorRecord Record(
    const VaultPlan& plan,
    const std::string& label = {}) {
    CovenantDescriptorRecord result;
    result.descriptor_id = plan.descriptorId;
    result.profile = "vault";
    result.descriptor = plan.recoveryDescriptor;
    result.script_pubkey = plan.delayedUnvault.scriptPubKey;
    result.label = label;
    return result;
}

CovenantDescriptorRecord Record(
    const CCVPlan& plan,
    const std::string& label = {},
    const std::string& parent = {}) {
    CovenantDescriptorRecord result;
    result.descriptor_id = plan.descriptorId;
    result.profile = "ccv";
    result.descriptor = plan.recoveryDescriptor;
    result.script_pubkey = plan.taproot.scriptPubKey;
    result.label = label;
    result.parent_descriptor_id = parent;
    return result;
}

TEST_F(
    CovenantWalletRecoveryTest,
    VaultDescriptorSurvivesWalletSwitchRestartAndSameWalletRecovery) {
    std::vector<uint8_t> hotSecret(32, 0);
    std::vector<uint8_t> recoverySecret(32, 0);
    hotSecret.back() = 31;
    recoverySecret.back() = 32;
    const VaultPlan vault = BuildVaultPlan(
        288,
        OwnerXOnlyPublicKey(hotSecret),
        OwnerXOnlyPublicKey(recoverySecret),
        "m/86'/1448'/0'/2/1",
        "m/86'/1448'/0'/3/1");

    {
        WalletManager wallet(root_);
        wallet.create("owner");
        wallet.open("owner");
        ASSERT_TRUE(wallet.storeCovenantDescriptor(Record(vault, "personal-vault")));
        wallet.create("other");
        wallet.open("other");
        EXPECT_TRUE(wallet.listCovenantDescriptors().empty());
        wallet.open("owner");
        ASSERT_EQ(wallet.listCovenantDescriptors().size(), 1U);
    }
    {
        WalletManager restarted(root_);
        restarted.open("owner");
        const auto record = restarted.getCovenantDescriptor(vault.descriptorId);
        ASSERT_TRUE(record.has_value());
        const VaultPlan recovered = RecoverVaultPlan(record->descriptor);
        EXPECT_EQ(recovered.delayedUnvault.scriptPubKey,
                  vault.delayedUnvault.scriptPubKey);
        EXPECT_EQ(recovered.recoveryKeyOrigin, vault.recoveryKeyOrigin);
    }
}

TEST_F(
    CovenantWalletRecoveryTest,
    MigrationPersistsDescriptorsAndWatchScriptsAcrossRestart) {
    const CTVPlan ctv = BuildCTVPlan(
        {0xfffffffeU},
        0,
        {Output{AmountUna::Una(50'000), {0x51}}});
    const CCVPlan ccv = BuildCCVPlan(7, {0xaa, 0xbb});
    std::vector<uint8_t> ownerPrivateKey(32, 0);
    ownerPrivateKey.back() = 9;
    const CCVPlan ownerCcv = BuildOwnerAuthorizedCCVPlan(
        3,
        {0xcc},
        OwnerXOnlyPublicKey(ownerPrivateKey),
        "m/86'/1448'/0'/0/9");

    {
        WalletManager wallet(root_);
        wallet.create("recovery");
        wallet.open("recovery");

        EXPECT_TRUE(wallet.storeCovenantDescriptor(
            Record(ctv, "vault-template")));
        EXPECT_TRUE(wallet.storeCovenantDescriptor(
            Record(ccv, "state-7")));
        EXPECT_TRUE(wallet.storeCovenantDescriptor(
            Record(ownerCcv, "owned-state-3")));
        EXPECT_TRUE(wallet.storeCovenantDescriptor(
            Record(ctv, "ignored-idempotent-label")));

        const auto records = wallet.listCovenantDescriptors();
        ASSERT_EQ(records.size(), 3U);
        EXPECT_TRUE(wallet.getCovenantDescriptor(
            ctv.descriptorId).has_value());
        EXPECT_TRUE(wallet.getCovenantDescriptor(
            ownerCcv.descriptorId).has_value());
        std::optional<std::vector<uint8_t>> covenantKey;
        EXPECT_NO_THROW(
            covenantKey = wallet.deriveKeyForScriptPubKey(
                TransactionSerializer::ToHex(
                    ctv.taproot.scriptPubKey)));
        EXPECT_FALSE(covenantKey.has_value())
            << "NUMS covenant watch scripts must never be treated as "
               "wallet-owned HD keys";

        sqlite3_stmt* statement = nullptr;
        ASSERT_EQ(
            sqlite3_prepare_v2(
                wallet.getCurrentDatabase(),
                "SELECT COUNT(*) FROM watch_scripts "
                "WHERE script_pubkey IN (?, ?, ?)",
                -1,
                &statement,
                nullptr),
            SQLITE_OK);
        sqlite3_bind_blob(
            statement, 1,
            ctv.taproot.scriptPubKey.data(),
            static_cast<int>(ctv.taproot.scriptPubKey.size()),
            SQLITE_TRANSIENT);
        sqlite3_bind_blob(
            statement, 2,
            ccv.taproot.scriptPubKey.data(),
            static_cast<int>(ccv.taproot.scriptPubKey.size()),
            SQLITE_TRANSIENT);
        sqlite3_bind_blob(
            statement, 3,
            ownerCcv.taproot.scriptPubKey.data(),
            static_cast<int>(ownerCcv.taproot.scriptPubKey.size()),
            SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(statement, 0), 3);
        sqlite3_finalize(statement);

        statement = nullptr;
        ASSERT_EQ(
            sqlite3_prepare_v2(
                wallet.getCurrentDatabase(),
                "PRAGMA user_version",
                -1,
                &statement,
                nullptr),
            SQLITE_OK);
        ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(statement, 0), 27);
        sqlite3_finalize(statement);
    }

    {
        WalletManager restarted(root_);
        restarted.open("recovery");
        const auto ctvRecord =
            restarted.getCovenantDescriptor(ctv.descriptorId);
        const auto ccvRecord =
            restarted.getCovenantDescriptor(ccv.descriptorId);
        const auto ownerCcvRecord =
            restarted.getCovenantDescriptor(ownerCcv.descriptorId);
        ASSERT_TRUE(ctvRecord.has_value());
        ASSERT_TRUE(ccvRecord.has_value());
        ASSERT_TRUE(ownerCcvRecord.has_value());
        EXPECT_EQ(
            RecoverCTVPlan(ctvRecord->descriptor).taproot.scriptPubKey,
            ctv.taproot.scriptPubKey);
        EXPECT_EQ(
            RecoverCCVPlan(ccvRecord->descriptor).state.stateHash,
            ccv.state.stateHash);
        const CCVPlan recoveredOwner =
            RecoverCCVPlan(ownerCcvRecord->descriptor);
        EXPECT_EQ(
            recoveredOwner.authorization,
            CCVAuthorization::OwnerSchnorr);
        EXPECT_EQ(recoveredOwner.ownerPublicKey, ownerCcv.ownerPublicKey);
        EXPECT_EQ(recoveredOwner.ownerKeyOrigin, ownerCcv.ownerKeyOrigin);
        EXPECT_EQ(
            recoveredOwner.taproot.scriptPubKey,
            ownerCcv.taproot.scriptPubKey);
    }
}

TEST_F(
    CovenantWalletRecoveryTest,
    SuccessorLineageIsDurableAndCollisionsFailClosed) {
    const CCVPlan current = BuildCCVPlan(11, {0x01});
    const CCVTransition transition = BuildCCVTransition(
        current,
        {Input{
            TxOutPoint{
                TxId(uint256::FromHexUnsafe(
                    "000000000000000000000000000000000000000000000000000000000000cc11")),
                0},
            0xfffffffeU}},
        AmountUna::Una(75'000),
        {0x02});

    WalletManager wallet(root_);
    wallet.create("lineage");
    wallet.open("lineage");
    EXPECT_TRUE(wallet.storeCovenantDescriptor(
        Record(current, "current")));
    EXPECT_TRUE(wallet.storeCovenantDescriptor(
        Record(
            transition.successor,
            "successor",
            current.descriptorId)));

    const auto successor =
        wallet.getCovenantDescriptor(
            transition.successor.descriptorId);
    ASSERT_TRUE(successor.has_value());
    EXPECT_EQ(
        successor->parent_descriptor_id,
        current.descriptorId);

    auto collision = Record(current, "collision");
    collision.script_pubkey[2] ^= 0x01;
    EXPECT_FALSE(wallet.storeCovenantDescriptor(collision));
    EXPECT_EQ(wallet.listCovenantDescriptors().size(), 2U);
}

TEST_F(
    CovenantWalletRecoveryTest,
    ExistingWatchPathCollisionRollsBackDescriptorAtomically) {
    const CTVPlan plan = BuildCTVPlan(
        {0xfffffffeU},
        0,
        {Output{AmountUna::Una(50'000), {0x51}}});

    WalletManager wallet(root_);
    wallet.create("watch-collision");
    wallet.open("watch-collision");

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            wallet.getCurrentDatabase(),
            "INSERT INTO watch_scripts "
            "(script_pubkey, path, is_change, last_seen_height) "
            "VALUES (?, 'existing-watch-path', 0, 0)",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    sqlite3_bind_blob(
        statement, 1,
        plan.taproot.scriptPubKey.data(),
        static_cast<int>(plan.taproot.scriptPubKey.size()),
        SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);

    EXPECT_FALSE(wallet.storeCovenantDescriptor(Record(plan)));
    EXPECT_FALSE(wallet.getCovenantDescriptor(plan.descriptorId).has_value())
        << "descriptor and watch registration must commit or roll back together";

    statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            wallet.getCurrentDatabase(),
            "SELECT path FROM watch_scripts WHERE script_pubkey = ?",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    sqlite3_bind_blob(
        statement, 1,
        plan.taproot.scriptPubKey.data(),
        static_cast<int>(plan.taproot.scriptPubKey.size()),
        SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        std::string(reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0))),
        "existing-watch-path");
    sqlite3_finalize(statement);
}

} // namespace
