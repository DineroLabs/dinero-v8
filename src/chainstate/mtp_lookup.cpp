#include "consensus/mtp_lookup.h"
#include "consensus/block_index.h"  // For FindBlockIndex, CBlockIndex::GetMedianTimePast
#include "storage/chain_db.h"       // For ChainDB::getBlockHashByHeight

namespace dinero {
namespace consensus {

// ============================================================================
// BIP68 MTP Lookup Factory (Phase 23.3: Time-based Sequence Locks)
// ============================================================================

MtpLookupFn CreateMtpLookup(::dinero::ChainDB* chain_db) {
    // Fail-closed: If no ChainDB available, return nullptr
    // This means time-based sequence locks will be rejected (checkSequenceLocks returns false)
    if (!chain_db) {
        return nullptr;
    }

    // Create a lambda that captures chain_db and looks up MTP for a given height
    return [chain_db](uint32_t height) -> std::optional<uint64_t> {
        // Step 1: Get block hash at the given height
        auto hash_result = chain_db->getBlockHashByHeight(static_cast<int>(height));
        if (!hash_result.ok()) {
            // Height not found in chain - fail-closed
            return std::nullopt;
        }

        const uint256& block_hash = hash_result.value();

        // Step 2: Look up CBlockIndex in the in-memory index
        CBlockIndex* pindex = FindBlockIndex(block_hash);
        if (!pindex) {
            // Block index not found (shouldn't happen if ChainDB is consistent)
            return std::nullopt;
        }

        // Step 3: Get Median Time Past from the block index
        // GetMedianTimePast() walks back through pprev pointers to get last 11 timestamps
        uint64_t mtp = pindex->GetMedianTimePast();
        return mtp;
    };
}

} // namespace consensus
} // namespace dinero
