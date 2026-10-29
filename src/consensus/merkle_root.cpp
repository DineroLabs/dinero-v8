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

namespace {
uint256 MerkleLayers(std::vector<uint256> layer, bool* mutated) {
    if (mutated) *mutated = false;
    if (layer.empty()) return uint256();
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

} // namespace
uint256 ComputeTransactionMerkleRoot(std::span<const TxId> ids, bool* mutated) {
    std::vector<uint256> layer; layer.reserve(ids.size());
    for (const auto& id : ids) layer.push_back(id.AsUint256());
    return MerkleLayers(std::move(layer), mutated);
}
uint256 ComputeWitnessMerkleRootFromIds(std::span<const WTxId> ids, bool* mutated) {
    std::vector<uint256> layer; layer.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        layer.push_back(i == 0 ? uint256() : ids[i].AsUint256());
    return MerkleLayers(std::move(layer), mutated);
}
uint256 ComputeMerkleRoot(const std::vector<Transaction>& vtx, bool* mutated) {
    std::vector<uint256> layer; layer.reserve(vtx.size());
    for (const auto& tx : vtx) layer.push_back(tx.GetTxid().AsUint256());
    return MerkleLayers(std::move(layer), mutated);
}
uint256 ComputeWitnessMerkleRoot(const std::vector<Transaction>& vtx, bool* mutated) {
    std::vector<uint256> layer; layer.reserve(vtx.size());
    for (size_t i = 0; i < vtx.size(); ++i)
        layer.push_back(i == 0 ? uint256() : vtx[i].GetWtxid().AsUint256());
    return MerkleLayers(std::move(layer), mutated);
}
} // namespace dinero::consensus
