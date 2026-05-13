#pragma once

#include "wallet/canonical_wallet_utxo.h"
#include "wallet/policy_descriptor.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinero {

/**
 * UTXO classification based on policy template.
 */
enum class UTXOClassification : uint8_t {
    STANDARD       = 0,   // Key-path only (BIP86)
    PROTECTED      = 1,   // Key-path(user) + panic + recovery
    ESCROW         = 2,   // Key-path(buyer) + release + timeout
    VAULT          = 3,   // CTV vault pattern (future)
    UNKNOWN_POLICY = 255  // Has policy but unrecognized template
};

/**
 * Extended transaction detail — enriched metadata for iOS display.
 */
struct ExtendedTxDetail {
    UTXOClassification classification = UTXOClassification::STANDARD;

    /// Policy ID (32 bytes if non-STANDARD, empty otherwise)
    std::optional<std::array<uint8_t, 32>> policy_id;

    /// Human-readable policy description
    std::string policy_description;

    /// Panic window remaining blocks (PROTECTED only)
    std::optional<uint32_t> panic_window_remaining;

    /// Recovery delay remaining blocks (PROTECTED only)
    std::optional<uint32_t> recovery_delay_remaining;

    /// Escrow state name (ESCROW only)
    std::optional<std::string> escrow_state;

    /// Template name
    std::string template_name;
};

/**
 * Transaction/UTXO classifier.
 *
 * Two classification strategies:
 * 1. Policy-based: look up policy_id from DB (fast, authoritative)
 * 2. Script-based: heuristic analysis of scriptPubKey (fallback for recovery)
 */
class TxClassifier {
public:
    /**
     * Classify a UTXO by its policy_id field.
     * Returns STANDARD if policy_id is empty.
     */
    static UTXOClassification Classify(const CanonicalWalletUTXO& utxo);

    /**
     * Get full extended detail for a UTXO at the given chain height.
     * Computes remaining timelocks, escrow state, etc.
     */
    static ExtendedTxDetail GetExtendedDetail(
        const CanonicalWalletUTXO& utxo,
        uint32_t current_height
    );

    /**
     * Fallback: classify by scriptPubKey pattern analysis.
     * Used during mnemonic-only recovery when DB is unavailable.
     *
     * Heuristics:
     * - 34-byte OP_1 <32-byte key>: Taproot (could be any template)
     * - Known script patterns in leaves: attempt to match templates
     *
     * Returns STANDARD for unrecognized scripts.
     */
    static UTXOClassification ClassifyByScript(
        const std::vector<uint8_t>& scriptPubKey
    );
};

} // namespace dinero
