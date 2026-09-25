#pragma once
#include "primitives/uint256.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace dinero::storage {
// Persistence representation, not a cryptographic validation result. The host
// must obtain the frontier/root from the pinned Orchard tree implementation.
// Never mix this namespace with the historical shielded tree or nullifiers.
struct OrchardStoredState {
    uint32_t height = 0;
    uint256 block_hash;
    uint256 anchor;
    uint64_t pool_balance = 0;
    uint64_t tree_size = 0;
    std::string frontier;
    bool operator==(const OrchardStoredState&) const = default;
};
// Canonical logical-set digests, not RocksDB serialization or undo metadata.
struct OrchardCommitmentSets {
    uint256 nullifiers;
    uint64_t nullifier_count = 0;
    uint256 anchors;
    uint64_t anchor_count = 0;
    uint64_t anchor_references = 0;
    bool operator==(const OrchardCommitmentSets&) const = default;
};
// Storage resource ceilings; these do not establish transaction/block rules.
inline constexpr std::size_t ORCHARD_STORED_FRONTIER_LIMIT = 4096;
inline constexpr std::size_t ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT = 16384;
} // namespace dinero::storage
