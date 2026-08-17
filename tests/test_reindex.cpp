// Standalone reindex tester
// Tests BlockReindexer without full daemon dependencies

#include "consensus/reindexer.h"
#include "storage/chain_db.h"
#include "storage/block_storage.h"
#include "consensus/chainparams.h"
#include <iostream>
#include <filesystem>

using namespace dinero;
using namespace dinero::consensus;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <datadir> [--reindex-chainstate]" << std::endl;
        return 1;
    }

    std::filesystem::path datadir = argv[1];
    bool chainstate_only = (argc > 2 && std::string(argv[2]) == "--reindex-chainstate");

    std::cout << "===========================================\n";
    std::cout << "REINDEX TEST\n";
    std::cout << "===========================================\n";
    std::cout << "Datadir: " << datadir << "\n";
    std::cout << "Mode: " << (chainstate_only ? "chainstate-only" : "full") << "\n\n";

    // Select network (default regtest for testing)
    SelectParams(Chain::REGTEST);

    // Initialize ChainDB
    std::unique_ptr<ChainDB> chain_db = std::make_unique<ChainDB>();
    std::filesystem::path chain_db_path = datadir / "blockchain" / "chaindb";
    
    auto db_status = chain_db->init(chain_db_path);
    if (db_status != Status::Ok) {
        std::cerr << "Failed to init ChainDB: " << StatusToString(db_status) << std::endl;
        return 1;
    }

    // Initialize BlockStorage
    std::unique_ptr<BlockStorage> block_storage = std::make_unique<BlockStorage>();
    auto storage_status = block_storage->init(datadir);
    if (storage_status != Status::Ok) {
        std::cerr << "Failed to init BlockStorage: " << StatusToString(storage_status) << std::endl;
        return 1;
    }

    // Configure reindexer
    BlockReindexer::Config config;
    config.mode = chainstate_only ? BlockReindexer::Mode::CHAINSTATE_ONLY : BlockReindexer::Mode::FULL;
    config.use_assumevalid = true;
    config.progress_interval = 100;

    // Execute reindex
    BlockReindexer reindexer(datadir, chain_db.get(), block_storage.get(), config);
    auto result = reindexer.execute();

    if (!result.ok()) {
        std::cerr << "\n❌ REINDEX FAILED\n";
        std::cerr << "Status: " << StatusToString(result.status()) << std::endl;
        return 1;
    }

    const auto& stats = result.value();
    if (!stats.success) {
        std::cerr << "\n❌ REINDEX FAILED\n";
        std::cerr << "Error: " << stats.error << std::endl;
        return 1;
    }

    std::cout << "\n✅ REINDEX SUCCESS\n";
    std::cout << "Blocks: " << stats.blocks_processed << "\n";
    std::cout << "Files: " << stats.files_scanned << "\n";
    std::cout << "Duration: " << (stats.duration_ms / 1000.0) << "s\n";

    return 0;
}
