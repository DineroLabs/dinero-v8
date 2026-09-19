#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace dinero::storage {

// Offline ChainDB-copy engine, not whole-datadir recovery or a launch permit.
// Its caller must qualify the complete stopped cohort and binary/datadir pairing
// before selecting the result. Neither daemon startup nor NodeCore invokes this.
struct ShieldedMigrationLimits {
    uint64_t batch_bytes;
    uint32_t batch_rows;
    uint64_t max_record_bytes;
    uint64_t max_nullifiers;
};

struct ShieldedMigrationResult {
    bool ok = false;
    bool ready = false;
    uint64_t selected_rows = 0;
    uint64_t retired_rows = 0;
    std::string phase;
    std::string operation;
    std::string source_digest;
    std::string error;
};

// Paths are existing, disjoint ChainDB directories, never production paths
// chosen implicitly. The original is opened read-only under its real LOCK.
// The candidate stays locked across inspection, open handoff and publication.
// apply=false inspects without changing any logical record. Resource limits are
// explicit; no unmeasured device budget is silently selected by this engine.
ShieldedMigrationResult MigrateShieldedStateCopy(
    const std::filesystem::path& original,
    const std::filesystem::path& candidate,
    const ShieldedMigrationLimits& limits,
    bool apply,
    const std::function<void(const char*)>& checkpoint = {});

} // namespace dinero::storage
