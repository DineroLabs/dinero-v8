#pragma once

#include "wallet/wallet_policy.h"
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

/**
 * @brief Descriptor signing capability classification
 *
 * Phase 2 introduces three classes of descriptors:
 * - none: Watch-only, no signing capability
 * - internal: Hot wallet, signing keys in Dinero
 * - external: Hardware wallet, signing via external device
 */
enum class SigningCapability {
    None,      // Watch-only descriptor (active=false)
    Internal,  // Hot wallet (xprv present, fingerprint must match)
    External   // Hardware wallet (xpub only, external signing)
};

/**
 * @brief Result of descriptor activation validation
 */
struct ActivationValidationResult {
    bool valid;
    std::string error_message;
    SigningCapability signing_capability;
};

/**
 * @brief Descriptor Activation Gate (Phase 2 - Critical Security Component)
 *
 * All active=true descriptors pass through this gate before activation.
 * Enforces wallet policy compliance and prevents policy violations.
 *
 * Security Invariant: Descriptors inherit wallet policy, never define it.
 *
 * Validation Rules:
 * 1. Descriptor type must match wallet policy (tr ↔ BIP86, wpkh ↔ BIP84)
 * 2. Derivation path must match policy (86h for BIP86, 84h for BIP84)
 * 3. Fingerprint validation (internal: must match, external: must exist)
 * 4. Key material check (internal: xprv required, external: xpub only)
 *
 * Failure at any step → hard rejection
 */
class DescriptorActivationValidator {
public:
    /**
     * @brief Validate descriptor activation request
     *
     * @param descriptor_type "wpkh" or "tr"
     * @param wallet_policy Wallet's policy (BIP84, BIP86, WatchOnly)
     * @param active Whether descriptor is being activated
     * @param fingerprint Descriptor's key origin fingerprint
     * @param wallet_fingerprint Wallet's master key fingerprint
     * @param derivation_path Descriptor's derivation path (e.g., [86h/1448h/0h])
     * @param has_private_key Whether xprv material is present
     * @return ActivationValidationResult Validation result
     */
    static ActivationValidationResult ValidateActivation(
        const std::string& descriptor_type,
        WalletPolicy wallet_policy,
        bool active,
        const std::string& fingerprint,
        const std::string& wallet_fingerprint,
        const std::vector<uint32_t>& derivation_path,
        bool has_private_key
    );

    /**
     * @brief Determine signing capability from descriptor properties
     *
     * @param active Whether descriptor is active
     * @param has_private_key Whether xprv material is present
     * @param fingerprint_matches Whether fingerprint matches wallet
     * @return SigningCapability Determined capability
     */
    static SigningCapability DetermineSigningCapability(
        bool active,
        bool has_private_key,
        bool fingerprint_matches
    );

    /**
     * @brief Validate descriptor type matches wallet policy
     *
     * @param descriptor_type "wpkh" or "tr"
     * @param wallet_policy Wallet's policy
     * @return true if valid, false otherwise
     */
    static bool ValidateDescriptorType(
        const std::string& descriptor_type,
        WalletPolicy wallet_policy
    );

    /**
     * @brief Validate derivation path matches wallet policy
     *
     * @param derivation_path Path components (hardened indices)
     * @param wallet_policy Wallet's policy
     * @return true if valid, false otherwise
     */
    static bool ValidateDerivationPath(
        const std::vector<uint32_t>& derivation_path,
        WalletPolicy wallet_policy
    );

    /**
     * @brief Convert wallet policy string to enum
     *
     * @param policy_str "bip84", "bip86", or "watch-only"
     * @return WalletPolicy enum value
     */
    static WalletPolicy ParseWalletPolicy(const std::string& policy_str);

    /**
     * @brief Convert signing capability to string
     *
     * @param capability SigningCapability enum
     * @return std::string "none", "internal", or "external"
     */
    static std::string SigningCapabilityToString(SigningCapability capability);
};

} // namespace dinero
