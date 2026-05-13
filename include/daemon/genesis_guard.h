#pragma once

#include <filesystem>
#include <string>

namespace dinero {

enum class GenesisGuardResult {
    OK,          // Stored genesis matches expected
    MISMATCH,    // Stored genesis does NOT match expected
    NO_DB,       // No database exists (fresh install)
    READ_ERROR   // Could not read database
};

// Check if stored genesis hash matches expected mainnet genesis.
// Must be called BEFORE full daemon init (before DaemonApp).
// datadir = e.g. ~/.dinero (the ChainDB lives at datadir/blockchain/chaindb)
GenesisGuardResult checkGenesisGuard(const std::filesystem::path& datadir);

// Wipe stale chain data, backing up wallets/ first.
// Returns true on success.
bool wipeStaleChainData(const std::filesystem::path& datadir);

} // namespace dinero
