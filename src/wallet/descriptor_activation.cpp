// SPDX-License-Identifier: MIT
// Dinero - Descriptor Activation Gate (Phase 2 - Security Critical)

#include "wallet/descriptor_activation.h"
#include <sstream>
#include <algorithm>

namespace dinero {

WalletPolicy DescriptorActivationValidator::ParseWalletPolicy(const std::string& policy_str) {
    // Use the existing StringToWalletPolicy from wallet_policy.h
    return StringToWalletPolicy(policy_str);
}

std::string DescriptorActivationValidator::SigningCapabilityToString(SigningCapability capability) {
    switch (capability) {
        case SigningCapability::None: return "none";
        case SigningCapability::Internal: return "internal";
        case SigningCapability::External: return "external";
    }
    return "none";
}

bool DescriptorActivationValidator::ValidateDescriptorType(
    const std::string& descriptor_type,
    WalletPolicy wallet_policy
) {
    // Wallet Policy Enforcement Matrix:
    // BIP86_TAPROOT   → tr() only
    // BIP84_LEGACY    → wpkh() only

    switch (wallet_policy) {
        case WalletPolicy::BIP86_TAPROOT:
            return descriptor_type == "tr";
        case WalletPolicy::BIP84_LEGACY:
            return descriptor_type == "wpkh";
    }
    return false;
}

bool DescriptorActivationValidator::ValidateDerivationPath(
    const std::vector<uint32_t>& derivation_path,
    WalletPolicy wallet_policy
) {
    // Derivation paths must match policy:
    // BIP86_TAPROOT: m/86h/...
    // BIP84_LEGACY: m/84h/...

    if (derivation_path.empty()) {
        return false; // Empty path is invalid
    }

    uint32_t purpose = derivation_path[0];
    uint32_t expected_purpose = 0;

    switch (wallet_policy) {
        case WalletPolicy::BIP86_TAPROOT:
            expected_purpose = 86 | 0x80000000; // 86h
            break;
        case WalletPolicy::BIP84_LEGACY:
            expected_purpose = 84 | 0x80000000; // 84h
            break;
    }

    return purpose == expected_purpose;
}

SigningCapability DescriptorActivationValidator::DetermineSigningCapability(
    bool active,
    bool has_private_key,
    bool fingerprint_matches
) {
    // Signing capability decision tree:
    // - active=false → none (watch-only)
    // - active=true + xprv + fingerprint_match → internal (hot wallet)
    // - active=true + xpub + no fingerprint_match → external (hardware wallet)
    // - active=true + xpub + fingerprint_match → error (ambiguous)

    if (!active) {
        return SigningCapability::None;
    }

    if (has_private_key) {
        // xprv present: MUST be internal, fingerprint MUST match
        if (fingerprint_matches) {
            return SigningCapability::Internal;
        } else {
            // ERROR: xprv present but fingerprint mismatch
            // This will be caught by ValidateActivation
            return SigningCapability::None;
        }
    } else {
        // xpub only: external signing
        // Fingerprint must be present (checked in ValidateActivation)
        return SigningCapability::External;
    }
}

ActivationValidationResult DescriptorActivationValidator::ValidateActivation(
    const std::string& descriptor_type,
    WalletPolicy wallet_policy,
    bool active,
    const std::string& fingerprint,
    const std::string& wallet_fingerprint,
    const std::vector<uint32_t>& derivation_path,
    bool has_private_key
) {
    ActivationValidationResult result;
    result.valid = false;
    result.signing_capability = SigningCapability::None;

    // Step 0: Watch-only descriptors (active=false) bypass all policy checks
    // They grant no signing authority, so policy enforcement is not needed
    if (!active) {
        result.valid = true;
        result.signing_capability = SigningCapability::None;
        return result;
    }

    // Step 1: Descriptor Type Check
    if (!ValidateDescriptorType(descriptor_type, wallet_policy)) {
        std::ostringstream oss;
        oss << "Descriptor type mismatch: cannot activate " << descriptor_type << "() descriptor in ";
        if (wallet_policy == WalletPolicy::BIP86_TAPROOT) {
            oss << "BIP86 wallet (tr() only)";
        } else if (wallet_policy == WalletPolicy::BIP84_LEGACY) {
            oss << "BIP84 wallet (wpkh() only)";
        }
        result.error_message = oss.str();
        return result;
    }

    // Step 2: Derivation Path Check
    if (!ValidateDerivationPath(derivation_path, wallet_policy)) {
        std::ostringstream oss;
        oss << "Derivation path mismatch: ";
        if (wallet_policy == WalletPolicy::BIP86_TAPROOT) {
            oss << "BIP86 wallets must use m/86h/... path";
        } else if (wallet_policy == WalletPolicy::BIP84_LEGACY) {
            oss << "BIP84 wallets must use m/84h/... path";
        }
        result.error_message = oss.str();
        return result;
    }

    // Step 3: Fingerprint Validation
    bool fingerprint_matches = (fingerprint == wallet_fingerprint);

    if (active) {
        // Active descriptors require fingerprint
        if (fingerprint.empty()) {
            result.error_message = "Active descriptor requires key origin fingerprint";
            return result;
        }

        // Internal signing requires fingerprint match
        if (has_private_key && !fingerprint_matches) {
            std::ostringstream oss;
            oss << "Fingerprint mismatch: descriptor [" << fingerprint
                << "] != wallet [" << wallet_fingerprint << "] "
                << "(internal signing requires matching fingerprint)";
            result.error_message = oss.str();
            return result;
        }

        // External signing requires fingerprint present (but not matched)
        if (!has_private_key && fingerprint_matches) {
            result.error_message = "External descriptor fingerprint should not match wallet "
                                  "(use different hardware wallet or create new wallet)";
            return result;
        }
    }

    // Step 4: Key Material Check
    if (active && has_private_key) {
        // Internal signing: xprv must be present
        // This is already validated above (fingerprint match check)
    } else if (active && !has_private_key) {
        // External signing: xpub only (no xprv)
        // This is the expected case for hardware wallets
    }

    // Determine signing capability
    result.signing_capability = DetermineSigningCapability(active, has_private_key, fingerprint_matches);

    // All checks passed
    result.valid = true;
    result.error_message = "";
    return result;
}

} // namespace dinero
