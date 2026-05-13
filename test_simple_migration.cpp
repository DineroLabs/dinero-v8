#include <iostream>
#include <sqlite3.h>
#include <cstdlib>

int main() {
    std::cout << "=== Simple Wallet Policy Column Test ===" << std::endl << std::endl;

    std::string wallet_path = std::string(getenv("HOME")) + "/.dinero/wallets/wallet_default.db";
    sqlite3* db = nullptr;

    // Test: Check if wallet_policy column exists and its value
    std::cout << "Checking wallet_default.db:" << std::endl;
    if (sqlite3_open(wallet_path.c_str(), &db) == SQLITE_OK) {
        // Check schema version
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int version = sqlite3_column_int(stmt, 0);
            std::cout << "  Schema version: " << version << std::endl;
        }
        sqlite3_finalize(stmt);

        // List all columns in wallet_meta
        std::cout << "\n  Columns in wallet_meta table:" << std::endl;
        sqlite3_prepare_v2(db, "PRAGMA table_info(wallet_meta)", -1, &stmt, nullptr);
        bool has_wallet_policy = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            std::string col_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            int not_null = sqlite3_column_int(stmt, 3);
            const unsigned char* dflt = sqlite3_column_text(stmt, 4);

            std::cout << "    - " << col_name << " (" << col_type << ")";
            if (not_null) std::cout << " NOT NULL";
            if (dflt) std::cout << " DEFAULT '" << dflt << "'";
            std::cout << std::endl;

            if (col_name == "wallet_policy") {
                has_wallet_policy = true;
            }
        }
        sqlite3_finalize(stmt);

        std::cout << "\n  Has wallet_policy column: " << (has_wallet_policy ? "✅ YES" : "❌ NO") << std::endl;

        // Show wallet_meta data
        if (has_wallet_policy) {
            std::cout << "\n  Wallet metadata:" << std::endl;
            sqlite3_prepare_v2(db, "SELECT name, network, wallet_policy FROM wallet_meta", -1, &stmt, nullptr);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string network = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                std::string policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

                std::cout << "    Name: " << name << std::endl;
                std::cout << "    Network: " << network << std::endl;
                std::cout << "    Policy: " << policy << std::endl;
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_close(db);
    } else {
        std::cout << "  ❌ Failed to open database" << std::endl;
        return 1;
    }

    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}
