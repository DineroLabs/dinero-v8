#pragma once
/**
 * BIP158 Filter Commitment Structure
 *
 * Coinbase OP_RETURN commitment to the block's GCS filter hash.
 * Follows the same pattern as witness_commitment.h (DINW magic).
 *
 * Filter Commitment Format:
 *   - Location: Coinbase transaction output (OP_RETURN)
 *   - Structure: OP_RETURN <37 bytes commitment>
 *   - Commitment = SHA256d(GCS filter encoded_data)
 *
 * Script format:
 *   [0]    = 0x6a (OP_RETURN)
 *   [1]    = 0x25 (37 bytes data length)
 *   [2-5]  = 0x444E5246 (DNRF magic = "Dinero Filter")
 *   [6]    = 0x01 (version)
 *   [7-38] = 32-byte SHA256d of GCS filter data
 *
 * Trust chain: filter ← commitment ← merkle_root ← PoW-validated header
 *
 * Activation:
 *   - Before ACTIVATION_HEIGHT: Optional — validates if present, does not require
 *   - At/after ACTIVATION_HEIGHT: MANDATORY — blocks without valid DNRF commitment are rejected
 */

#include <vector>
#include <optional>
#include <string>
#include "primitives/transaction.h"
#include "primitives/uint256.h"

namespace dinero::consensus {

struct FilterCommitment {
    // Dinero filter commitment magic: "DNRF" (0x44 0x4E 0x52 0x46)
    static constexpr uint32_t MAGIC = 0x444E5246;  // DNRF

    // Version byte (0x01 = initial)
    static constexpr uint8_t VERSION = 0x01;

    // Commitment size: 4 bytes magic + 1 byte version + 32 bytes hash
    static constexpr size_t SIZE = 37;

    // Activation height: blocks at or above this height MUST contain a valid DNRF commitment
    // NOTE: Must be set above the current chain tip when deploying to nodes that
    // have been mining without DNRF. The miner must also produce DNRF commitments
    // (SegWit coinbase + filter output) before this height is reached.
    static constexpr uint64_t ACTIVATION_HEIGHT = 1;
};

/// Check if filter commitment is mandatory at the given block height.
inline bool RequiresFilterCommitment(uint64_t height) {
    return height >= FilterCommitment::ACTIVATION_HEIGHT;
}

/**
 * Build filter commitment script for coinbase output.
 *
 * @param filter_hash SHA256d of the GCS filter encoded_data
 * @return Script for coinbase output (OP_RETURN commitment), empty if filter_hash is null
 */
std::vector<uint8_t> BuildFilterCommitmentScript(const uint256& filter_hash);

/**
 * Find filter commitment in coinbase outputs.
 * Searches backwards (last matching output wins, same as witness commitment).
 *
 * @param coinbase Coinbase transaction
 * @return Index of commitment output, or nullopt if not found
 */
std::optional<size_t> FindFilterCommitmentIndex(const Transaction& coinbase);

/**
 * Extract filter hash from coinbase commitment output.
 *
 * @param coinbase Coinbase transaction
 * @param index Output index containing commitment
 * @return 32-byte filter hash, or nullopt if invalid format
 */
std::optional<uint256> ExtractFilterCommitment(
    const Transaction& coinbase,
    size_t index
);

/**
 * Validate filter commitment in a block.
 *
 * Recomputes the GCS filter from the block's transactions and compares
 * its SHA256d hash to the commitment in the coinbase.
 *
 * Before activation: Validates IF present (optional).
 * After activation: MANDATORY — rejects blocks without valid commitment.
 *
 * @param coinbase Coinbase transaction
 * @param filter_hash Pre-computed filter hash (avoids rebuilding filter)
 * @param height Block height (for activation check)
 * @param error Output error message if validation fails
 * @return true if valid, false if invalid or missing (after activation)
 */
bool ValidateFilterCommitment(
    const Transaction& coinbase,
    const uint256& filter_hash,
    uint64_t height,
    std::string& error
);

} // namespace dinero::consensus
