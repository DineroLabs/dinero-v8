#pragma once
#include "storage/shielded_migration.h"
namespace dinero::storage::detail {
// Only the companion-inventory wrapper derives this binding; no RPC/CLI takes
// a caller-asserted digest or an ownership boolean as a substitute for a lease.
ShieldedMigrationResult MigrateBoundCopy(
    const std::filesystem::path&, const std::filesystem::path&,
    const ShieldedMigrationLimits&, bool, const std::string&,
    const std::function<void()>&, const std::function<void(const char*)>&);
}
