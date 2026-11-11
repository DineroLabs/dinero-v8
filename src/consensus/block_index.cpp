#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "consensus/target_helpers.h"
#include "consensus/chainwork.h"
#include "common/logger.h"
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <mutex>

namespace dinero {

// Global block index storage
std::unordered_map<uint256, std::unique_ptr<CBlockIndex>> g_block_index;

// #353: g_block_index / g_candidates / g_orphan_pool are shared, mutable global
// state with no other guard. Canonical #360 added a genesis→base materialization
// loop inside LoadSnapshot that hammers AddBlockIndex on the load thread while the
// P2P/scheduler thread also calls AddBlockIndex/FindBlockIndex for incoming blocks
// — concurrent unordered_map insert + read triggers a rehash-under-read → heap
// corruption → nondeterministic crash mid-materialization. RECURSIVE (AddBlockIndex
// nests FindBlockIndex/MarkBlockValid/OnParentValidated) and INNERMOST: every
// function below is self-contained (never calls out to activation_mutex_-holding
// code), so the only lock order is [caller lock] -> g_block_index_mutex, never the
// reverse — deadlock-free by construction.
std::recursive_mutex g_block_index_mutex;

// Global candidate tips (ordered by work, then hash)
BlockCandidates g_candidates;

// Orphan pool for headers/blocks with missing parents
std::unordered_map<uint256, std::vector<CBlockIndex*>> g_orphan_pool;

// BIP113: Median Time Past calculation
// Used for CHECKLOCKTIMEVERIFY time-based locktime validation
uint64_t CBlockIndex::GetMedianTimePast() const {
    std::vector<uint64_t> timestamps;
    timestamps.reserve(11);

    const CBlockIndex* pindex = this;
    for (int i = 0; i < 11 && pindex; i++) {
        timestamps.push_back(pindex->timestamp);
        pindex = pindex->pprev;
    }

    // Sort timestamps
    std::sort(timestamps.begin(), timestamps.end());

    // Return median (middle element)
    return timestamps[timestamps.size() / 2];
}

// === Block Index Management ===

CBlockIndex* FindBlockIndex(const uint256& hash) {
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);
    auto it = g_block_index.find(hash);
    return (it != g_block_index.end()) ? it->second.get() : nullptr;
}

CBlockIndex* AddBlockIndex(const BlockHeader& header, uint32_t height) {
    // #353: serialize the entire insert (find-check → pprev link → children
    // push → chainwork → map insert → candidate/orphan bookkeeping) against
    // FindBlockIndex/GetBestCandidate on other threads. Recursive: the nested
    // FindBlockIndex / MarkBlockValid→AddCandidate / OnParentValidated calls
    // below re-enter this lock.
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);

    // Phase M.0: GetHash() already returns uint256, no conversion needed
    uint256 hash = header.GetHash();

    // Check if already exists
    if (FindBlockIndex(hash)) {
        return FindBlockIndex(hash);
    }

    // Create new block index
    auto block_index = std::make_unique<CBlockIndex>(header, height);
    CBlockIndex* raw_ptr = block_index.get();

    // Set parent relationship - Phase M.1: Use uint256 directly
    uint256 prev_hash = !header.prev_block_hash.IsNull() ? header.prev_block_hash : header.prev_block_hash;
    if (!prev_hash.IsNull()) {
        raw_ptr->pprev = FindBlockIndex(prev_hash);
        if (raw_ptr->pprev) {
            raw_ptr->pprev->children.push_back(raw_ptr);
        }
    }

    // Calculate chainwork
    UpdateChainwork(raw_ptr);

    // Store in global index
    g_block_index[hash] = std::move(block_index);

    // ═══════════════════════════════════════════════════════════════════════════
    // INVARIANT: Exactly one CBlockIndex per hash, forever.
    // This is the canonical insertion point. Verify FindBlockIndex returns
    // the same pointer we just inserted.
    // ═══════════════════════════════════════════════════════════════════════════
#ifndef NDEBUG
    assert(FindBlockIndex(hash) == raw_ptr &&
           "ONE BlockIndex per hash invariant violated in AddBlockIndex!");
#endif

    // Header-first sync: check if we can connect immediately
    if (CanConnect(raw_ptr)) {
        // Mark as chain-valid and add to candidates
        MarkBlockValid(raw_ptr, BLOCK_VALID_CHAIN);

        // Process any orphans waiting for this block
        OnParentValidated(raw_ptr);
    } else {
        // Queue as orphan until parent becomes available.
        MaybeQueueOrphan(raw_ptr);
        // Header arrival establishes graph identity even before validation.
        // A child received first otherwise keeps pprev=null while a stored
        // competing branch waits for activation, which rejects it as having
        // no common ancestor. ProcessOrphanQueue links waiting children but
        // retains its CanConnect check before granting any validation status.
        ProcessOrphanQueue(raw_ptr->hash);
    }

    dinero::g_logger.info("Added block: height=" + std::to_string(height) +
                         " hash=" + hash.GetHex().substr(0, 16) + "... work=..." +
                         // #353: chainwork is un-padded (short) when pprev isn't yet
                         // materialized (UpdateChainwork's genesis branch → WorkForBits
                         // only). substr(48,...) then threw "invalid string position",
                         // aborting LoadSnapshot's genesis→base materialization before
                         // StartBackgroundValidation() and deadlocking the snapshot node.
                         // Display-only; guard it (EnsureHeaderBranchIndexed fixes chainwork after).
                         (raw_ptr->chainwork.size() >= 48 ? raw_ptr->chainwork.substr(48, 16)
                                                          : raw_ptr->chainwork));

    return raw_ptr;
}

void UpdateChainwork(CBlockIndex* block_index) {
    if (!block_index) return;
    
    // Calculate work for this block
    std::string block_work = chainwork::WorkForBits(block_index->bits);
    
    // Add parent's chainwork
    if (block_index->pprev) {
        block_index->chainwork = chainwork::AddWork(block_index->pprev->chainwork, block_work);
    } else {
        // Genesis block
        block_index->chainwork = block_work;
    }
}

void AddCandidate(CBlockIndex* block_index) {
    if (!block_index) return;
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);  // #353

    // P0 invariant: single eligibility gate for all candidate paths.
    if (!IsEligibleForCandidacy(block_index->status)) {
        return;
    }

    // Remove parent from candidates (no longer a tip)
    if (block_index->pprev) {
        g_candidates.erase(block_index->pprev);
    }
    
    // Add this block as a new tip
    g_candidates.insert(block_index);
}

void RemoveCandidate(CBlockIndex* block_index) {
    if (!block_index) return;
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);  // #353
    g_candidates.erase(block_index);
}

CBlockIndex* GetBestCandidate() {
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);  // #353
    if (g_candidates.empty()) return nullptr;
    return g_candidates.Best([](CBlockIndex*) { return true; });
}

std::vector<CBlockIndex*> GetCandidateTipsSnapshot() {
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);
    return g_candidates.Snapshot();
}

// === #741: read-only chain-tip enumeration (getchaintips) ===

const char* ChainTipStatusName(ChainTipStatus status) {
    switch (status) {
        case ChainTipStatus::Active:       return "active";
        case ChainTipStatus::ValidFork:    return "valid-fork";
        case ChainTipStatus::ValidHeaders: return "valid-headers";
        case ChainTipStatus::HeadersOnly:  return "headers-only";
        case ChainTipStatus::Invalid:      return "invalid";
    }
    return "unknown";
}

ChainTipStatus ClassifyChainTip(const CBlockIndex* tip, const CBlockIndex* active_tip) {
    if (tip && tip == active_tip) return ChainTipStatus::Active;
    const uint32_t st = tip ? tip->status : 0u;
    if (st & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) return ChainTipStatus::Invalid;
    if (!(st & BLOCK_HAVE_DATA)) return ChainTipStatus::HeadersOnly;
    // BLOCK_VALID_SCRIPTS is stamped by BlockAcceptor once every consensus
    // check has passed and stays set on a block ChainstateService later
    // disconnects, so an abandoned branch reads as valid-fork (Core semantics).
    if (st & BLOCK_VALID_SCRIPTS) return ChainTipStatus::ValidFork;
    return ChainTipStatus::ValidHeaders;
}

std::vector<ChainTipEntry> GetChainTipsSnapshot(const CBlockIndex* active_tip,
                                                uint32_t max_fork_depth) {
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);
    std::vector<ChainTipEntry> out;

    // Always report the active tip, even when a header-only child is already
    // indexed (header-first sync), exactly as Core does.
    if (active_tip) {
        out.push_back(ChainTipEntry{active_tip, ChainTipStatus::Active, 0});
    }

    for (const auto& kv : g_block_index) {
        const CBlockIndex* idx = kv.second.get();
        if (!idx || idx == active_tip || !idx->children.empty()) continue;

        ChainTipEntry entry;
        entry.tip = idx;
        entry.status = ClassifyChainTip(idx, active_tip);

        if (active_tip) {
            // Fork point must lie within max_fork_depth blocks below the active
            // tip; a tip already deeper than that cannot qualify.
            if (active_tip->height > idx->height &&
                active_tip->height - idx->height > max_fork_depth) {
                continue;
            }
            const CBlockIndex* a = active_tip;
            const CBlockIndex* b = idx;
            uint32_t depth = 0;   // how far below the active tip we have walked
            bool bounded = true;
            while (a && a->height > b->height) {
                a = a->pprev;
                if (++depth > max_fork_depth) { bounded = false; break; }
            }
            // O(branchlen): the walk we need to report the length anyway.
            while (bounded && a && b && b->height > a->height) b = b->pprev;
            while (bounded && a && b && a != b) {
                a = a->pprev;
                b = b->pprev;
                if (++depth > max_fork_depth) { bounded = false; break; }
            }
            if (!bounded || !a || !b) continue;   // beyond window, or unlinked orphan
            entry.branchlen = idx->height - a->height;
        }
        out.push_back(entry);
    }

    std::sort(out.begin(), out.end(), [](const ChainTipEntry& x, const ChainTipEntry& y) {
        return ByWorkThenHash{}(x.tip, y.tip);
    });
    return out;
}

// === Header-First Sync Implementation ===

bool MaybeQueueOrphan(CBlockIndex* block_index) {
    if (!block_index) return false;
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);  // #353

    // Genesis blocks can always connect
    if (block_index->IsGenesis()) return false;

    // If parent doesn't exist at all, queue as orphan
    if (!block_index->pprev) {
        uint256 parent_hash = block_index->prev_hash;
        g_orphan_pool[parent_hash].push_back(block_index);

        dinero::g_logger.debug("Queued orphan: " + block_index->hash.GetHex().substr(0, 16) +
                              "... waiting for missing parent " + parent_hash.GetHex().substr(0, 16) + "...");
        return true;
    }

    // If parent exists but is not chain-valid, queue as orphan
    if (!(block_index->pprev->status & BLOCK_VALID_CHAIN)) {
        uint256 parent_hash = block_index->prev_hash;
        g_orphan_pool[parent_hash].push_back(block_index);

        dinero::g_logger.info("Queued orphan: " + block_index->hash.GetHex().substr(0, 16) +
                             "... waiting for parent " + parent_hash.GetHex().substr(0, 16) + "... to be validated");
        return true;
    }

    return false; // Parent is valid, can connect immediately
}

void OnParentValidated(CBlockIndex* parent_index) {
    if (!parent_index) return;
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);  // #353

    uint256 parent_hash = parent_index->hash;
    ProcessOrphanQueue(parent_hash);
}

void ProcessOrphanQueue(const uint256& parent_hash) {
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);  // #353
    auto it = g_orphan_pool.find(parent_hash);
    if (it == g_orphan_pool.end()) {
        dinero::g_logger.debug("No orphans found for parent " + parent_hash.GetHex().substr(0, 16) + "...");
        return;
    }

    std::vector<CBlockIndex*> orphans = std::move(it->second);
    g_orphan_pool.erase(it);

    dinero::g_logger.info("Processing " + std::to_string(orphans.size()) +
                         " orphans for parent " + parent_hash.GetHex().substr(0, 16) + "...");

    // Process orphans in topological order (by height)
    std::sort(orphans.begin(), orphans.end(),
              [](const CBlockIndex* a, const CBlockIndex* b) {
                  return a->height < b->height;
              });

    for (CBlockIndex* orphan : orphans) {
        // If this orphan was inserted before its parent existed, re-link it now.
        // Without this, pprev stays null forever and the orphan can never connect.
        if (!orphan->pprev) {
            CBlockIndex* parent_index = FindBlockIndex(orphan->prev_hash);
            if (parent_index) {
                orphan->pprev = parent_index;
                if (std::find(parent_index->children.begin(),
                              parent_index->children.end(),
                              orphan) == parent_index->children.end()) {
                    parent_index->children.push_back(orphan);
                }
            }
        }

        // Recompute cumulative work after any parent-link update so fork-choice
        // compares the true chainwork of the now-linkable branch.
        if (orphan->pprev) {
            UpdateChainwork(orphan);
        }

        if (CanConnect(orphan)) {
            // Mark as chain-valid and try to add as candidate
            MarkBlockValid(orphan, BLOCK_VALID_CHAIN);

            dinero::g_logger.info("Connected orphan: " + orphan->hash.GetHex().substr(0, 16) +
                                 "... height=" + std::to_string(orphan->height));

            // Process any children of this orphan (cascade)
            dinero::g_logger.debug("Looking for children of " + orphan->hash.GetHex().substr(0, 16) + "...");
            OnParentValidated(orphan);
        } else {
            // Still can't connect, re-queue if parent is missing
            MaybeQueueOrphan(orphan);
        }
    }
}

bool CanConnect(CBlockIndex* block_index) {
    if (!block_index || !block_index->pprev) {
        return block_index && block_index->IsGenesis(); // Genesis can always connect
    }
    
    // Can connect if parent is chain-valid
    return (block_index->pprev->status & BLOCK_VALID_CHAIN) != 0;
}

void MarkBlockValid(CBlockIndex* block_index, uint32_t validation_flags) {
    if (!block_index) return;
    std::lock_guard<std::recursive_mutex> lk(g_block_index_mutex);  // #353

    uint32_t old_status = block_index->status;
    block_index->status |= validation_flags;

    dinero::g_logger.debug("Block " + block_index->hash.GetHex().substr(0, 16) +
                          "... status: " + std::to_string(old_status) +
                          " -> " + std::to_string(block_index->status));

    // If block became chain-valid, add to candidates
    if ((validation_flags & BLOCK_VALID_CHAIN) && !(old_status & BLOCK_VALID_CHAIN)) {
        AddCandidate(block_index);
    }
}

} // namespace dinero
