#include "nodecore/sync_profile_policy.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "nodecore/nodecore_ffi.h"

#include <algorithm>
#include <cctype>

namespace dinero::nodecore {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool DefaultUtreexoStateless() {
#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return true;
#else
    return false;
#endif
}

std::string DefaultSyncProfile() {
#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return "ios_utreexo";
#else
    return "mac_fullblock";
#endif
}

bool IsValidSyncProfile(const std::string& profile) {
    return profile == "ios_utreexo" || profile == "mac_fullblock";
}

bool ProfileIsStateless(const std::string& profile) {
    return profile == "ios_utreexo";
}

bool ProfileRetainsHistoricalBodies(const std::string& profile) {
    return profile == "mac_fullblock";
}

uint64_t CapabilitiesForProfile(const std::string& profile) {
    if (profile == "ios_utreexo") {
        return NODECORE_CAP_SYNC_STATELESS;
    }
    if (profile == "mac_fullblock") {
        return NODECORE_CAP_SYNC_FULLBLOCK | NODECORE_CAP_MINING_LOCAL | NODECORE_CAP_MINING_POOL;
    }
    return 0;
}

SyncProfileResolution ResolveSyncProfile(
    const std::optional<std::string>& explicit_profile,
    const std::optional<bool>& legacy_utreexo_stateless
) {
    SyncProfileResolution out;

    std::string profile = DefaultSyncProfile();
    bool utreexo_stateless = DefaultUtreexoStateless();

    if (explicit_profile.has_value()) {
        profile = ToLowerAscii(*explicit_profile);
        if (!IsValidSyncProfile(profile)) {
            out.ok = false;
            out.error = "Invalid sync_profile (expected ios_utreexo or mac_fullblock)";
            return out;
        }
        // Explicit profile is authoritative.
        utreexo_stateless = ProfileIsStateless(profile);
    } else if (legacy_utreexo_stateless.has_value()) {
        // Backward-compatible fallback path.
        utreexo_stateless = *legacy_utreexo_stateless;
        profile = utreexo_stateless ? "ios_utreexo" : "mac_fullblock";
    }

    out.ok = true;
    out.profile = profile;
    out.utreexo_stateless = utreexo_stateless;
    return out;
}

}  // namespace dinero::nodecore
