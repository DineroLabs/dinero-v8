#include "wallet/safety_profile.h"
#include <cstring>
#include <stdexcept>

namespace dinero {

// ============================================================================
// SafetyProfile Serialization
// ============================================================================

std::vector<uint8_t> SafetyProfile::Serialize() const {
    // panic_window(4 LE) || panic_chain(4 LE) || recovery_delay(4 LE) ||
    // recovery_chain(4 LE) || name_len(1) || name(N) || is_active(1)
    std::vector<uint8_t> out;
    out.reserve(17 + profile_name.size());

    auto push_u32 = [&](uint32_t v) {
        out.push_back(v & 0xff);
        out.push_back((v >> 8) & 0xff);
        out.push_back((v >> 16) & 0xff);
        out.push_back((v >> 24) & 0xff);
    };

    push_u32(panic_window_blocks);
    push_u32(panic_key_chain);
    push_u32(recovery_delay_blocks);
    push_u32(recovery_key_chain);

    uint8_t name_len = static_cast<uint8_t>(
        std::min(profile_name.size(), size_t(255)));
    out.push_back(name_len);
    out.insert(out.end(), profile_name.begin(),
        profile_name.begin() + name_len);

    out.push_back(is_active ? 1 : 0);
    return out;
}

SafetyProfile SafetyProfile::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 18) {
        throw std::runtime_error("SafetyProfile: data too short");
    }

    SafetyProfile p;
    size_t off = 0;

    auto read_u32 = [&]() -> uint32_t {
        uint32_t v = static_cast<uint32_t>(data[off])
            | (static_cast<uint32_t>(data[off + 1]) << 8)
            | (static_cast<uint32_t>(data[off + 2]) << 16)
            | (static_cast<uint32_t>(data[off + 3]) << 24);
        off += 4;
        return v;
    };

    p.panic_window_blocks = read_u32();
    p.panic_key_chain = read_u32();
    p.recovery_delay_blocks = read_u32();
    p.recovery_key_chain = read_u32();

    uint8_t name_len = data[off++];
    if (data.size() < off + name_len + 1) {
        throw std::runtime_error("SafetyProfile: name truncated");
    }
    p.profile_name.assign(data.begin() + off, data.begin() + off + name_len);
    off += name_len;

    p.is_active = (data[off] != 0);
    return p;
}

// ============================================================================
// SafetyProfileManager
// ============================================================================

bool SafetyProfileManager::CreateProfile(const SafetyProfile& profile) {
    // Check for duplicate name
    for (const auto& p : profiles_) {
        if (p.profile_name == profile.profile_name) {
            return false;
        }
    }
    profiles_.push_back(profile);
    return true;
}

std::optional<SafetyProfile> SafetyProfileManager::GetActiveProfile() const {
    for (const auto& p : profiles_) {
        if (p.is_active) {
            return p;
        }
    }
    return std::nullopt;
}

bool SafetyProfileManager::ActivateProfile(const std::string& name) {
    bool found = false;
    for (auto& p : profiles_) {
        if (p.profile_name == name) {
            p.is_active = true;
            found = true;
        } else {
            p.is_active = false;
        }
    }
    return found;
}

std::vector<SafetyProfile> SafetyProfileManager::ListProfiles() const {
    return profiles_;
}

} // namespace dinero
