/**
 * Phase 11a.2: Canonical Merkle Root Implementation
 *
 * This is the single source of truth for merkle computation.
 * Locked by tests/consensus/test_merkle_invariants.cpp
 */

#include "consensus/merkle_root.h"
#include "crypto/sha256.h"
#include <cstring>

namespace dinero::consensus {

uint256 ComputeMerkleRoot(const std::vector<Transaction>& vtx, bool* mutated) {
    if (mutated) *mutated = false;
    if (vtx.empty()) {
        return uint256();  // Zero hash for empty block
    }

    // Build initial layer from transaction IDs
    std::vector<uint256> layer;
    layer.reserve(vtx.size());

    for (const auto& tx : vtx) {
        layer.push_back(tx.GetTxid().AsUint256());
    }

    // Single transaction: merkle_root == txid (invariant)
    if (layer.size() == 1) {
        return layer[0];
    }

    // Build merkle tree bottom-up
    while (layer.size() > 1) {
        std::vector<uint256> next;
        next.reserve((layer.size() + 1) / 2);

        for (size_t i = 0; i < layer.size(); i += 2) {
            const bool have_right = (i + 1 < layer.size());
            const uint256& left = layer[i];
            const uint256& right = have_right ? layer[i + 1] : layer[i];

            // CVE-2012-2459: two REAL adjacent nodes being equal means a
            // duplicated subtree that preserves the root. Distinct valid txids
            // never collide, so this only happens with a duplicated transaction
            // crafted to forge another block's merkle root. The legitimate
            // odd-count self-duplication (!have_right) is NOT a mutation.
            if (have_right && mutated && left == right) {
                *mutated = true;
            }

            // Concatenate internal uint256 bytes (64 bytes total)
            uint8_t buf[64];
            std::memcpy(buf, left.data, 32);
            std::memcpy(buf + 32, right.data, 32);

            // Double-SHA256 using canonical crypto::CSHA256
            uint8_t mid[32];
            crypto::CSHA256().Write(buf, 64).Finalize(mid);
            uint256 hash;
            crypto::CSHA256().Write(mid, 32).Finalize(hash.data);
            next.push_back(hash);
        }

        layer = std::move(next);
    }

    return layer[0];
}

// Consensus witness merkle root committed by the DINW coinbase output.
uint256 ComputeWitnessMerkleRoot(const std::vector<Transaction>& vtx, bool* mutated) {
    if (mutated) *mutated = false;
    if (vtx.empty()) {
        return uint256();  // Zero hash for empty block
    }

    // Build initial layer from witness transaction IDs (wtxids)
    std::vector<uint256> layer;
    layer.reserve(vtx.size());

    for (size_t i = 0; i < vtx.size(); ++i) {
        if (i == 0) {
            // Bitcoin convention: Coinbase wtxid is always 0x00...00
            // This is because coinbase has no inputs to commit to
            layer.push_back(uint256());
        } else {
            // Non-coinbase: wtxid = hash of serialization WITH witness data
            layer.push_back(vtx[i].GetWtxid().AsUint256());
        }
    }

    // Single transaction: witness_merkle_root == wtxid (or 0x00 for coinbase)
    if (layer.size() == 1) {
        return layer[0];
    }

    // Build merkle tree bottom-up (identical algorithm to ComputeMerkleRoot)
    while (layer.size() > 1) {
        std::vector<uint256> next;
        next.reserve((layer.size() + 1) / 2);

        for (size_t i = 0; i < layer.size(); i += 2) {
            const bool have_right = (i + 1 < layer.size());
            const uint256& left = layer[i];
            const uint256& right = have_right ? layer[i + 1] : layer[i];

            // CVE-2012-2459 mutation detection (see ComputeMerkleRoot).
            if (have_right && mutated && left == right) {
                *mutated = true;
            }

            // Concatenate internal uint256 bytes (64 bytes total)
            uint8_t buf[64];
            std::memcpy(buf, left.data, 32);
            std::memcpy(buf + 32, right.data, 32);

            // Double-SHA256 using canonical crypto::CSHA256
            uint8_t mid[32];
            crypto::CSHA256().Write(buf, 64).Finalize(mid);
            uint256 hash;
            crypto::CSHA256().Write(mid, 32).Finalize(hash.data);
            next.push_back(hash);
        }

        layer = std::move(next);
    }

    return layer[0];
}

} // namespace dinero::consensus
