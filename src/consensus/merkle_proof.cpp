/**
 * Merkle Proof Generation and Verification
 *
 * See include/consensus/merkle_proof.h for specification.
 */

#include "consensus/merkle_proof.h"
#include "crypto/sha256.h"
#include <cstring>

namespace dinero::consensus {

namespace {

// Double-SHA256 of two concatenated 32-byte hashes (matches merkle tree construction)
uint256 HashPair(const uint256& a, const uint256& b) {
    uint8_t buf[64];
    std::memcpy(buf, a.data, 32);
    std::memcpy(buf + 32, b.data, 32);

    uint8_t mid[32];
    crypto::CSHA256().Write(buf, 64).Finalize(mid);
    uint256 result;
    crypto::CSHA256().Write(mid, 32).Finalize(result.data);
    return result;
}

} // anonymous namespace

std::vector<uint256> GenerateMerkleProof(
    const std::vector<Transaction>& vtx,
    size_t tx_index
) {
    if (vtx.empty() || tx_index >= vtx.size()) {
        return {};
    }

    // Single transaction: no proof needed (root == txid)
    if (vtx.size() == 1) {
        return {};
    }

    // Build initial layer from txids
    std::vector<uint256> layer;
    layer.reserve(vtx.size());
    for (const auto& tx : vtx) {
        layer.push_back(tx.GetTxid().AsUint256());
    }

    std::vector<uint256> proof;
    size_t index = tx_index;

    // Walk up the tree, capturing siblings
    while (layer.size() > 1) {
        // Find sibling index
        size_t sibling;
        if (index % 2 == 0) {
            sibling = (index + 1 < layer.size()) ? index + 1 : index;  // Duplicate last if odd
        } else {
            sibling = index - 1;
        }

        proof.push_back(layer[sibling]);

        // Build next layer
        std::vector<uint256> next;
        next.reserve((layer.size() + 1) / 2);
        for (size_t i = 0; i < layer.size(); i += 2) {
            const uint256& left = layer[i];
            const uint256& right = (i + 1 < layer.size()) ? layer[i + 1] : layer[i];
            next.push_back(HashPair(left, right));
        }

        layer = std::move(next);
        index /= 2;
    }

    return proof;
}

bool VerifyMerkleProof(
    const uint256& txid,
    const std::vector<uint256>& proof,
    size_t tx_index,
    size_t tx_count,
    const uint256& expected_root
) {
    if (tx_count == 0) return false;

    // Single transaction: root == txid, no proof needed
    if (tx_count == 1 && proof.empty()) {
        return txid == expected_root;
    }

    uint256 current = txid;
    size_t index = tx_index;

    for (const auto& sibling : proof) {
        if (index % 2 == 0) {
            current = HashPair(current, sibling);
        } else {
            current = HashPair(sibling, current);
        }
        index /= 2;
    }

    return current == expected_root;
}

} // namespace dinero::consensus
