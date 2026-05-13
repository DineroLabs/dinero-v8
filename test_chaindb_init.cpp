// Minimal test to diagnose ChainDB initialization
#include "storage/chain_db.h"
#include <iostream>
#include <filesystem>

int main() {
    std::string db_path = "/tmp/test_chaindb_diag";

    // Clean up any existing database
    std::filesystem::remove_all(db_path);
    std::filesystem::create_directories(db_path);

    std::cout << "Initializing ChainDB at: " << db_path << std::endl;

    dinero::ChainDB db;
    auto status = db.init(db_path);

    std::cout << "ChainDB init status: " << (status == dinero::Status::OK ? "OK" : "FAILED") << std::endl;

    if (status == dinero::Status::OK) {
        std::cout << "Database initialized successfully" << std::endl;
        std::cout << "Testing forEachUTXO..." << std::endl;

        auto iter_status = db.forEachUTXO([](const std::string& key, const std::string& val) -> bool {
            std::cout << "UTXO: " << key << std::endl;
            return true;
        });

        std::cout << "forEachUTXO status: " << (iter_status == dinero::Status::OK ? "OK" : "FAILED") << std::endl;
    }

    return 0;
}
