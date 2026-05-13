#include "wallet/script_ownership.h"
#include "wallet/keystore.h"
#include <algorithm>

namespace dinero {
namespace wallet {

ScriptOwnershipResolver::ScriptOwnershipResolver(WalletKeyStore* keystore)
    : keystore_(keystore) {
}

ScriptOwnership ScriptOwnershipResolver::IsMine(
    const std::vector<uint8_t>& scriptPubKey) const {

    // Determine script type
    bool is_taproot = IsP2TR(scriptPubKey);

    // Extract all key IDs referenced by this script
    auto key_ids = ExtractKeyIDs(scriptPubKey);
    if (key_ids.empty()) {
        return ScriptOwnership::NO;  // Unrecognized script type
    }

    // Check if we have all required keys
    bool have_all = true;
    bool any_spendable = false;

    for (const auto& key_id : key_ids) {
        std::optional<WalletKey> key;

        // CRITICAL: For Taproot, key_id is actually output_key_id
        // We need to look it up via GetKeyByOutputKeyID, not GetKey
        if (is_taproot) {
            key = keystore_->GetKeyByOutputKeyID(key_id);
        } else {
            key = keystore_->GetKey(key_id);
        }

        if (!key.has_value()) {
            have_all = false;
            break;
        }

        // Check if this key is spendable
        if (key->spendable) {
            any_spendable = true;
        }
    }

    if (!have_all) {
        return ScriptOwnership::NO;  // Missing keys
    }

    // We have all keys - check if any are spendable
    return any_spendable ? ScriptOwnership::SPENDABLE
                         : ScriptOwnership::WATCH_ONLY;
}

std::vector<KeyID> ScriptOwnershipResolver::ExtractKeyIDs(
    const std::vector<uint8_t>& scriptPubKey) const {

    std::vector<KeyID> result;

    // Try P2WPKH
    if (IsP2WPKH(scriptPubKey)) {
        auto key_id = ExtractP2WPKHKeyID(scriptPubKey);
        if (key_id.has_value()) {
            result.push_back(key_id.value());
        }
        return result;
    }

    // Try P2TR
    if (IsP2TR(scriptPubKey)) {
        auto output_key_id = ExtractP2TRKeyID(scriptPubKey);
        if (output_key_id.has_value()) {
            result.push_back(output_key_id.value());
        }
        return result;
    }

    // Unrecognized script type
    return result;
}

bool ScriptOwnershipResolver::HaveKey(const KeyID& key_id) const {
    return keystore_->HaveKey(key_id);
}

std::optional<KeyOriginInfo> ScriptOwnershipResolver::GetKeyOrigin(
    const KeyID& key_id) const {

    auto key = keystore_->GetKey(key_id);
    if (!key.has_value()) {
        return std::nullopt;
    }

    return key->origin;
}

// ═══════════════════════════════════════════════════════════════
// Script Type Detection
// ═══════════════════════════════════════════════════════════════

bool ScriptOwnershipResolver::IsP2WPKH(const std::vector<uint8_t>& script) const {
    // P2WPKH: 0x00 0x14 <20-byte-hash>
    // Total: 22 bytes
    if (script.size() != 22) {
        return false;
    }
    if (script[0] != 0x00) {  // Witness version 0
        return false;
    }
    if (script[1] != 0x14) {  // Push 20 bytes
        return false;
    }
    return true;
}

bool ScriptOwnershipResolver::IsP2TR(const std::vector<uint8_t>& script) const {
    // P2TR: 0x51 0x20 <32-byte-pubkey>
    // Total: 34 bytes
    if (script.size() != 34) {
        return false;
    }
    if (script[0] != 0x51) {  // OP_1 (witness version 1)
        return false;
    }
    if (script[1] != 0x20) {  // Push 32 bytes
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Key Extraction
// ═══════════════════════════════════════════════════════════════

std::optional<KeyID> ScriptOwnershipResolver::ExtractP2WPKHKeyID(
    const std::vector<uint8_t>& script) const {

    if (!IsP2WPKH(script)) {
        return std::nullopt;
    }

    // Extract the 20-byte hash (which IS the KeyID)
    // P2WPKH: 0x00 0x14 <20-byte-KeyID>
    KeyID key_id;
    std::copy(script.begin() + 2, script.end(), key_id.begin());

    return key_id;
}

std::optional<KeyID> ScriptOwnershipResolver::ExtractP2TRKeyID(
    const std::vector<uint8_t>& script) const {

    if (!IsP2TR(script)) {
        return std::nullopt;
    }

    // Extract the 32-byte tweaked pubkey
    // P2TR: 0x51 0x20 <32-byte-tweaked-pubkey>
    std::array<uint8_t, 32> output_key;
    std::copy(script.begin() + 2, script.begin() + 34, output_key.begin());

    // CRITICAL: Compute output_key_id from tweaked key
    // This matches the output_key_id we stored during address generation
    KeyID output_key_id = ComputeKeyIDFromXOnly(output_key);

    return output_key_id;
}

} // namespace wallet
} // namespace dinero
