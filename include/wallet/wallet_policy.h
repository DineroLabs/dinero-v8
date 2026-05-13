#pragma once
#include <string>

namespace dinero {

/**
 * @brief Wallet derivation policy
 *
 * Determines which BIP standard and address type the wallet uses.
 * This is a PERMANENT property set at wallet creation - wallets
 * are NEVER automatically migrated between policies.
 */
enum class WalletPolicy {
    /**
     * BIP84 - Native SegWit (P2WPKH)
     * - Derivation: m/84'/1448'/0'
     * - Descriptor: wpkh([fingerprint/84h/1448h/0h]xpub/0/\*)
     * - Address format: bech32 (din1q...)
     * - Maximum compatibility with exchanges and tools
     */
    BIP84_LEGACY = 0,

    /**
     * BIP86 - Taproot Single-Key (P2TR key-path only)
     * - Derivation: m/86'/1448'/0'
     * - Descriptor: tr([fingerprint/86h/1448h/0h]xpub/0/\*)
     * - Address format: bech32m (din1p...)
     * - Modern default, maximum privacy
     * - Key-path spending only (no script-path by default)
     */
    BIP86_TAPROOT = 1
};

/**
 * @brief Convert WalletPolicy to string for storage
 */
inline std::string WalletPolicyToString(WalletPolicy policy) {
    switch (policy) {
        case WalletPolicy::BIP84_LEGACY:
            return "bip84";
        case WalletPolicy::BIP86_TAPROOT:
            return "bip86";
        default:
            return "unknown";
    }
}

/**
 * @brief Parse WalletPolicy from string
 */
inline WalletPolicy StringToWalletPolicy(const std::string& str) {
    if (str == "bip84") return WalletPolicy::BIP84_LEGACY;
    if (str == "bip86") return WalletPolicy::BIP86_TAPROOT;
    // Default to BIP84 for unknown/missing values (safety)
    return WalletPolicy::BIP84_LEGACY;
}

/**
 * @brief Get human-readable description of policy
 */
inline std::string WalletPolicyDescription(WalletPolicy policy) {
    switch (policy) {
        case WalletPolicy::BIP84_LEGACY:
            return "BIP84 Native SegWit (P2WPKH, maximum compatibility)";
        case WalletPolicy::BIP86_TAPROOT:
            return "BIP86 Taproot (P2TR key-path, maximum privacy)";
        default:
            return "Unknown policy";
    }
}

} // namespace dinero
