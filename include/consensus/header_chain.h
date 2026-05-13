#pragma once

/**
 * Phase N.0: Header-First Sync - Header-Only Data Model
 *
 * This file defines structures for header validation and fork-choice
 * WITHOUT requiring full block bodies, UTXO sets, or ChainDB writes.
 *
 * Key Invariants (Phase N):
 * - Headers are validated without bodies
 * - Bodies are validated only after headers win fork-choice
 * - Fork-choice uses chainwork accumulation
 * - No UTXO updates during header processing
 * - No ChainDB writes during header processing
 *
 * This separation is the architectural boundary of header-first sync.
 */

#include "primitives/block.h"
#include "primitives/uint256.h"
#include "consensus/chainwork.h"
#include <memory>
#include <map>
#include <cstdint>
#include <mutex>

namespace dinero {
namespace consensus {

/**
 * @brief Header-only index entry (no transactions, no UTXO)
 *
 * Phase N.0: Pure header view for fork-choice and header validation.
 *
 * Properties:
 * - No transaction data
 * - No UTXO state
 * - No disk persistence (memory-only initially)
 * - Suitable for header-first sync
 *
 * This structure contains only what can be validated from headers alone.
 */
struct HeaderIndexEntry {
    // Header identity (Phase M.0 compliant - uint256 is identity)
    uint256 hash;           // Block hash (header hash)
    uint256 prev_hash;      // Previous block hash (linkage)

    // Chain position
    uint32_t height;        // Block height

    // Fork-choice data
    arith_uint256 chainwork;  // Accumulated proof-of-work

    // Full header for validation
    BlockHeader header;

    // Parent linkage (nullptr for genesis)
    const HeaderIndexEntry* parent;

    /**
     * @brief Default constructor
     */
    HeaderIndexEntry()
        : hash()
        , prev_hash()
        , height(0)
        , chainwork(0)
        , header()
        , parent(nullptr)
    {}

    /**
     * @brief Construct from header and parent
     *
     * Automatically computes hash, height, and chainwork.
     *
     * @param hdr Block header
     * @param prev_entry Parent header entry (nullptr for genesis)
     */
    HeaderIndexEntry(const BlockHeader& hdr, const HeaderIndexEntry* prev_entry);

    /**
     * @brief Check if this is the genesis header
     */
    bool IsGenesis() const {
        return height == 0 && parent == nullptr;
    }

    /**
     * @brief Get ancestor at specified height
     *
     * @param ancestor_height Height of ancestor to retrieve
     * @return Ancestor entry, or nullptr if not found
     */
    const HeaderIndexEntry* GetAncestor(uint32_t ancestor_height) const;

    /**
     * @brief Get Median Time Past (MTP) for this header's chain
     *
     * Computes the median timestamp of the last 11 blocks in this
     * header's ancestry (or fewer if chain is shorter).
     *
     * This is fork-aware: uses THIS header's parent chain, not the
     * active chain. Required for validating blocks on competing forks.
     *
     * @return Median time past in seconds since epoch
     */
    uint32_t GetMedianTimePast() const;
};

/**
 * @brief Header-only fork-choice engine
 *
 * Phase N.2: Determines best chain using only headers (no bodies).
 *
 * Responsibilities:
 * - Accept new headers
 * - Validate headers (stateless checks only)
 * - Compute chainwork
 * - Select best tip (highest chainwork)
 * - Track competing forks
 *
 * Explicitly NOT responsible for:
 * ❌ UTXO updates
 * ❌ Mempool interaction
 * ❌ Transaction validation
 * ❌ Reorg application (that's Phase M.0)
 * ❌ ChainDB writes
 */
class HeaderChainSelector {
public:
    /**
     * @brief Construct without persistence
     */
    HeaderChainSelector();

    /**
     * @brief Construct with persistent storage
     *
     * Phase N.1: Enables restart safety via HeaderStore.
     *
     * @param store Header storage (optional, nullptr for in-memory only)
     */
    explicit HeaderChainSelector(class HeaderStore* store);

    ~HeaderChainSelector();

    /**
     * @brief Add a new header to the header tree
     *
     * Phase N.2: Accepts header, validates it, and updates fork-choice.
     *
     * Validation includes:
     * - Version sanity
     * - Timestamp rules
     * - Difficulty target (using parent)
     * - PoW validity
     * - Linkage (prev_hash)
     *
     * Does NOT validate:
     * ❌ Merkle root (requires transactions)
     * ❌ UTXO validity
     * ❌ Transaction rules
     *
     * @param header Block header to add
     * @return true if header was valid and added, false otherwise
     */
    bool AddHeader(const BlockHeader& header);

    /**
     * @brief Get the current best header tip
     *
     * Phase N.2: Returns header with highest accumulated chainwork.
     *
     * Fork-choice rules:
     * 1. Max chainwork wins
     * 2. Height is informational only
     * 3. Ties break deterministically by hash
     *
     * @return Best header tip, or nullptr if no headers
     */
    const HeaderIndexEntry* GetBestHeader() const;

    /**
     * @brief Get header by hash
     *
     * @param hash Block hash to lookup
     * @return Header entry, or nullptr if not found
     */
    const HeaderIndexEntry* GetHeader(const uint256& hash) const;

    /**
     * @brief Get header at specific height on best chain
     *
     * @param height Block height
     * @return Header at height, or nullptr if not on best chain
     */
    const HeaderIndexEntry* GetHeaderAtHeight(uint32_t height) const;

    /**
     * @brief Find fork point between two headers
     *
     * Phase N.4: Used to determine which blocks to fetch.
     *
     * @param a First header
     * @param b Second header
     * @return Common ancestor header
     */
    const HeaderIndexEntry* FindForkPoint(
        const HeaderIndexEntry* a,
        const HeaderIndexEntry* b
    ) const;

    /**
     * @brief Get total number of headers
     */
    size_t GetHeaderCount() const;

    /**
     * @brief Clear all headers (for testing)
     */
    void Clear();

    /**
     * @brief Load headers from storage and rebuild tree
     *
     * Phase N.1: Restart safety - restores header chain from disk.
     *
     * @return true if successful
     */
    bool LoadFromStorage();

    /**
     * @brief Save current best header to storage
     *
     * @return true if successful
     */
    bool SaveBestHeader();

private:
    // Guards header_index_, best_header_, and header store interactions.
    mutable std::mutex mutex_;

    // Header storage (hash -> HeaderIndexEntry)
    std::map<uint256, std::unique_ptr<HeaderIndexEntry>> header_index_;

    // Best header tip (highest chainwork)
    const HeaderIndexEntry* best_header_;

    // Persistent storage (Phase N.1: restart safety)
    class HeaderStore* header_store_;  // Not owned, optional

    /**
     * @brief Validate header (stateless checks only)
     *
     * Phase N.1: Header validation without bodies.
     *
     * @param header Header to validate
     * @param prev Parent header (nullptr for genesis)
     * @return true if valid
     */
    bool ValidateHeader(
        const BlockHeader& header,
        const HeaderIndexEntry* prev
    );

    /**
     * @brief Update best header after adding new header
     *
     * @param new_entry Newly added header
     */
    void UpdateBestHeader(const HeaderIndexEntry* new_entry);

    /**
     * @brief Compute chainwork for a header
     *
     * @param header Block header
     * @param parent_chainwork Parent's accumulated chainwork
     * @return New accumulated chainwork
     */
    arith_uint256 ComputeChainwork(
        const BlockHeader& header,
        const arith_uint256& parent_chainwork
    );
};

} // namespace consensus
} // namespace dinero
