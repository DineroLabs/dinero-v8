#pragma once
/**
 * Merkle Proof Generation and Verification
 *
 * Generates inclusion proofs for transactions in blocks.
 * Used by lightweight clients to verify that a coinbase transaction
 * (containing the filter commitment) is part of the block.
 *
 * Trust chain: filter ← coinbase commitment ← merkle proof ← merkle_root ← header
 */

#include <vector>
#include "primitives/transaction.h"
#include "primitives/uint256.h"

namespace dinero::consensus {

/**
 * Generate merkle proof for a transaction at `tx_index` in a block.
 *
 * Returns the sibling hashes needed to reconstruct the path from
 * the transaction's leaf to the merkle root.
 *
 * @param vtx Block transactions
 * @param tx_index Index of the transaction to prove (0 for coinbase)
 * @return Vector of sibling hashes (bottom-up), empty if invalid index
 */
std::vector<uint256> GenerateMerkleProof(
    const std::vector<Transaction>& vtx,
    size_t tx_index
);

/**
 * Verify a merkle proof.
 *
 * @param txid Transaction hash (leaf)
 * @param proof Sibling hashes from GenerateMerkleProof (bottom-up)
 * @param tx_index Index of the transaction in the block
 * @param tx_count Total number of transactions in the block
 * @param expected_root Expected merkle root from the block header
 * @return true if proof is valid
 */
bool VerifyMerkleProof(
    const uint256& txid,
    const std::vector<uint256>& proof,
    size_t tx_index,
    size_t tx_count,
    const uint256& expected_root
);

} // namespace dinero::consensus
