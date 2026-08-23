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
#include <set>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

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

    // Number of stored children (4d-2 / #181). RUNTIME-ONLY: never serialized;
    // rebuilt by LoadFromStorage. Used to identify side-branch "tips"
    // (child_count == 0) for the bounded-storage eviction policy. An entry with
    // child_count == 0 that is not best_header_ is a losing side-branch tip.
    uint32_t child_count = 0;

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
 * Value-only ASERT ancestry derived while HeaderChainSelector is locked.
 * No selector-owned pointer or parent link escapes with this snapshot.
 */
struct HeaderAsertContext {
    uint32_t parent_height{0};
    int64_t parent_mtp{0};
    int64_t block1_time{0};
};

/**
 * @brief Order side-branch tips by ascending cumulative work (4d-2 / #181).
 *
 * Strict-weak ordering over HeaderIndexEntry pointers: by chainwork ascending,
 * then by hash ascending as a deterministic, unique tiebreaker. `begin()` of a
 * set ordered this way is the LOWEST-work tip — the eviction candidate.
 */
struct SideBranchTipLess {
    bool operator()(const HeaderIndexEntry* a, const HeaderIndexEntry* b) const {
        if (a->chainwork < b->chainwork) return true;
        if (b->chainwork < a->chainwork) return false;
        return a->hash < b->hash;
    }
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
     * @brief Copy the best header out under the lock (issue #439).
     *
     * This replaced the former raw GetBestHeader() accessor, which returned a
     * selector-owned pointer after releasing mutex_.
     *
     * This is a USE-AFTER-FREE hazard, not merely a stale/racy read: "best
     * header" is not a permanent property of an entry. A reorg can make the
     * former best header a side-branch tip, and side-branch tips are evictable
     * (EvictBranch, which runs under THIS class's mutex — not the caller's).
     * So the entry a caller is holding can be freed the moment the lock is
     * released. The same reasoning that justifies GetHeaderCopy() applies here.
     *
     * Copying under the lock removes it. The copy's parent pointer is nulled —
     * it must not be followed (same eviction hazard).
     *
     * @param out Filled with a by-value copy of the best header (parent == nullptr)
     * @return true iff a best header exists
     */
    bool GetBestHeaderCopy(HeaderIndexEntry& out) const;

    /** Value-returning best-header accessor; parent is always null. */
    std::optional<HeaderIndexEntry> GetBestHeaderValue() const;

    /**
     * @brief Look up a header by hash and copy the entry out under the lock.
     *
     * Exists for callers that may hit SIDE-BRANCH entries (e.g. the AssumeUTXO
     * backfill receive path validating snapshot-chain bodies): selector-owned
     * pointers can be freed by side-branch eviction
     * (EvictBranch, which runs under THIS class's mutex — not the caller's)
     * the moment this lock is released. Copying under the lock removes the
     * use-after-free hazard. The copied entry's parent pointer is nulled —
     * it must not be followed (same eviction hazard).
     *
     * @param hash Block hash to lookup
     * @param out  Filled with a by-value copy of the entry (parent == nullptr)
     * @return true iff the hash is known
     */
    bool GetHeaderCopy(const uint256& hash, HeaderIndexEntry& out) const;

    /** Value-returning hash accessor; parent is always null. */
    std::optional<HeaderIndexEntry> GetHeaderValue(const uint256& hash) const;

    /** Return whether a header identity is known without exporting its pointer. */
    bool ContainsHeader(const uint256& hash) const;

    /**
     * @brief Build a block locator from the best header, entirely under the lock.
     *
     * Bitcoin-style exponential back-off: tip, then walk back with gaps of
     * 1, 2, 4, 8, ... up to `max_entries` ordinary hashes, followed by the
     * genesis hash when it was not reached within that budget.
     *
     * Exists because the former raw-accessor composition — best tip followed
     * by repeated per-height lookups — was unsafe twice over (issue #441):
     *
     *   1. Both accessors return raw pointers AFTER releasing mutex_, and a
     *      reorg can demote the former best header to a side-branch tip, which
     *      EvictBranch may then free. Dereferencing is use-after-free.
     *   2. Even ignoring lifetime, the walk takes the lock once PER STEP, so a
     *      concurrent reorg between steps yields a locator that mixes hashes
     *      from different chain states — an inconsistent locator, which is
     *      worse than a stale but coherent one.
     *
     * Building the whole locator under a single lock removes both.
     *
     * @param max_entries Maximum non-genesis hashes to emit (Bitcoin uses ~10)
     * @return Locator hashes, tip first. Empty if no headers are known.
     */
    std::vector<uint256> BuildLocatorCopy(size_t max_entries = 10) const;

    /**
     * @brief Copy the best-chain header at `height` out under the lock (#441).
     *
     * Resolves best_header_->GetAncestor(height) and copies it before releasing
     * mutex_. Ancestors are especially exposed: a reorg can move the
     * best chain out from under a height that was previously on it, leaving the
     * entry an evictable side branch.
     *
     * The copy's parent pointer is nulled — it must not be followed. Callers
     * that need to walk ancestry should use CollectAncestorsByHash() or
     * BuildLocatorCopy() instead, both of which resolve the whole walk under a
     * single lock.
     *
     * @param height Best-chain height to resolve
     * @param out    Filled with a by-value copy (parent == nullptr)
     * @return true iff a best-chain header exists at that height
     */
    bool GetHeaderAtHeightCopy(uint32_t height, HeaderIndexEntry& out) const;

    /**
     * @brief Compute a header's Median Time Past entirely under the lock (#441).
     *
     * MTP is fork-aware: it walks the entry's own parent chain (11 timestamps).
     * That makes it unreachable through the *Copy accessors, which deliberately
     * null the copy's parent pointer — parents carry the same eviction hazard as
     * the entry itself.
     *
     * Rather than export a pointer so the caller can walk, the walk happens
     * here, holding mutex_ throughout. Same principle as BuildLocatorCopy():
     * derive the value inside the selector; never let ancestry escape.
     *
     * @param hash        Header to compute MTP for (may be a side branch)
     * @param mtp_out     Median Time Past, seconds since epoch
     * @param height_out  That header's height (callers usually log it)
     * @return true iff the hash is known
     */
    bool GetMedianTimePastByHash(const uint256& hash,
                                 uint32_t& mtp_out,
                                 uint32_t& height_out) const;

    /**
     * Derive all header-ancestry values needed by ASERT under one lock (#441).
     * block1_time is zero only when the parent is genesis (target height 1).
     */
    bool GetAsertContextByHash(const uint256& parent_hash,
                               HeaderAsertContext& out) const;

    /**
     * @brief Atomically resolve an anchor by hash and copy its ancestor
     *        (hash, height) pairs for heights [start_height, anchor height],
     *        ascending — all under the internal mutex.
     *
     * Exists for callers that must walk a possibly-SIDE-BRANCH anchor (the
     * AssumeUTXO backfill base): selector-owned HeaderIndexEntry pointers can
     * be freed by side-branch eviction (EvictBranch, which
     * runs under THIS class's mutex — not the caller's) the moment this
     * lock is released, so following parent pointers outside the lock is a
     * use-after-free hazard. Copying under the lock removes it. A caller must
     * not special-case the current best entry: fork choice can demote it before
     * the caller walks its parents, and a later eviction can then free it.
     *
     * @param anchor_hash      Anchor block hash (resolved by hash, never height)
     * @param start_height     Lowest height to include
     * @param anchor_height_out Set to the anchor's height when it exists
     * @param out_ascending    Cleared, then filled ascending; left empty if
     *                         start_height > anchor height
     * @return true iff the anchor hash is known
     */
    bool CollectAncestorsByHash(const uint256& anchor_hash,
                                uint32_t start_height,
                                uint32_t& anchor_height_out,
                                std::vector<std::pair<uint256, uint32_t>>& out_ascending) const;

    /**
     * @brief Resolve one ancestor hash from a hash-anchored branch while locked.
     *
     * The anchor may be the best tip or a side-branch entry. Both it and every
     * parent traversed remain protected from EvictBranch for the complete walk.
     * This is the value-only replacement for exporting an entry and calling
     * HeaderIndexEntry::GetAncestor() after the selector lock is released.
     *
     * @param anchor_hash       Branch tip/anchor to resolve by identity
     * @param ancestor_height   Height requested on that anchor's own branch
     * @param ancestor_hash_out Filled with the ancestor hash on success
     * @param anchor_height_out Filled with the anchor height when it is known
     * @return true iff the anchor exists and reaches ancestor_height
     */
    bool GetAncestorHashByHash(const uint256& anchor_hash,
                               uint32_t ancestor_height,
                               uint256& ancestor_hash_out,
                               uint32_t& anchor_height_out) const;

    /**
     * @brief Copy a hash-anchored branch back to the first supplied stop hash.
     *
     * The walk and every HeaderIndexEntry copy happen under mutex_. No selector
     * pointer or parent link escapes. The common ancestor itself is excluded
     * from out_ascending; copied branch entries are returned oldest-to-newest
     * with parent == nullptr. If none of stop_hashes is on the anchor branch,
     * common_ancestor_out remains null and the complete branch through genesis
     * is returned so callers can fail closed on incompatibility.
     *
     * @param anchor_hash         Branch tip/anchor to resolve by identity
     * @param stop_hashes         Candidate common-ancestor identities
     * @param out_ascending       Cleared, then filled oldest-to-newest
     * @param common_ancestor_out Matching stop hash, or null if none matched
     * @return true iff anchor_hash is known
     */
    bool CollectBranchCopiesByHash(
        const uint256& anchor_hash,
        const std::unordered_set<uint256>& stop_hashes,
        std::vector<HeaderIndexEntry>& out_ascending,
        uint256& common_ancestor_out) const;

    /** Value-returning best-chain height accessor; parent is always null. */
    std::optional<HeaderIndexEntry> GetHeaderAtHeightValue(uint32_t height) const;

    /** Resolve a fork point by identity without exporting selector pointers. */
    bool FindForkPointHash(const uint256& a_hash,
                           const uint256& b_hash,
                           uint256& fork_hash_out) const;

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

    // 4d-2 (issue #181): bounded side-branch header storage with work-aware
    // eviction. `evictable_tips_` holds every losing side-branch TIP
    // (child_count == 0 and != best_header_), ordered by ascending cumulative
    // work, so begin() is the lowest-work eviction candidate. The active best
    // chain (best_header_ + its ancestors) is never present here and is never
    // evicted; the AssumeUTXO/replay anchor sits on the best chain so it is
    // protected transitively. Runtime-only state (rebuilt by LoadFromStorage).
    std::set<const HeaderIndexEntry*, SideBranchTipLess> evictable_tips_;

    // Throttle the cap-hit warning so a header flood can't become a log flood.
    bool side_branch_cap_warned_ = false;

    /**
     * @brief Recompute `entry`'s membership in `evictable_tips_` to match the
     * invariant (in the set iff child_count == 0 && entry != best_header_).
     * Lock must already be held; touches members directly (no locking accessors).
     */
    void RefreshTipStatus(const HeaderIndexEntry* entry);

    /**
     * @brief Evict a losing side-branch starting at `tip`, pruning upward while
     * ancestors become childless, stopping at the best tip or the fork point.
     * Never removes a best-chain header. Lock must already be held.
     */
    void EvictBranch(const HeaderIndexEntry* tip);

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
