#pragma once
#include "storage/shielded_migration.h"

namespace dinero::storage {

struct ShieldedCompanionLimits {
    uint64_t max_entries;
    uint64_t max_bytes;
    uint64_t max_metadata_rows;
    uint64_t max_metadata_value_bytes;
    uint64_t max_sqlite_steps;
    uint64_t max_ancestry_headers;
};

// Native, noninstalled qualification layer. Holds the daemon's existing
// dinerod.lock on both datadirs while the inner engine owns both ChainDB LOCKs.
// Binds unchanged blockchain companions, blocks, headers and checkpoints into
// the migration identity. Wallets, secrets, logs and runtime PID files are not
// copied, hashed or changed. Existing lock files are required, never created.
//
// Includes bounded read-only SQLite lifecycle/provenance checks and protected
// base ancestry checks. This is NOT a launch permit: network/profile, protected-
// base forest reconstruction, binary pairing and disk headroom still
// need independent qualification. READY describes only the relocated ChainDB.
ShieldedMigrationResult MigrateShieldedDatadirCopy(
    const std::filesystem::path& original,
    const std::filesystem::path& candidate,
    const ShieldedMigrationLimits& database_limits,
    const ShieldedCompanionLimits& companion_limits,
    bool apply,
    const std::function<void(const char*)>& checkpoint = {});

} // namespace dinero::storage
