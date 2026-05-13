#include "daemon/genesis_guard.h"
#include "consensus/chain_identity.h"
#include "storage/chain_db.h"
#include "primitives/uint256.h"
#include <iostream>
#include <chrono>
#include <ctime>

namespace dinero {

// Expected mainnet genesis hash (display format)
static constexpr const char* EXPECTED_GENESIS_HASH =
    dinero::consensus::kMainnetGenesisHash.data();

GenesisGuardResult checkGenesisGuard(const std::filesystem::path& datadir) {
    std::filesystem::path chaindb_path = datadir / "blockchain" / "chaindb";

    // If the chaindb directory doesn't exist, this is a fresh install
    if (!std::filesystem::exists(chaindb_path)) {
        return GenesisGuardResult::NO_DB;
    }

    // Open ChainDB to read the stored genesis hash
    ChainDB db;
    auto status = db.init(chaindb_path);
    if (status != Status::Ok) {
        std::cerr << "[GenesisGuard] Could not open ChainDB: "
                  << StatusToString(status) << "\n";
        return GenesisGuardResult::READ_ERROR;
    }

    // Read the block hash at height 0
    auto result = db.getBlockHashByHeight(0);
    db.close();

    if (!result.ok()) {
        // DB exists but no height 0 — possibly empty or corrupted
        // Treat as no DB (will be populated on first sync)
        return GenesisGuardResult::NO_DB;
    }

    const uint256 stored_hash = result.value();
    const uint256 expected_hash = uint256::FromHexUnsafe(EXPECTED_GENESIS_HASH);

    if (stored_hash != expected_hash) {
        std::cerr << "GENESIS_MISMATCH\n";
        std::cerr << "[GenesisGuard] Stored genesis hash does not match expected.\n";
        std::cerr << "  Stored:   " << stored_hash.GetHex() << "\n";
        std::cerr << "  Expected: " << expected_hash.GetHex() << "\n";
        std::cerr << "\n";
        std::cerr << "Your chain data is from an older or incompatible version.\n";
        std::cerr << "Run with --wipe-stale-chain to back up your wallet and reset chain data.\n";
        return GenesisGuardResult::MISMATCH;
    }

    return GenesisGuardResult::OK;
}

bool wipeStaleChainData(const std::filesystem::path& datadir) {
    // Back up wallets/ directory first
    std::filesystem::path wallets_dir = datadir / "wallets";
    if (std::filesystem::exists(wallets_dir)) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S",
                      std::localtime(&time_t_now));

        std::filesystem::path backup_dir =
            datadir / ("wallets-backup-" + std::string(timestamp));

        std::error_code ec;
        std::filesystem::rename(wallets_dir, backup_dir, ec);
        if (ec) {
            // rename failed (cross-device?), try copy
            std::filesystem::copy(wallets_dir, backup_dir,
                std::filesystem::copy_options::recursive, ec);
            if (ec) {
                std::cerr << "[GenesisGuard] Failed to back up wallets: "
                          << ec.message() << "\n";
                return false;
            }
            std::filesystem::remove_all(wallets_dir, ec);
        }

        std::cout << "[GenesisGuard] Wallet backed up to: " << backup_dir << "\n";
    }

    // Chain data directories/files to remove
    const std::vector<std::string> chain_data = {
        "blockchain", "blocks", "headers", "utreexo",
        "blockchaindb", "mempool.db", "peers.db", "blocks.db"
    };

    std::error_code ec;
    for (const auto& name : chain_data) {
        std::filesystem::path target = datadir / name;
        if (std::filesystem::exists(target, ec)) {
            std::filesystem::remove_all(target, ec);
            if (ec) {
                std::cerr << "[GenesisGuard] Warning: could not remove "
                          << target << ": " << ec.message() << "\n";
            } else {
                std::cout << "[GenesisGuard] Removed: " << target << "\n";
            }
        }
    }

    // Restore wallet backup to wallets/
    // Find the backup we just created
    for (auto& entry : std::filesystem::directory_iterator(datadir, ec)) {
        if (entry.path().filename().string().find("wallets-backup-") == 0) {
            std::filesystem::rename(entry.path(), wallets_dir, ec);
            if (!ec) {
                std::cout << "[GenesisGuard] Wallet restored from backup.\n";
            }
            break;
        }
    }

    std::cout << "[GenesisGuard] Stale chain data wiped. Ready for fresh sync.\n";
    return true;
}

} // namespace dinero
