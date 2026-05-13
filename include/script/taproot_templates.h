#pragma once

/**
 * Phase 24.5: Taproot Template Engine
 *
 * Expands #TAPROOTOUTPUT#, #CONTROLBLOCK#, and #SCRIPT# placeholders
 * in Bitcoin Core script_tests.json format for test validation.
 *
 * This module is used ONLY for testing, not for consensus validation.
 */

#include <vector>
#include <string>
#include <cstdint>

namespace dinero {
namespace script {

/**
 * Context for Taproot template expansion
 */
struct TaprootTemplateContext {
    // Internal key (32-byte x-only pubkey)
    std::vector<uint8_t> internal_key;

    // Script to execute (bytecode)
    std::vector<uint8_t> script;

    // Leaf version (default 0xc0 for tapscript v0)
    uint8_t leaf_version = 0xc0;

    // Merkle branches (for multi-leaf trees, empty for single leaf)
    std::vector<std::vector<uint8_t>> merkle_branches;
};

/**
 * Result of Taproot template expansion
 */
struct TaprootTemplateResult {
    // 32-byte tweaked output key (x-only)
    std::vector<uint8_t> output_key;

    // Control block: leaf_version|parity + internal_key + merkle_path
    std::vector<uint8_t> control_block;

    // Script bytecode
    std::vector<uint8_t> script;

    // Parity of the output key (0 or 1)
    int output_parity;

    // TapLeaf hash
    std::vector<uint8_t> tapleaf_hash;

    // Merkle root (same as tapleaf_hash for single leaf)
    std::vector<uint8_t> merkle_root;

    // Success flag
    bool success = false;

    // Error message if failed
    std::string error;
};

// ============================================================================
// Main API
// ============================================================================

/**
 * Expand a Taproot template with given context
 */
TaprootTemplateResult ExpandTaprootTemplate(const TaprootTemplateContext& ctx);

/**
 * Check if a string contains Taproot template placeholders
 */
bool HasTaprootPlaceholders(const std::string& str);

/**
 * Expand placeholders in witness element
 * Returns the expanded hex string
 */
std::string ExpandWitnessPlaceholder(const std::string& witness_element,
                                     const TaprootTemplateResult& result);

/**
 * Expand placeholders in scriptPubKey
 * Returns the expanded script string
 */
std::string ExpandScriptPubKeyPlaceholder(const std::string& script_str,
                                          const TaprootTemplateResult& result);

// ============================================================================
// Low-Level Functions
// ============================================================================

/**
 * Compute TapLeaf hash: tagged_hash("TapLeaf", leaf_version || script)
 */
std::vector<uint8_t> ComputeTapLeafHash(uint8_t leaf_version,
                                        const std::vector<uint8_t>& script);

/**
 * Compute TapBranch hash: tagged_hash("TapBranch", sorted(left, right))
 */
std::vector<uint8_t> ComputeTapBranchHash(const std::vector<uint8_t>& left,
                                          const std::vector<uint8_t>& right);

/**
 * Compute TapTweak hash: tagged_hash("TapTweak", pubkey || merkle_root)
 */
std::vector<uint8_t> ComputeTapTweakHash(const std::vector<uint8_t>& internal_key,
                                         const std::vector<uint8_t>& merkle_root);

/**
 * Tweak an x-only pubkey by adding tweak * G
 * Returns tweaked key and parity
 */
bool TweakXOnlyPubkey(const std::vector<uint8_t>& internal_key,
                      const std::vector<uint8_t>& tweak,
                      std::vector<uint8_t>& output_key,
                      int& output_parity);

/**
 * Build control block for script-path spend
 * Format: (leaf_version | output_parity) || internal_key || merkle_path
 */
std::vector<uint8_t> BuildControlBlock(uint8_t leaf_version,
                                       int output_parity,
                                       const std::vector<uint8_t>& internal_key,
                                       const std::vector<std::vector<uint8_t>>& merkle_path);

/**
 * Get the standard test internal key (BIP-341 test vector key)
 * This is the "nothing up my sleeve" key for testing
 */
std::vector<uint8_t> GetTestInternalKey();

/**
 * Convert hex string to bytes
 */
std::vector<uint8_t> HexToBytes(const std::string& hex);

/**
 * Convert bytes to hex string
 */
std::string BytesToHex(const std::vector<uint8_t>& bytes);

} // namespace script
} // namespace dinero
