/**
 * @file test_wallet_sidecar_migration.cpp
 * @brief Verifies explicit legacy sidecar migration is idempotent and DB-authoritative.
 */

#include "wallet/hd_wallet.h"
#include "consensus/coin_type.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(cond, msg)                                                    \
    do {                                                                          \
        g_tests_run++;                                                            \
        if (!(cond)) {                                                            \
            std::cerr << "  FAIL: " << msg << "\n";                               \
            std::cerr << "    at " << __FILE__ << ":" << __LINE__ << "\n";      \
            return false;                                                         \
        }                                                                         \
        g_tests_passed++;                                                         \
    } while (0)

#define ASSERT_EQ(a, b, msg)                                                      \
    do {                                                                          \
        g_tests_run++;                                                            \
        if ((a) != (b)) {                                                         \
            std::cerr << "  FAIL: " << msg << "\n";                               \
            std::cerr << "    expected: " << (b) << "\n";                         \
            std::cerr << "    got:      " << (a) << "\n";                         \
            std::cerr << "    at " << __FILE__ << ":" << __LINE__ << "\n";      \
            return false;                                                         \
        }                                                                         \
        g_tests_passed++;                                                         \
    } while (0)

static fs::path MakeTempDir() {
    const auto nonce = static_cast<unsigned long long>(std::rand());
    fs::path dir = fs::temp_directory_path() /
                   ("dinero_wallet_sidecar_migration_" + std::to_string(nonce));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static std::string BuildDeterministicSeedHex() {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(128);  // 64 bytes

    for (int i = 0; i < 64; ++i) {
        const uint8_t b = static_cast<uint8_t>(i);
        out.push_back(kHex[(b >> 4) & 0x0f]);
        out.push_back(kHex[b & 0x0f]);
    }

    return out;
}

static bool WriteLegacyWalletConf(const fs::path& datadir, const std::string& seed_hex) {
    std::ofstream out(datadir / "wallet.conf", std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << "encrypted=0\n";
    out << "coin_type=" << dinero::consensus::DINERO_COIN_TYPE << "\n";
    out << "next_index=7\n";
    out << "next_change_index=3\n";
    out << "next_mining_index=2\n";
    out << "next_taproot_index=11\n";
    out << "next_taproot_change_index=5\n";
    out << "next_taproot_mining_index=4\n";
    out << "seed_hex=" << seed_hex << "\n";
    out << "mnemonic=abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about\n";
    out.close();
    return out.good();
}

static std::map<std::string, std::string> LoadWalletStateRows(const fs::path& wallet_state_db_path) {
    std::map<std::string, std::string> rows;
    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_state_db_path.string().c_str(), &db) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return rows;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key, value FROM wallet_state ORDER BY key ASC;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return rows;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (key && value) {
            rows[key] = value;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

static bool TestLegacyMigrationIdempotence() {
    std::cout << "\n[TEST] Legacy sidecar migration idempotence" << std::endl;

    const fs::path datadir = MakeTempDir();
    const std::string seed_hex = BuildDeterministicSeedHex();

    ASSERT_TRUE(WriteLegacyWalletConf(datadir, seed_hex), "failed to write legacy wallet.conf");

    auto first = HDWallet::MigrateLegacySidecarToStateDb(
        datadir.string(),
        dinero::consensus::DINERO_COIN_TYPE,
        /*create_backup=*/true,
        /*overwrite_existing=*/false);

    ASSERT_TRUE(first.success, "first migration must succeed");
    ASSERT_TRUE(first.migrated, "first migration must mark migrated=true");
    ASSERT_TRUE(!first.already_migrated, "first migration must not report already_migrated");
    ASSERT_TRUE(!first.wallet_state_path.empty(), "wallet_state_path must be populated");
    ASSERT_TRUE(fs::exists(first.wallet_state_path), "wallet_state.db must exist after migration");
    ASSERT_TRUE(!first.backup_path.empty(), "backup_path must be populated when backup enabled");
    ASSERT_TRUE(fs::exists(first.backup_path), "backup file must be created");

    const auto state_after_first = LoadWalletStateRows(first.wallet_state_path);
    ASSERT_TRUE(!state_after_first.empty(), "wallet_state rows must exist after first migration");
    auto coin_type_it = state_after_first.find("coin_type");
    ASSERT_TRUE(coin_type_it != state_after_first.end(), "coin_type key must exist");
    ASSERT_EQ(coin_type_it->second,
              std::to_string(dinero::consensus::DINERO_COIN_TYPE),
              "coin_type must match canonical DINERO_COIN_TYPE");

    auto next_index_it = state_after_first.find("next_index");
    ASSERT_TRUE(next_index_it != state_after_first.end(), "next_index key must exist");
    ASSERT_EQ(next_index_it->second, "7", "next_index must be migrated");

    auto next_change_index_it = state_after_first.find("next_change_index");
    ASSERT_TRUE(next_change_index_it != state_after_first.end(), "next_change_index key must exist");
    ASSERT_EQ(next_change_index_it->second, "3", "next_change_index must be migrated");

    auto next_taproot_index_it = state_after_first.find("next_taproot_index");
    ASSERT_TRUE(next_taproot_index_it != state_after_first.end(), "next_taproot_index key must exist");
    ASSERT_EQ(next_taproot_index_it->second, "11", "next_taproot_index must be migrated");
    ASSERT_TRUE(state_after_first.find("seed_hex") == state_after_first.end(),
                "seed_hex must not be persisted in wallet_state");
    ASSERT_TRUE(state_after_first.find("mnemonic") == state_after_first.end(),
                "mnemonic must not be persisted in wallet_state");
    ASSERT_TRUE(state_after_first.find("password_salt") != state_after_first.end(),
                "password_salt must be persisted");
    ASSERT_TRUE(state_after_first.find("password_hash") != state_after_first.end(),
                "password_hash must be persisted");
    ASSERT_TRUE(state_after_first.find("encrypted_seed") != state_after_first.end(),
                "encrypted_seed must be persisted");

    auto second = HDWallet::MigrateLegacySidecarToStateDb(
        datadir.string(),
        dinero::consensus::DINERO_COIN_TYPE,
        /*create_backup=*/true,
        /*overwrite_existing=*/false);

    ASSERT_TRUE(second.success, "second migration must succeed");
    ASSERT_TRUE(!second.migrated, "second migration must not rewrite migrated state");
    ASSERT_TRUE(second.already_migrated, "second migration must report already_migrated");

    const auto state_after_second = LoadWalletStateRows(first.wallet_state_path);
    ASSERT_EQ(state_after_second.size(),
              state_after_first.size(),
              "wallet_state key count must remain unchanged on second migration");
    ASSERT_TRUE(state_after_second == state_after_first,
                "wallet_state content must be identical after second migration");

    // Explicitly exercise no-sidecar path after successful migration.
    std::error_code ec;
    fs::remove(datadir / "wallet.conf", ec);
    ASSERT_TRUE(!fs::exists(datadir / "wallet.conf"), "wallet.conf should be removed for no-sidecar check");

    auto third = HDWallet::MigrateLegacySidecarToStateDb(
        datadir.string(),
        dinero::consensus::DINERO_COIN_TYPE,
        /*create_backup=*/true,
        /*overwrite_existing=*/false);

    ASSERT_TRUE(third.success, "third migration must succeed without sidecar when DB already populated");
    ASSERT_TRUE(third.already_migrated, "third migration must report already_migrated");
    ASSERT_TRUE(!third.migrated, "third migration must not mutate DB state");

    fs::remove_all(datadir, ec);
    std::cout << "  PASS: legacy migration is idempotent and DB-authoritative" << std::endl;
    return true;
}

int main() {
    std::srand(0xD1E3);

    if (!TestLegacyMigrationIdempotence()) {
        std::cerr << "\nFAILED: " << g_tests_passed << "/" << g_tests_run << " assertions passed\n";
        return 1;
    }

    std::cout << "\nPASS: " << g_tests_passed << "/" << g_tests_run << " assertions passed\n";
    return 0;
}
