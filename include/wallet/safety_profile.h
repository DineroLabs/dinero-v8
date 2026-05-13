#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinero {

/**
 * Safety Profile — configures panic and recovery parameters
 * for PROTECTED template outputs.
 *
 * Panic: fast-spending emergency path (e.g., 6 blocks ~1h)
 *   Key derivation: m/86'/1448'/0'/100'/index
 *
 * Recovery: slow-spending disaster recovery path (e.g., 25920 blocks ~6mo)
 *   Key derivation: m/86'/1448'/0'/101'/index
 */
struct SafetyProfile {
    uint32_t panic_window_blocks = 6;          // CSV for panic leaf (~1h)
    uint32_t panic_key_chain = 100;            // Hardened BIP32 chain for panic keys
    uint32_t recovery_delay_blocks = 25920;    // CSV for recovery leaf (~6mo)
    uint32_t recovery_key_chain = 101;         // Hardened BIP32 chain for recovery keys
    std::string profile_name;
    bool is_active = false;

    /// Serialize for DB storage
    std::vector<uint8_t> Serialize() const;
    static SafetyProfile Deserialize(const std::vector<uint8_t>& data);
};

/**
 * Manages safety profiles — one active profile at a time.
 * Profiles are stored in the wallet_policies DB table (template_type=PROTECTED).
 */
class SafetyProfileManager {
public:
    bool CreateProfile(const SafetyProfile& profile);
    std::optional<SafetyProfile> GetActiveProfile() const;
    bool ActivateProfile(const std::string& name);
    std::vector<SafetyProfile> ListProfiles() const;

private:
    std::vector<SafetyProfile> profiles_;
};

} // namespace dinero
