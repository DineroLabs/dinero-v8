/**
 * @file hash_domains.cpp
 * @brief Phase M.3: Semantic Hash Domains - Implementation
 *
 * Domain-locked constructors that ensure hashes are computed in the correct semantic domain
 */

#include "primitives/hash_domains.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "crypto/sha256.h"
#include <algorithm>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════
// BlockHash Domain-Locked Constructor
// ═══════════════════════════════════════════════════════════════════════

BlockHash BlockHash::Compute(const BlockHeader& header) {
    // Use existing GetHash() method which correctly computes block hash
    // from the full 128-byte header (BlockHeader v1)
    return BlockHash(header.GetHash());
}

// ═══════════════════════════════════════════════════════════════════════
// TxId Domain-Locked Constructor
// ═══════════════════════════════════════════════════════════════════════

TxId TxId::Compute(const Transaction& tx) {
    // Use existing GetTxid() method which correctly computes txid
    // (hash of transaction without witness data - non-malleable)
    return TxId(tx.GetTxid());
}

// ═══════════════════════════════════════════════════════════════════════
// WTxId Domain-Locked Constructor
// ═══════════════════════════════════════════════════════════════════════

WTxId WTxId::Compute(const Transaction& tx) {
    // Use existing GetWtxid() method which correctly computes wtxid
    // (hash of transaction with witness data - includes witness commitment)
    return WTxId(tx.GetWtxid());
}

// ═══════════════════════════════════════════════════════════════════════
// MerkleRoot Domain-Locked Constructor
// ═══════════════════════════════════════════════════════════════════════

MerkleRoot MerkleRoot::Compute(const std::vector<uint256>& leaves) {
    // Bitcoin-style merkle tree computation
    // If empty, return null hash
    if (leaves.empty()) {
        return MerkleRoot(uint256());
    }

    // If single leaf, return it directly
    if (leaves.size() == 1) {
        return MerkleRoot(leaves[0]);
    }

    // Build merkle tree bottom-up
    std::vector<uint256> tree = leaves;

    while (tree.size() > 1) {
        std::vector<uint256> next_level;

        for (size_t i = 0; i < tree.size(); i += 2) {
            // If odd number of elements, duplicate the last one (Bitcoin consensus rule)
            const uint256& left = tree[i];
            const uint256& right = (i + 1 < tree.size()) ? tree[i + 1] : tree[i];

            // Concatenate and double-SHA256
            std::vector<uint8_t> combined;
            combined.reserve(64);

            // Append left (32 bytes)
            combined.insert(combined.end(),
                           reinterpret_cast<const uint8_t*>(&left),
                           reinterpret_cast<const uint8_t*>(&left) + 32);

            // Append right (32 bytes)
            combined.insert(combined.end(),
                           reinterpret_cast<const uint8_t*>(&right),
                           reinterpret_cast<const uint8_t*>(&right) + 32);

            // Double-SHA256 hash
            uint256 hash;
            crypto::CSHA256 sha1, sha2;
            sha1.Write(combined.data(), combined.size());
            uint8_t temp[32];
            sha1.Finalize(temp);
            sha2.Write(temp, 32);
            sha2.Finalize(reinterpret_cast<uint8_t*>(&hash));

            next_level.push_back(hash);
        }

        tree = next_level;
    }

    return MerkleRoot(tree[0]);
}

}  // namespace dinero
