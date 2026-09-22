#include "consensus/block_lifecycle.h"
#include "consensus/block_index.h"
#include "common/logger.h"
#include <chrono>
#include <unordered_set>
#include <vector>

namespace dinero {

// Global state tracking
std::unordered_map<uint256, InvalidBlockEntry> g_invalid_blocks;
std::unordered_map<uint256, InFlightBlock> g_inflight_blocks;
std::unordered_map<uint256, uint256> g_invalid_descendants;

// Negative-result cache for HasInvalidAncestor(): hashes whose entire ancestry
// has been walked and found free of BLOCK_FAILED_VALID. Cleared by
// InvalidateAncestryCache() whenever any index entry gains a failure flag.
// Without this the walk is O(chain height) per call, and GetBestCandidate()
// runs it for every candidate on every ActivateBestChain pass: a node that
// accumulated thousands of same-height sibling tips (a pool re-submitting a
// stale job) spent ~1.3 s per pass at height 114k, saturating the block
// ingress lock and starving its peer sockets.
// All cache operations use the existing recursive block-index mutex. Candidate
// selection already owns it before calling HasInvalidAncestor; reusing it
// avoids introducing a second lock with an inverse acquisition order.
static std::unordered_set<uint256> g_clean_ancestry;
uint64_t g_invalid_ancestor_walk_steps = 0;

void InvalidateAncestryCache() {
    std::lock_guard<std::recursive_mutex> lock(g_block_index_mutex);
    g_clean_ancestry.clear();
}

/**
 * Mark block as invalid and propagate to all descendants
 *
 * This is critical for DoS prevention - once a block is proven invalid,
 * we never reprocess it or any of its descendants.
 */
void MarkBlockInvalid(CBlockIndex* pindex, BlockRejectReason reason, const std::string& message) {
    if (!pindex) return;
    std::lock_guard<std::recursive_mutex> lock(g_block_index_mutex);

    // Add to invalid block cache
    g_invalid_blocks.emplace(pindex->GetBlockHash(),
                            InvalidBlockEntry(pindex->GetBlockHash(), reason, message));

    // Set failure flags
    pindex->status |= BLOCK_FAILED_VALID;
    InvalidateAncestryCache();

    g_logger.log(LogLevel::WARNING, "Block marked invalid: " + message);

    // Propagate failure to all descendants (recursive)
    PropagateInvalidToDescendants(pindex);

    // Remove from candidate tips if present
    RemoveCandidate(pindex);
}

/**
 * Recursively mark all descendants as having invalid ancestor
 */
void PropagateInvalidToDescendants(CBlockIndex* pindex) {
    if (!pindex) return;
    std::lock_guard<std::recursive_mutex> lock(g_block_index_mutex);

    for (CBlockIndex* child : pindex->children) {
        if (!child) continue;

        // Mark child as having invalid ancestor
        child->status |= BLOCK_FAILED_CHILD;
        g_invalid_descendants[child->GetBlockHash()] = pindex->GetBlockHash();

        g_logger.log(LogLevel::DEBUG, "Descendant marked invalid");

        // Recursively propagate to grandchildren
        PropagateInvalidToDescendants(child);

        // Remove from candidate tips
        RemoveCandidate(child);
    }
}

/**
 * Check if block is directly invalid
 */
bool IsBlockInvalid(const uint256& block_hash) {
    std::lock_guard<std::recursive_mutex> lock(g_block_index_mutex);
    // Check invalid block cache first (fast path)
    if (g_invalid_blocks.count(block_hash)) {
        return true;
    }

    // Check block index status
    CBlockIndex* pindex = FindBlockIndex(block_hash);
    if (pindex) {
        return (pindex->status & BLOCK_FAILED_VALID) != 0;
    }

    return false;
}

/**
 * Check if block has invalid ancestor
 */
bool HasInvalidAncestor(const CBlockIndex* pindex) {
    if (!pindex) return false;
    // Hold through lookup, the ancestry walk and cache publication. Locking
    // individual unordered_set operations would allow an invalidation between
    // the walk and insertion to publish a stale clean result afterwards.
    std::lock_guard<std::recursive_mutex> lock(g_block_index_mutex);

    // Check BLOCK_FAILED_CHILD flag (fast path)
    if (pindex->status & BLOCK_FAILED_CHILD) {
        return true;
    }

    // Check descendant cache
    if (g_invalid_descendants.count(pindex->GetBlockHash())) {
        return true;
    }

    // Walk chain backward to find invalid ancestor. Stop at the first ancestor
    // whose own ancestry is already known clean; everything walked below that
    // point is then clean too and is recorded, so repeated calls over
    // siblings/descendants cost O(new blocks), not O(chain height).
    //
    // A walk may only be recorded as clean when it PROVED the whole ancestry:
    // it ended at genesis (height 0, no parent) or at an entry already proven
    // clean. A null pprev on a higher block means the parent has not arrived
    // yet (orphan-queued); the missing parent can later link this branch below
    // an already-invalid block, so such a walk proves nothing and must not be
    // cached (PR #800 review, orphan_cache_repro).
    std::vector<const CBlockIndex*> walked;
    bool ancestry_complete = false;
    const CBlockIndex* current = pindex->pprev;
    while (current) {
        ++g_invalid_ancestor_walk_steps;
        if (current->status & BLOCK_FAILED_VALID) {
            // Cache this result
            g_invalid_descendants[pindex->GetBlockHash()] = current->GetBlockHash();
            return true;
        }
        if (g_clean_ancestry.count(current->GetBlockHash())) {
            ancestry_complete = true;
            break;
        }
        walked.push_back(current);
        if (!current->pprev) {
            ancestry_complete = (current->height == 0);
            break;
        }
        current = current->pprev;
    }
    if (ancestry_complete) {
        for (const CBlockIndex* clean : walked) {
            g_clean_ancestry.insert(clean->GetBlockHash());
        }
    }

    return false;
}

// #309: whole-branch-data check — see header for rationale.
bool BranchHasDataToConnectedBase(const CBlockIndex* pindex) {
    for (const CBlockIndex* cur = pindex; cur; cur = cur->pprev) {
        if (cur->status & BLOCK_VALID_CHAIN) return true;   // reached connected base
        if (!(cur->status & BLOCK_HAVE_DATA)) return false; // body gap above the base
        if (cur->IsGenesis()) return true;                  // genesis with data == base
    }
    return false; // ran off the top without a connected base
}

size_t ContiguousBodyPrefix(const std::vector<CBlockIndex*>& connect_path) {
    size_t n = 0;
    while (n < connect_path.size() && connect_path[n] && (connect_path[n]->status & BLOCK_HAVE_DATA)) ++n;
    return n;
}

bool BranchHasDataToFork(const CBlockIndex* candidate, const CBlockIndex* active_tip) {
    if (!candidate || !active_tip) return false;

    const CBlockIndex* candidate_walk = candidate;
    const CBlockIndex* active_walk = active_tip;
    while (candidate_walk && active_walk && candidate_walk->height > active_walk->height) {
        if (!(candidate_walk->status & BLOCK_HAVE_DATA)) return false;
        candidate_walk = candidate_walk->pprev;
    }
    while (candidate_walk && active_walk && active_walk->height > candidate_walk->height) {
        active_walk = active_walk->pprev;
    }
    while (candidate_walk && active_walk && candidate_walk->hash != active_walk->hash) {
        if (!(candidate_walk->status & BLOCK_HAVE_DATA)) return false;
        candidate_walk = candidate_walk->pprev;
        active_walk = active_walk->pprev;
    }
    return candidate_walk && active_walk && candidate_walk->hash == active_walk->hash;
}

/**
 * Mark block as in-flight (requested from peer)
 */
void MarkBlockInFlight(const uint256& block_hash, uint64_t peer_id) {
    // Check for duplicate request
    auto it = g_inflight_blocks.find(block_hash);
    if (it != g_inflight_blocks.end()) {
        it->second.duplicate_request = true;
        g_logger.log(LogLevel::DEBUG, "Duplicate block request");
        return;
    }

    // Add to in-flight tracking
    g_inflight_blocks.emplace(block_hash, InFlightBlock(block_hash, peer_id));

    // Update block index status flag (runtime only, not persisted)
    CBlockIndex* pindex = FindBlockIndex(block_hash);
    if (pindex) {
        pindex->status |= BLOCK_IN_FLIGHT;
    }

    g_logger.log(LogLevel::DEBUG, "Block marked in-flight");
}

/**
 * Mark block as received (no longer in-flight)
 */
void MarkBlockReceived(const uint256& block_hash) {
    // Remove from in-flight tracking
    auto it = g_inflight_blocks.find(block_hash);
    if (it != g_inflight_blocks.end()) {
        g_logger.log(LogLevel::DEBUG, "Block received");
        g_inflight_blocks.erase(it);
    }

    // Clear in-flight flag
    CBlockIndex* pindex = FindBlockIndex(block_hash);
    if (pindex) {
        pindex->status &= ~BLOCK_IN_FLIGHT;
    }
}

/**
 * Check if block is currently in-flight
 */
bool IsBlockInFlightFrom(const uint256& block_hash, uint64_t& peer_id) {
    auto it = g_inflight_blocks.find(block_hash);
    if (it != g_inflight_blocks.end()) {
        peer_id = it->second.peer_id;
        return true;
    }
    return false;
}

/**
 * Clean up timed-out block requests
 *
 * Should be called periodically (e.g., every 60 seconds) to re-request blocks
 * that timed out.
 */
void CleanupTimedOutRequests() {
    std::vector<uint256> timed_out;

    for (auto& [hash, inflight] : g_inflight_blocks) {
        if (inflight.IsTimedOut()) {
            timed_out.push_back(hash);
        }
    }

    for (const auto& hash : timed_out) {
        g_logger.log(LogLevel::WARNING, "Block request timed out: hash=" + hash.GetHex());

        // Clear in-flight status
        MarkBlockReceived(hash);

        // Block can be re-requested now
        // (actual re-request logic is in P2P layer)
    }

    if (!timed_out.empty()) {
        g_logger.log(LogLevel::INFO, "Cleaned up timed-out block requests: count=" + std::to_string(timed_out.size()));
    }
}

/**
 * Check if block can progress to next validation state
 */
bool CanProgressToNextState(const CBlockIndex* pindex) {
    if (!pindex) return false;

    // Cannot progress if invalid
    if (IsFailed(pindex->status)) return false;

    // Cannot progress if has invalid ancestor
    if (HasInvalidAncestor(pindex)) return false;

    // Cannot progress if orphaned (parent missing)
    if (IsOrphaned(pindex)) return false;

    // Can progress if in-flight (waiting for data) but not actually blocked
    return true;
}

/**
 * Transition block to new validation state
 *
 * This enforces the state machine transitions defined in block_lifecycle.h
 */
void TransitionBlockState(CBlockIndex* pindex, uint32_t new_status_flags) {
    if (!pindex) return;

    uint32_t old_status = pindex->status;
    pindex->status |= new_status_flags;

    g_logger.log(LogLevel::DEBUG, "Block state transition: hash=" + pindex->GetBlockHash().GetHex() +
        " height=" + std::to_string(pindex->height) +
        " old_status=" + std::to_string(old_status) +
        " new_status=" + std::to_string(pindex->status));

    // If transitioning to BLOCK_VALID_CHAIN, add to candidate tips
    if ((new_status_flags & BLOCK_VALID_CHAIN) && !(old_status & BLOCK_VALID_CHAIN)) {
        AddCandidate(pindex);
    }

    // If transitioning to fully validated, log milestone
    if ((new_status_flags & BLOCK_VALID_SCRIPTS) && !(old_status & BLOCK_VALID_SCRIPTS)) {
        g_logger.log(LogLevel::INFO, "Block fully validated: hash=" + pindex->GetBlockHash().GetHex() +
            " height=" + std::to_string(pindex->height) +
            " chainwork=" + pindex->chainwork);
    }
}

} // namespace dinero
