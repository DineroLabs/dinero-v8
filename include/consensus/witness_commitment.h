#pragma once
/**
 * Phase 11c.1: Witness Commitment Structure
 *
 * BIP 141-style witness commitments (OP_RETURN in coinbase output)
 *
 * Witness Commitment Format:
 *   - Location: Coinbase transaction output (OP_RETURN)
 *   - Structure: OP_RETURN <36 bytes commitment>
 *   - Commitment = SHA256(witness_merkle_root || witness_nonce)
 *   - witness_nonce = 32 bytes (usually all zeros for simplicity)
 *
 * Why NOT in block header:
 *   - Avoids hard fork
 *   - Backward compatible
 *   - Can be made mandatory via soft fork
 *   - Follows Bitcoin BIP 141 design
 *
 * Status: OPTIONAL (Phase 11c)
 *   - Mining: Creates commitment if block has witness data
 *   - Validation: Validates commitment IF present
 *   - NOT required yet (will become mandatory in Phase 11d)
 *
 * Locked by: tests/consensus/test_witness_commitment.cpp (Phase 11c.4)
 */

#include <vector>
#include <optional>
#include "primitives/transaction.h"
#include "primitives/uint256.h"

namespace dinero::consensus {

/**
 * Witness commitment constants (DINERO-SPECIFIC)
 *
 * ⚠️ CRITICAL: Uses Dinero magic, NOT Bitcoin's 0xaa21a9ed
 *
 * Why Dinero-specific magic:
 *   - Bitcoin magic implies BIP141 segwit semantics (NOT active yet)
 *   - Prevents false assumptions from external tools/explorers
 *   - Enables clean future migration path
 *   - Avoids accidental soft-fork activation
 *
 * Magic bytes: 0x444E5257 ("DINW" = Dinero Witness)
 *   - Human-readable in hex dumps
 *   - Impossible to confuse with Bitcoin
 *   - Easy to grep in block explorers
 */
struct WitnessCommitment {
    // Dinero witness commitment magic: "DINW" (0x44 0x4E 0x52 0x57)
    static constexpr uint32_t MAGIC = 0x444E5257;  // DINW

    // Bitcoin witness commitment magic: 0xaa21a9ed (for translation only)
    // Phase 11e: Used ONLY when translation switch is enabled
    // Default: NEVER used (translation is OFF)
    static constexpr uint32_t BITCOIN_MAGIC = 0xaa21a9ed;  // Bitcoin BIP141

    // Version byte (0x01 = Phase 11c groundwork, NOT enforced)
    // Version semantics:
    //   0x01 - Witness merkle container only, optional, NOT consensus-active
    //   Future versions may add enforcement, activation, etc.
    static constexpr uint8_t VERSION = 0x01;

    // Commitment size: 4 bytes magic + 1 byte version + 32 bytes hash
    static constexpr size_t SIZE = 37;

    // ═══════════════════════════════════════════════════════════════════════
    // DESIGN DECISION: Deterministic Witness Nonce
    // ═══════════════════════════════════════════════════════════════════════
    // Dinero uses a DETERMINISTIC witness commitment nonce (32 zero bytes)
    // instead of a coinbase witness stack like Bitcoin BIP-141.
    //
    // Bitcoin approach:
    //   - Miner puts 32-byte nonce in coinbase.vin[0].witness[0]
    //   - commitment = SHA256(SHA256(witness_merkle_root || coinbase_witness))
    //   - Nonce is miner-chosen (typically zeros, but can vary)
    //
    // Dinero approach:
    //   - Nonce is ALWAYS 32 zero bytes (deterministic)
    //   - Coinbase has NO witness data (witness_version = 0xFF)
    //   - commitment = SHA256(SHA256(witness_merkle_root || 0x00...00))
    //
    // Why this divergence is INTENTIONAL:
    //   1. DETERMINISM: Same block always produces same commitment
    //   2. UTREEXO: Stateless validation doesn't need coinbase witness lookup
    //   3. ZK PROOFS: Deterministic inputs simplify circuit design
    //   4. SIMPLICITY: No extra serialization complexity for coinbase
    //
    // Security properties PRESERVED:
    //   - Witness data is still committed to merkle root
    //   - Commitment binds witness merkle to coinbase
    //   - Tampering detection is identical
    //
    // DO NOT "fix" this to match Bitcoin. This is a deliberate design choice.
    // ═══════════════════════════════════════════════════════════════════════
    static const std::vector<uint8_t> DEFAULT_NONCE;  // 32 zero bytes (deterministic)
};

/**
 * Build witness commitment for a block
 *
 * Phase 11c.1: Creates BIP 141-style witness commitment
 *
 * Algorithm:
 * 1. Compute witness merkle root from transactions
 * 2. Concatenate: witness_merkle_root || witness_nonce
 * 3. Hash with SHA256: commitment = SHA256(merkle || nonce)
 * 4. Build script: OP_RETURN <DINW magic> <version> <commitment>
 *
 * Script format:
 *   OP_RETURN <37 bytes>
 *   [0]    = 0x6a (OP_RETURN)
 *   [1]    = 0x25 (37 bytes data length)
 *   [2-5]  = 0x444E5257 (DINW magic)
 *   [6]    = 0x01 (version)
 *   [7-38] = 32-byte witness commitment hash
 *
 * @param vtx Block transactions (must include coinbase at index 0)
 * @param witness_nonce Optional nonce (default: 32 zero bytes)
 * @return Script for coinbase output (OP_RETURN commitment)
 */
std::vector<uint8_t> BuildWitnessCommitment(
    const std::vector<Transaction>& vtx,
    const std::vector<uint8_t>& witness_nonce = WitnessCommitment::DEFAULT_NONCE
);

/**
 * Find witness commitment in coinbase outputs
 *
 * Searches coinbase outputs for witness commitment (OP_RETURN with magic).
 *
 * @param coinbase Coinbase transaction
 * @return Index of commitment output, or nullopt if not found
 */
std::optional<size_t> FindWitnessCommitmentIndex(const Transaction& coinbase);

/**
 * Extract witness commitment from coinbase output
 *
 * Parses commitment script to extract the 32-byte hash.
 *
 * @param coinbase Coinbase transaction
 * @param index Output index containing commitment
 * @return 32-byte commitment hash, or nullopt if invalid
 */
std::optional<uint256> ExtractWitnessCommitment(
    const Transaction& coinbase,
    size_t index
);

/**
 * Validate witness commitment in a block
 *
 * Phase 11c.3: Validates commitment IF present (not required yet)
 *
 * Steps:
 * 1. Find witness commitment in coinbase
 * 2. If not found: return true (optional in Phase 11c)
 * 3. Extract commitment hash
 * 4. Compute expected witness merkle root
 * 5. Compute expected commitment
 * 6. Compare extracted vs expected
 *
 * @param vtx Block transactions (must include coinbase at index 0)
 * @param error Output error message if validation fails
 * @return true if valid (or not present), false if present but invalid
 */
bool ValidateWitnessCommitment(
    const std::vector<Transaction>& vtx,
    std::string& error
);

/**
 * Check if witness commitment is required for this block (Phase 11d.1)
 *
 * Phase 11d.1: Enforcement plumbing (OFF by default)
 *
 * Enforcement rule:
 * - If enforce_witness_commitment = true AND height >= enforcement_height:
 *   * Blocks with witness data MUST have valid witness commitment
 *   * Blocks without witness data are NOT affected
 *   * Missing/invalid commitment → block rejected
 *
 * @param vtx Block transactions (must include coinbase at index 0)
 * @param height Block height
 * @param enforce_commitment Enforcement flag from consensus params
 * @param enforcement_height Height to start enforcement
 * @param error Output error message if enforcement fails
 * @return true if block passes enforcement check, false otherwise
 */
bool EnforceWitnessCommitment(
    const std::vector<Transaction>& vtx,
    uint32_t height,
    bool enforce_commitment,
    uint32_t enforcement_height,
    std::string& error
);

/**
 * Translate DINW witness magic to Bitcoin magic (Phase 11e.1)
 *
 * Phase 11e.1: Bitcoin compatibility translation (OFF by default)
 *
 * Translation rule:
 * - This is INTERPRETATION, not mutation
 * - NO block rewrite, NO header changes, NO history modification
 * - If translation enabled AND height >= translation_height:
 *   * DINW commitments (0x444E5257) are interpreted as Bitcoin (0xaa21a9ed)
 *   * Validation sees Bitcoin magic, mining still creates DINW
 *   * Pure validation-time interpretation only
 *
 * What this does NOT do:
 * - ❌ NOT changing blocks, headers, or txids
 * - ❌ NOT affecting mining (still creates DINW)
 * - ❌ NOT Bitcoin SegWit activation
 * - ❌ NOT changing consensus rules
 *
 * @param magic Original magic bytes from commitment
 * @param version Commitment version byte
 * @param height Block height
 * @param enable_translation Translation flag from consensus params
 * @param translation_height Height to start translation
 * @return Translated magic (Bitcoin if conditions met, original otherwise)
 */
uint32_t TranslateWitnessMagic(
    uint32_t magic,
    uint8_t version,
    uint32_t height,
    bool enable_translation,
    uint32_t translation_height
);

} // namespace dinero::consensus
