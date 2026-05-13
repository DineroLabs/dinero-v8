#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dinero::nodecore {

struct SyncProfileResolution {
    bool ok{false};
    std::string profile;
    bool utreexo_stateless{false};
    std::string error;
};

std::string ToLowerAscii(std::string value);

bool DefaultUtreexoStateless();
std::string DefaultSyncProfile();
bool IsValidSyncProfile(const std::string& profile);
bool ProfileIsStateless(const std::string& profile);
uint64_t CapabilitiesForProfile(const std::string& profile);

// Resolves the runtime profile with precedence:
// 1) explicit profile (if provided and valid)
// 2) legacy utreexo_stateless (for backward compatibility)
// 3) platform default profile
SyncProfileResolution ResolveSyncProfile(
    const std::optional<std::string>& explicit_profile,
    const std::optional<bool>& legacy_utreexo_stateless
);

}  // namespace dinero::nodecore
