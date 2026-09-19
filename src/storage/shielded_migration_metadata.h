#pragma once
#include "storage/shielded_migration_cohort.h"
#include <optional>

namespace dinero::storage::detail {
struct ProtectedMigrationBase {
    uint32_t height;
    std::string hash;
};
struct ExternalMigrationState {
    std::optional<ProtectedMigrationBase> promoted_base;
    std::optional<uint32_t> wallet_base_height;
    uint64_t max_ancestry_headers;
    ShieldedCompanionLimits limits;
};
ExternalMigrationState InspectMigrationMetadata(
    const std::filesystem::path& datadir, const ShieldedCompanionLimits& limits);
}
