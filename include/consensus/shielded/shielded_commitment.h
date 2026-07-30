#pragma once
/**
 * Dormant shielded pool coinbase-commitment experiment.
 *
 * IMPORTANT: this module has no production caller and is not part of the
 * deployed protocol. BlockHeader v1 requires reserved[12] to remain all zero,
 * and production block validation does not require this OP_RETURN. Shielded
 * state is deterministically derived from the committed transaction bundles.
 *
 * The helpers preserve an earlier proposed BIP141-shaped encoding:
 *
 *   1. A coinbase transaction could carry an OP_RETURN output with:
 *      [4-byte magic] [32-byte tree_root] [8-byte nullifier_count_le]
 *
 * Magic bytes: "DSP\x01" (Dinero Shielded Pool v1).
 *
 * Do not emit or require this encoding without a separately specified and
 * activated consensus change.
 */

#include "consensus/shielded/commitment_tree.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace dinero::consensus::shielded {

/** Magic prefix for the coinbase OP_RETURN shielded commitment. */
constexpr std::array<uint8_t, 4> SHIELDED_COMMITMENT_MAGIC = {0x44, 0x53, 0x50, 0x01}; // "DSP\x01"

/** Historical proposed flag only. Deployed BlockHeader v1 rejects its use. */
constexpr uint8_t HEADER_SHIELDED_FLAG = 0x01;

/** Total OP_RETURN payload: 4 (magic) + 32 (tree root) + 8 (nullifier count) = 44 bytes. */
constexpr size_t SHIELDED_COMMITMENT_SIZE = 44;

/**
 * Parsed shielded commitment from a coinbase OP_RETURN output.
 */
struct ShieldedBlockCommitment {
    Hash     tree_root;
    uint64_t nullifier_count;
};

/**
 * Build the OP_RETURN script for the coinbase shielded commitment.
 * Returns: OP_RETURN || PUSH(44) || magic || tree_root || nullifier_count_le
 */
std::vector<uint8_t> BuildShieldedCommitmentScript(const Hash& tree_root,
                                                    uint64_t nullifier_count);

/**
 * Extract the shielded commitment from a coinbase transaction.
 * Scans all outputs for OP_RETURN with the DSP magic prefix.
 * Returns nullopt if no valid commitment is found.
 *
 * @param coinbase_outputs  The scriptPubKeys of the coinbase tx outputs.
 */
std::optional<ShieldedBlockCommitment>
ExtractShieldedCommitment(const std::vector<std::vector<uint8_t>>& coinbase_outputs);

/**
 * Verify that a block's shielded commitment matches the computed state.
 * Called during block validation after all shielded bundles are validated.
 *
 * @param commitment  The commitment extracted from the coinbase.
 * @param tree        The commitment tree AFTER applying this block's bundles.
 * @param nullifier_count  Total nullifiers AFTER applying this block.
 */
bool VerifyShieldedCommitment(const ShieldedBlockCommitment& commitment,
                              const CommitmentTree& tree,
                              uint64_t nullifier_count);

} // namespace dinero::consensus::shielded
