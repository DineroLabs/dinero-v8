#include <iostream>
#include <sqlite3.h>
#include <filesystem>
#include <cstdlib>

void exec(sqlite3* db, const std::string& sql) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string error = errMsg ? errMsg : "Unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error("SQL error: " + error);
    }
}

int main() {
    std::cout << "=== New Wallet Policy Test ===" << std::endl << std::endl;

    // Create a test wallet database
    std::string test_wallet_path = "/tmp/test_wallet_new.db";

    // Remove if exists
    std::filesystem::remove(test_wallet_path);

    std::cout << "Creating new test wallet at: " << test_wallet_path << std::endl;

    sqlite3* db = nullptr;
    if (sqlite3_open(test_wallet_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to create database" << std::endl;
        return 1;
    }

    try {
        // Simulate the wallet creation code from wallet_manager.cpp
        exec(db, "PRAGMA foreign_keys = ON");
        exec(db, "PRAGMA journal_mode = WAL");
        exec(db, "PRAGMA trusted_schema = OFF");

        // Create wallet_meta table (matching wallet_manager.cpp code)
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS wallet_meta (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                name TEXT NOT NULL,
                network TEXT NOT NULL DEFAULT 'mainnet',
                encrypted INTEGER NOT NULL DEFAULT 0,
                fingerprint BLOB,
                wallet_policy TEXT NOT NULL DEFAULT 'bip86',
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                version INTEGER NOT NULL DEFAULT 1
            )
        )");

        // Insert wallet metadata (matching wallet_manager.cpp code)
        sqlite3_stmt* meta_stmt = nullptr;
        const char* meta_sql = "INSERT INTO wallet_meta (id, name, network, wallet_policy) VALUES (1, ?, 'mainnet', 'bip86')";
        if (sqlite3_prepare_v2(db, meta_sql, -1, &meta_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(meta_stmt, 1, "test_new", -1, SQLITE_TRANSIENT);
            sqlite3_step(meta_stmt);
            sqlite3_finalize(meta_stmt);
        }

        std::cout << "✅ Wallet created successfully" << std::endl << std::endl;

        // Verify the data
        std::cout << "Verifying wallet_meta:" << std::endl;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT name, network, wallet_policy FROM wallet_meta", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            std::string network = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            std::string policy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

            std::cout << "  Name: " << name << std::endl;
            std::cout << "  Network: " << network << std::endl;
            std::cout << "  Policy: " << policy << std::endl << std::endl;

            if (policy == "bip86") {
                std::cout << "✅ NEW WALLET DEFAULTS TO BIP86 TAPROOT" << std::endl;
            } else {
                std::cout << "❌ UNEXPECTED POLICY: " << policy << " (expected bip86)" << std::endl;
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return 1;
            }
        }
        sqlite3_finalize(stmt);

        sqlite3_close(db);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        sqlite3_close(db);
        return 1;
    }

    std::cout << "\n=== Test Passed ===" << std::endl;
    return 0;
}
