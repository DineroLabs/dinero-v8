/**
 * @file test_wallet_descriptor_active_context.cpp
 * @brief Verifies descriptor RPC reads from active wallet DB context only.
 */

#include "wallet/wallet_manager.h"
#include "core/rpc/wallet_descriptor_rpc_handlers.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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
                   ("dinero_wallet_descriptor_ctx_" + std::to_string(nonce));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static std::string CurrentDbPath(sqlite3* db) {
    if (!db) {
        return {};
    }

    sqlite3_stmt* stmt = nullptr;
    std::string db_path;
    if (sqlite3_prepare_v2(db, "PRAGMA database_list;", -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* file = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (name && std::string(name) == "main" && file) {
            db_path = file;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return db_path;
}

static std::string ExtractReceiveDescriptor(const Json::Value& response) {
    if (!response.isMember("descriptors") || !response["descriptors"].isArray()) {
        return {};
    }

    for (const auto& desc : response["descriptors"]) {
        if (desc.isMember("internal") && desc["internal"].isBool() && !desc["internal"].asBool() &&
            desc.isMember("desc") && desc["desc"].isString()) {
            return desc["desc"].asString();
        }
    }

    return {};
}

static bool TestDescriptorUsesActiveWalletContext() {
    std::cout << "\n[TEST] Descriptor RPC active-wallet DB context" << std::endl;

    const fs::path datadir = MakeTempDir();
    dinero::WalletManager wallet_manager(datadir);

    wallet_manager.create("alpha");
    wallet_manager.open("alpha");

    Json::Value params(Json::objectValue);
    Json::Value alpha = dinero::rpc_wallet_listdescriptors(params, &wallet_manager);
    ASSERT_TRUE(!alpha.isMember("error"), "alpha wallet descriptor listing must succeed");
    ASSERT_EQ(alpha["wallet_name"].asString(), "alpha", "alpha listing must report wallet_name=alpha");

    const std::string alpha_receive_desc = ExtractReceiveDescriptor(alpha);
    ASSERT_TRUE(!alpha_receive_desc.empty(), "alpha receive descriptor must exist");
    // Canonical coin type is 1448 (v7+, per derivation paths used everywhere
    // in src/wallet/* and src/daemon/*; the 1447 legacy scan path was
    // removed entirely on 2026-04-18). This test originally pinned 1447h and
    // was quarantined when that became wrong; the impl was always correct.
    ASSERT_TRUE(alpha_receive_desc.find("1448h") != std::string::npos,
                "alpha descriptor must encode canonical coin type 1448h");

    const std::string alpha_db_path = CurrentDbPath(wallet_manager.getCurrentDatabase());
    ASSERT_TRUE(!alpha_db_path.empty(), "alpha active DB path must resolve");

    wallet_manager.create("beta");
    wallet_manager.open("beta");

    Json::Value beta = dinero::rpc_wallet_listdescriptors(params, &wallet_manager);
    ASSERT_TRUE(!beta.isMember("error"), "beta wallet descriptor listing must succeed");
    ASSERT_EQ(beta["wallet_name"].asString(), "beta", "beta listing must report wallet_name=beta");

    const std::string beta_receive_desc = ExtractReceiveDescriptor(beta);
    ASSERT_TRUE(!beta_receive_desc.empty(), "beta receive descriptor must exist");
    ASSERT_TRUE(beta_receive_desc.find("1448h") != std::string::npos,
                "beta descriptor must encode canonical coin type 1448h");

    const std::string beta_db_path = CurrentDbPath(wallet_manager.getCurrentDatabase());
    ASSERT_TRUE(!beta_db_path.empty(), "beta active DB path must resolve");

    ASSERT_TRUE(alpha_db_path != beta_db_path, "active DB file must switch between alpha and beta");
    ASSERT_TRUE(alpha_receive_desc != beta_receive_desc,
                "receive descriptor must be wallet-specific (no cross-wallet leakage)");

    wallet_manager.open("alpha");
    Json::Value alpha_reopen = dinero::rpc_wallet_listdescriptors(params, &wallet_manager);
    ASSERT_TRUE(!alpha_reopen.isMember("error"), "alpha reopen descriptor listing must succeed");
    ASSERT_EQ(alpha_reopen["wallet_name"].asString(),
              "alpha",
              "reopened alpha listing must report wallet_name=alpha");

    const std::string alpha_reopen_desc = ExtractReceiveDescriptor(alpha_reopen);
    ASSERT_EQ(alpha_reopen_desc,
              alpha_receive_desc,
              "reopened alpha descriptor must match original alpha descriptor");

    const std::string alpha_reopen_db = CurrentDbPath(wallet_manager.getCurrentDatabase());
    ASSERT_EQ(alpha_reopen_db, alpha_db_path, "reopened alpha DB path must match original alpha DB path");

    std::error_code ec;
    fs::remove_all(datadir, ec);

    std::cout << "  PASS: descriptor RPC is bound to active wallet DB context" << std::endl;
    return true;
}

int main() {
    std::srand(0xC0FFEE);

    if (!TestDescriptorUsesActiveWalletContext()) {
        std::cerr << "\nFAILED: " << g_tests_passed << "/" << g_tests_run << " assertions passed\n";
        return 1;
    }

    std::cout << "\nPASS: " << g_tests_passed << "/" << g_tests_run << " assertions passed\n";
    return 0;
}
