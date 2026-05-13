#include "wallet/wallet_manager.h"
#include <iostream>
#include <sqlite3.h>

int main() {
    std::cout << "=== Wallet Policy Migration Test ===" << std::endl << std::endl;

    // Test 1: Check existing wallet before migration
    std::cout << "1. Checking wallet schema BEFORE migration:" << std::endl;
    sqlite3* db = nullptr;
    std::string wallet_path = std::string(getenv("HOME")) + "/.dinero/wallets/wallet_default.db";

    if (sqlite3_open(wallet_path.c_str(), &db) == SQLITE_OK) {
        // Check PRAGMA user_version
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int version = sqlite3_column_int(stmt, 0);
            std::cout << "   Schema version: " << version << std::endl;
        }
        sqlite3_finalize(stmt);

        // Check if wallet_policy column exists
        sqlite3_prepare_v2(db, "PRAGMA table_info(wallet_meta)", -1, &stmt, nullptr);
        bool has_wallet_policy = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (col_name == "wallet_policy") {
                has_wallet_policy = true;
            }
        }
        sqlite3_finalize(stmt);
        std::cout << "   Has wallet_policy column: " << (has_wallet_policy ? "YES" : "NO") << std::endl;
        sqlite3_close(db);
    }
    std::cout << std::endl;

    // Test 2: Trigger migration by opening wallet through WalletManager
    std::cout << "2. Opening wallet through WalletManager (triggers migration):" << std::endl;
    try {
        std::string data_dir = std::string(getenv("HOME")) + "/.dinero";
        dinero::WalletManager wallet_manager(data_dir);
        wallet_manager.open("default");
        std::cout << "   ✅ Wallet opened successfully" << std::endl;
        // Note: close() is called automatically in destructor
    } catch (const std::exception& e) {
        std::cout << "   ❌ Error: " << e.what() << std::endl;
        return 1;
    }
    std::cout << std::endl;

    // Test 3: Verify migration succeeded
    std::cout << "3. Verifying migration AFTER opening:" << std::endl;
    if (sqlite3_open(wallet_path.c_str(), &db) == SQLITE_OK) {
        // Check PRAGMA user_version
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int version = sqlite3_column_int(stmt, 0);
            std::cout << "   Schema version: " << version << std::endl;
            if (version >= 16) {
                std::cout << "   ✅ Migration to v16 successful" << std::endl;
            } else {
                std::cout << "   ❌ Migration failed - still at v" << version << std::endl;
            }
        }
        sqlite3_finalize(stmt);

        // Check wallet_meta table
        sqlite3_prepare_v2(db, "SELECT id, name, network, wallet_policy FROM wallet_meta", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            std::string network = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            std::string policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

            std::cout << "   Wallet metadata:" << std::endl;
            std::cout << "     - ID: " << id << std::endl;
            std::cout << "     - Name: " << name << std::endl;
            std::cout << "     - Network: " << network << std::endl;
            std::cout << "     - Policy: " << policy << std::endl;

            if (policy == "bip84") {
                std::cout << "   ✅ Policy correctly set to 'bip84' for existing wallet" << std::endl;
            } else {
                std::cout << "   ⚠️  Policy is '" << policy << "' (expected 'bip84' for migrated wallet)" << std::endl;
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    std::cout << std::endl;

    std::cout << "=== Migration Test Complete ===" << std::endl;
    return 0;
}
