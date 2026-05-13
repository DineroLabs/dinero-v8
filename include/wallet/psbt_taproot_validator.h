#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "dinero/core/wallet/psbt.h"

namespace dinero {

using din::PsbtMapKV;

// BIP 371: Taproot-specific PSBT types
enum class PSBTTaprootInputType : uint8_t {
    // Taproot Key Spend Signature (BIP 341)
    TAP_KEY_SIG = 0x13,

    // Taproot Script Spend Signature (BIP 341)
    TAP_SCRIPT_SIG = 0x14,

    // Taproot Leaf Script (BIP 341)
    TAP_LEAF_SCRIPT = 0x15,

    // Taproot BIP32 Derivation Paths
    TAP_BIP32_DERIVATION = 0x16,

    // Taproot Internal Key
    TAP_INTERNAL_KEY = 0x17,

    // Taproot Merkle Root
    TAP_MERKLE_ROOT = 0x18
};

/**
 * @brief Taproot PSBT Validation Result
 */
struct TaprootValidationResult {
    bool valid = true;
    std::string error;
    bool is_taproot = false;
    bool is_key_path = true;     // True if key-path only (no script-path)
    bool has_script_path = false; // True if script-path spending detected
};

/**
 * @brief PSBT Taproot Validator
 *
 * Enforces BIP86 key-path-only guardrails for Taproot wallets:
 * - Only allow key-path spending (simple single signature)
 * - Reject script-path spending (complex scripts)
 * - Prevent accidental creation of unspendable outputs
 */
class PSBTTaprootValidator {
public:
    /**
     * @brief Validate PSBT input for BIP86 compliance
     *
     * For BIP86 Taproot wallets (key-path only):
     * - MUST NOT have TAP_SCRIPT_SIG (script-path signature)
     * - MUST NOT have TAP_LEAF_SCRIPT (script tree)
     * - MUST NOT have TAP_MERKLE_ROOT (indicates script-path)
     * - MAY have TAP_KEY_SIG (key-path signature)
     * - MAY have TAP_INTERNAL_KEY (internal public key)
     *
     * @param input_kv Raw PSBT input key-value pairs
     * @param wallet_policy Wallet policy ("bip84" or "bip86")
     * @return Validation result with details
     */
    static TaprootValidationResult validateInput(
        const std::vector<PsbtMapKV>& input_kv,
        const std::string& wallet_policy
    );

    /**
     * @brief Check if scriptPubKey is Taproot
     *
     * Taproot scriptPubKey format: OP_1 <32-byte-x-only-pubkey>
     * - Byte 0: 0x51 (OP_1, witness version 1)
     * - Byte 1: 0x20 (push 32 bytes)
     * - Bytes 2-33: 32-byte x-only public key
     *
     * @param scriptPubKey The scriptPubKey to check
     * @return True if Taproot (witness v1), false otherwise
     */
    static bool isTaprootScriptPubKey(const std::vector<uint8_t>& scriptPubKey);

    /**
     * @brief Check if PSBT input has script-path spending data
     *
     * Script-path indicators:
     * - TAP_SCRIPT_SIG: Script-path signature
     * - TAP_LEAF_SCRIPT: Tapscript leaf
     * - TAP_MERKLE_ROOT: Merkle root (indicates script tree exists)
     *
     * @param input_kv Raw PSBT input key-value pairs
     * @return True if script-path spending detected
     */
    static bool hasScriptPath(
        const std::vector<PsbtMapKV>& input_kv
    );

    /**
     * @brief Get user-friendly error message for policy violation
     *
     * @param wallet_policy Wallet policy that was violated
     * @return Error message explaining the violation
     */
    static std::string getViolationMessage(const std::string& wallet_policy);
};

} // namespace dinero
