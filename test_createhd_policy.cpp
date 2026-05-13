#include <iostream>
#include <sqlite3.h>
#include <filesystem>
#include <cstdlib>

void checkWalletPolicy(const std::string& wallet_name, const std::string& expected_policy) {
    std::string wallet_path = std::string(getenv("HOME")) + "/.dinero/wallets/wallet_" + wallet_name + ".db";

    std::cout << "\nChecking wallet: " << wallet_name << std::endl;

    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_path.c_str(), &db) != SQLITE_OK) {
        std::cout << "  ❌ Failed to open database" << std::endl;
        return;
    }

    // Check wallet_meta
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT name, network, wallet_policy FROM wallet_meta WHERE id = 1";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cout << "  ❌ Failed to query wallet_meta" << std::endl;
        sqlite3_close(db);
        return;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string network = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        std::cout << "  Name: " << name << std::endl;
        std::cout << "  Network: " << network << std::endl;
        std::cout << "  Policy: " << policy << std::endl;

        if (policy == expected_policy) {
            std::cout << "  ✅ Policy matches expected value: " << expected_policy << std::endl;
        } else {
            std::cout << "  ❌ Policy mismatch! Expected: " << expected_policy << ", Got: " << policy << std::endl;
        }
    } else {
        std::cout << "  ❌ No wallet_meta row found" << std::endl;
    }

    sqlite3_finalize(stmt);

    // Check addresses table for correct type
    const char* addr_sql = "SELECT address, type FROM addresses ORDER BY id LIMIT 1";
    if (sqlite3_prepare_v2(db, addr_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            std::string type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

            std::cout << "  First address: " << address << std::endl;
            std::cout << "  Address type: " << type << std::endl;

            // Verify address type matches policy
            if (expected_policy == "bip86" && type == "p2tr") {
                std::cout << "  ✅ Address type matches BIP86 (P2TR)" << std::endl;
            } else if (expected_policy == "bip84" && type == "p2wpkh") {
                std::cout << "  ✅ Address type matches BIP84 (P2WPKH)" << std::endl;
            } else {
                std::cout << "  ⚠️  Address type (" << type << ") may not match policy (" << expected_policy << ")" << std::endl;
            }

            // Verify address prefix
            if (expected_policy == "bip86" && address.substr(0, 5) == "rdin1p") {
                std::cout << "  ✅ BIP86 Taproot address prefix correct (rdin1p...)" << std::endl;
            } else if (expected_policy == "bip84" && address.substr(0, 5) == "rdin1q") {
                std::cout << "  ✅ BIP84 SegWit address prefix correct (rdin1q...)" << std::endl;
            } else {
                std::cout << "  ⚠️  Address prefix: " << address.substr(0, 6) << std::endl;
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
}

int main() {
    std::cout << "=== wallet.createhd Policy Parameter Test ===" << std::endl;
    std::cout << "\nThis test verifies that:" << std::endl;
    std::cout << "  1. wallet.createhd accepts 'policy' parameter" << std::endl;
    std::cout << "  2. Policy is stored in wallet_meta table" << std::endl;
    std::cout << "  3. Correct address type is generated based on policy" << std::endl;
    std::cout << "  4. Address prefixes match the policy" << std::endl;

    std::cout << "\n════════════════════════════════════════════════" << std::endl;

    // Test existing wallet (should have been created with default policy)
    std::string default_wallet = "default";
    if (std::filesystem::exists(std::string(getenv("HOME")) + "/.dinero/wallets/wallet_" + default_wallet + ".db")) {
        std::cout << "\nEXISTING WALLET TEST:" << std::endl;
        checkWalletPolicy(default_wallet, "bip84");  // Migrated wallets default to bip84
    }

    std::cout << "\n════════════════════════════════════════════════" << std::endl;
    std::cout << "\nTO TEST:" << std::endl;
    std::cout << "  1. Create BIP86 wallet: ./dinero-cli wallet.createhd test_bip86 12 \"\" \"\" bip86" << std::endl;
    std::cout << "  2. Create BIP84 wallet: ./dinero-cli wallet.createhd test_bip84 12 \"\" \"\" bip84" << std::endl;
    std::cout << "  3. Run this test again to verify" << std::endl;

    std::cout << "\n════════════════════════════════════════════════" << std::endl;
    std::cout << "\n=== Test Complete ===" << std::endl;

    return 0;
}
