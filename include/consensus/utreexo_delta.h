#pragma once

#include <vector>
#include <cstdint>
#include "consensus/utreexo_accumulator.h"

namespace dinero {
namespace consensus {

/**
 * Deleted leaf in Utreexo forest
 * Used for delta-based undo mechanism
 */
struct DeletedLeaf {
    UtreexoHash leafHash;       // Leaf hash
    uint64_t position;          // Position in forest

    DeletedLeaf() : position(0) {}
    DeletedLeaf(const UtreexoHash& h, uint64_t pos) : leafHash(h), position(pos) {}
};

/**
 * Added leaf in Utreexo forest
 * Phase 11a: Carries position for proof generation indexing
 */
struct AddedLeaf {
    UtreexoHash hash;           // Leaf hash
    uint64_t position;          // Position assigned by accumulator

    AddedLeaf() : position(0) {}
    AddedLeaf(const UtreexoHash& h, uint64_t pos) : hash(h), position(pos) {}
};

/**
 * Utreexo delta for a block
 * Captures state changes for efficient rollback
 * Phase 4/6: Delta-based undo (10-20x size reduction vs snapshots)
 * Phase 11a: Extended to carry positions for indexing (consensus-neutral)
 */
struct UtreexoDelta {
    uint64_t numLeavesBefore;                    // Number of leaves before block
    std::vector<DeletedLeaf> deletedLeaves;      // Leaves deleted in this block
    std::vector<AddedLeaf> addedLeaves;          // Leaves added in this block (with positions)

    UtreexoDelta() : numLeavesBefore(0) {}

    void recordDelete(uint64_t position, const UtreexoHash& leafHash) {
        deletedLeaves.emplace_back(leafHash, position);
    }

    // Phase 11a: recordAdd now captures position from accumulator
    void recordAdd(const UtreexoHash& leafHash, uint64_t position) {
        addedLeaves.emplace_back(leafHash, position);
    }

    size_t getSize() const {
        return deletedLeaves.size() + addedLeaves.size();
    }
};

} // namespace consensus
} // namespace dinero
