/**
 * Phase N.0: Header-First Sync - Implementation
 *
 * Header validation and fork-choice WITHOUT requiring full blocks.
 */

#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include "consensus/difficulty.h"
#include "consensus/pow.h"
#include "consensus/pow.hpp"          // GetNextWorkRequiredForCandidate (shared expected-bits fn)
#include "consensus/pow_context.h"    // GetConsensusForCurrentNetwork, NoChainDb
#include "consensus/chainparams.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <iostream>

namespace dinero {
namespace consensus {

// 4d-2 (issue #181): maximum number of stored headers NOT on the active best
// chain (losing/side forks). Bounds a low-work header-flood DoS while leaving
// ample headroom for any realistic reorg (network reorgs are shallow; this
// allows a ~10000-header losing fork — far deeper than anything legitimate).
// The active chain and the AssumeUTXO/replay anchor on it are never counted or
// evicted. ~10000 entries ≈ a few MB of bounded storage. When the budget is
// full, a NEW side-branch header is admitted only by evicting a strictly
// lower-work side-branch tip (work-aware), so a higher-work reorg always wins
// even through a full budget; a header with no more work than what we already
// keep is refused.
static constexpr size_t MAX_SIDE_BRANCH_HEADERS = 10000;

// ============================================================================
// HeaderIndexEntry Implementation
// ============================================================================

HeaderIndexEntry::HeaderIndexEntry(
    const BlockHeader& hdr,
    const HeaderIndexEntry* prev_entry
)
    : header(hdr)
    , parent(prev_entry)
{
    // Compute block hash from header (Phase M.0: uint256 identity)
    hash = hdr.GetHash();

    // Extract prev_hash from header (Phase M.0: already uint256)
    prev_hash = hdr.prev_block_hash;

    // Compute height and chainwork
    // Work from header = 2^256 / (target + 1)
    arith_uint256 header_work = GetBlockProof(hdr.difficulty);

    if (prev_entry == nullptr) {
        // Genesis block: height 0, chainwork = just its own proof
        height = 0;
        chainwork = header_work;
    } else {
        // Non-genesis: height = parent + 1, chainwork = parent + this block
        height = prev_entry->height + 1;
        chainwork = prev_entry->chainwork + header_work;
    }
}

const HeaderIndexEntry* HeaderIndexEntry::GetAncestor(uint32_t ancestor_height) const {
    if (ancestor_height > height) {
        return nullptr;  // Can't get ancestor higher than current height
    }

    if (ancestor_height == height) {
        return this;
    }

    // Walk backwards through parent pointers
    const HeaderIndexEntry* current = this;
    while (current && current->height > ancestor_height) {
        current = current->parent;
    }

    return (current && current->height == ancestor_height) ? current : nullptr;
}

uint32_t HeaderIndexEntry::GetMedianTimePast() const {
    // BIP113: Median Time Past is the median of the last 11 block timestamps
    // For blocks on competing forks, we walk THIS header's parent chain

    constexpr int MTP_BLOCKS = 11;
    std::vector<uint32_t> timestamps;
    timestamps.reserve(MTP_BLOCKS);

    // Collect timestamps from this header's ancestors (including this header)
    const HeaderIndexEntry* current = this;
    for (int i = 0; i < MTP_BLOCKS && current != nullptr; ++i) {
        timestamps.push_back(current->header.timestamp);
        current = current->parent;
    }

    // If we have no timestamps (shouldn't happen), return 0
    if (timestamps.empty()) {
        return 0;
    }

    // Sort and return median
    std::sort(timestamps.begin(), timestamps.end());
    return timestamps[timestamps.size() / 2];
}

// ============================================================================
// HeaderChainSelector Implementation
// ============================================================================

HeaderChainSelector::HeaderChainSelector()
    : best_header_(nullptr)
    , header_store_(nullptr)
{
}

HeaderChainSelector::HeaderChainSelector(HeaderStore* store)
    : best_header_(nullptr)
    , header_store_(store)
{
    // Phase N.1: Load headers from storage on construction
    if (header_store_) {
        LoadFromStorage();
    }
}

HeaderChainSelector::~HeaderChainSelector() {
    // unique_ptr handles cleanup automatically
    // HeaderStore is not owned, so we don't delete it
}

bool HeaderChainSelector::AddHeader(const BlockHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Compute hash for lookup (Phase M.0: uint256 identity)
    uint256 hash = header.GetHash();

    // Check if already exists
    if (header_index_.find(hash) != header_index_.end()) {
        // Header exists, but ensure best_header_ is up to date.
        // After LoadFromStorage() with a stale persisted best marker,
        // existing fork headers may have more chainwork than best_header_.
        UpdateBestHeader(header_index_[hash].get());
        return true;
    }

    // Find parent (Phase M.0: prevBlockHash is already uint256)
    uint256 prev_hash = header.prev_block_hash;
    const HeaderIndexEntry* parent = nullptr;

    HeaderIndexEntry* parent_mut = nullptr;  // non-const handle for child_count
    if (!prev_hash.IsNull()) {  // Not genesis
        auto parent_it = header_index_.find(prev_hash);
        if (parent_it == header_index_.end()) {
            // Parent not found - can't add this header yet
            std::cerr << "[HeaderChainSelector] ❌ PARENT NOT FOUND for header "
                      << hash.GetHex().substr(0, 16) << "... prev="
                      << prev_hash.GetHex().substr(0, 16) << "..." << std::endl;
            return false;
        }
        parent_mut = parent_it->second.get();
        parent = parent_mut;
    }

    // Validate header (stateless checks)
    if (!ValidateHeader(header, parent)) {
        return false;
    }

    // Create new header entry (computes height + chainwork)
    auto new_entry = std::make_unique<HeaderIndexEntry>(header, parent);
    const HeaderIndexEntry* entry_ptr = new_entry.get();

    // 4d-2: RESERVE this header's slot in its parent BEFORE the eviction logic
    // runs. This bumps the parent's child_count and removes it from the
    // eviction-eligible tip set, which is load-bearing for safety: when the
    // incoming header EXTENDS a side-branch tip P, P would otherwise be (or be
    // pruned as) the lowest-work tip and `EvictBranch` would free this header's
    // own parent — a use-after-free. With the reservation, P has child_count >= 1
    // so it can neither be selected as min_tip nor be pruned away beneath us. The
    // reservation is undone on the refuse path below.
    if (parent_mut != nullptr) {
        parent_mut->child_count++;
        RefreshTipStatus(parent_mut);
    }

    // 4d-2 (issue #181): bounded side-branch storage with work-aware eviction.
    //
    // A header that would become the new best tip (more work, or an exact-tie the
    // fork-choice in UpdateBestHeader would pick) is the active chain and is never
    // capped. Otherwise it is a losing side branch: once the side-branch budget is
    // full we admit it ONLY by evicting a strictly lower-work side-branch tip, so
    // a higher-work reorg always wins even through a full budget of low-work junk,
    // while equal-or-lower-work flood headers are refused. The active best chain
    // (and the AssumeUTXO/replay anchor on it) is never an eviction candidate.
    if (best_header_ != nullptr) {
        const bool would_be_best =
            (entry_ptr->chainwork > best_header_->chainwork) ||
            (entry_ptr->chainwork == best_header_->chainwork &&
             entry_ptr->hash < best_header_->hash);
        if (!would_be_best) {
            const size_t active_len = static_cast<size_t>(best_header_->height) + 1;
            const size_t side_count =
                header_index_.size() > active_len ? header_index_.size() - active_len : 0;
            if (side_count >= MAX_SIDE_BRANCH_HEADERS) {
                const HeaderIndexEntry* min_tip =
                    evictable_tips_.empty() ? nullptr : *evictable_tips_.begin();
                if (min_tip == nullptr ||
                    !(entry_ptr->chainwork > min_tip->chainwork)) {
                    // No lower-work tip to displace — this header cannot belong to
                    // a chain that beats what we already store. Undo the parent
                    // reservation and refuse.
                    if (parent_mut != nullptr) {
                        parent_mut->child_count--;
                        RefreshTipStatus(parent_mut);
                    }
                    if (!side_branch_cap_warned_) {
                        std::cerr << "[HeaderChainSelector] side-branch budget full ("
                                  << side_count << " >= " << MAX_SIDE_BRANCH_HEADERS
                                  << ") — refusing low-work fork headers (first: "
                                  << hash.GetHex().substr(0, 16) << "...)" << std::endl;
                        side_branch_cap_warned_ = true;
                    }
                    return false;  // new_entry discarded; best chain untouched
                }
                // Newcomer outranks the lowest-work tip: evict that losing branch
                // to make room, then admit the newcomer below. The parent
                // reservation guarantees EvictBranch cannot touch our parent.
                EvictBranch(min_tip);
                side_branch_cap_warned_ = false;
            } else {
                side_branch_cap_warned_ = false;
            }
        }
    }

    // Store in index (parent's child_count was already incremented above).
    header_index_[hash] = std::move(new_entry);

    // Phase N.1: Persist header to storage
    if (header_store_) {
        header_store_->StoreHeader(*entry_ptr);
    }

    // Update best header if necessary
    const HeaderIndexEntry* old_best = best_header_;
    UpdateBestHeader(entry_ptr);

    // Reconcile tip status for exactly the entries whose tip/best state can change
    // in one AddHeader: the new entry, and (on a reorg) the demoted old best tip.
    RefreshTipStatus(entry_ptr);
    if (old_best != nullptr && old_best != best_header_) {
        RefreshTipStatus(old_best);
    }

    // Phase N.1: Persist best header if changed
    if (header_store_ && best_header_ == entry_ptr) {
        header_store_->StoreBestHeader(best_header_->hash);
    }

    return true;
}

void HeaderChainSelector::RefreshTipStatus(const HeaderIndexEntry* entry) {
    // Invariant: entry ∈ evictable_tips_  iff  (child_count == 0 && entry != best_header_).
    // (The only childless best-chain entry is best_header_ itself — every other
    // best-chain header has the next best-chain header as a child.)
    if (entry == nullptr) {
        return;
    }
    const bool should_be_tip = (entry->child_count == 0) && (entry != best_header_);
    auto it = evictable_tips_.find(entry);
    const bool present = (it != evictable_tips_.end());
    if (should_be_tip && !present) {
        evictable_tips_.insert(entry);
    } else if (!should_be_tip && present) {
        evictable_tips_.erase(it);
    }
}

void HeaderChainSelector::EvictBranch(const HeaderIndexEntry* tip) {
    // Prune a losing side branch from `tip` upward, removing each header while it
    // is childless and not the best tip, stopping at the fork point (an ancestor
    // that still has another stored child) or at best_header_. Never removes a
    // best-chain header: best-chain ancestors all have a best-chain child
    // (child_count >= 1), so the walk stops there; best_header_ is excluded
    // explicitly. Re-looks-up by hash each step so no freed pointer is touched.
    if (tip == nullptr) {
        return;
    }
    uint256 cur_hash = tip->hash;
    while (true) {
        auto it = header_index_.find(cur_hash);
        if (it == header_index_.end()) {
            break;
        }
        HeaderIndexEntry* cur = it->second.get();
        if (cur == best_header_) {
            break;  // never evict the best tip
        }
        if (cur->child_count != 0) {
            break;  // reached a fork point with a surviving branch — stop
        }
        const bool has_parent = (cur->parent != nullptr);
        const uint256 parent_hash = has_parent ? cur->parent->hash : uint256();

        // Remove from the tip set + disk, then erase from the index (frees cur).
        auto tip_it = evictable_tips_.find(cur);
        if (tip_it != evictable_tips_.end()) {
            evictable_tips_.erase(tip_it);
        }
        if (header_store_) {
            header_store_->DeleteHeader(cur_hash);
        }
        header_index_.erase(it);

        if (!has_parent) {
            break;  // reached genesis
        }
        auto parent_it = header_index_.find(parent_hash);
        if (parent_it == header_index_.end()) {
            break;
        }
        HeaderIndexEntry* parent = parent_it->second.get();
        if (parent->child_count > 0) {
            parent->child_count--;
        }
        // Continue upward: if the parent is now a childless non-best header it is
        // part of the same losing branch and will be pruned next iteration; if it
        // still has children or is best_header_, the loop stops there.
        cur_hash = parent_hash;
    }
}

const HeaderIndexEntry* HeaderChainSelector::GetBestHeader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return best_header_;
}

const HeaderIndexEntry* HeaderChainSelector::GetHeader(const uint256& hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = header_index_.find(hash);
    if (it == header_index_.end()) {
        return nullptr;
    }
    return it->second.get();
}

bool HeaderChainSelector::GetBestHeaderCopy(HeaderIndexEntry& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!best_header_) {
        return false;
    }
    out = *best_header_;
    // Same rule as GetHeaderCopy: a copy never carries the parent pointer out.
    out.parent = nullptr;
    return true;
}

std::vector<uint256> HeaderChainSelector::BuildLocatorCopy(size_t max_entries) const {
    std::vector<uint256> locator;
    std::lock_guard<std::mutex> lock(mutex_);

    if (!best_header_ || max_entries == 0) {
        return locator;
    }

    // Mirrors the previous GetHeaderLocator() walk exactly, but every step
    // resolves under THIS lock, so no pointer escapes and the whole locator
    // reflects one consistent chain state (#441).
    locator.reserve(max_entries);

    const HeaderIndexEntry* current = best_header_;
    uint32_t height = best_header_->height;
    uint32_t step = 1;

    while (current != nullptr && locator.size() < max_entries) {
        locator.push_back(current->hash);

        if (height < step) {
            break;
        }
        height -= step;

        // Same resolution GetHeaderAtHeight() performs, but inline and still
        // holding the lock.
        current = best_header_->GetAncestor(height);

        if (locator.size() > 1) {
            step *= 2;
        }
    }

    return locator;
}

bool HeaderChainSelector::GetHeaderCopy(const uint256& hash, HeaderIndexEntry& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = header_index_.find(hash);
    if (it == header_index_.end()) {
        return false;
    }
    out = *it->second;
    // The parent pointer is only valid under this lock (EvictBranch can free
    // the parent entry); never let a copy carry it out.
    out.parent = nullptr;
    return true;
}

bool HeaderChainSelector::CollectAncestorsByHash(
        const uint256& anchor_hash,
        uint32_t start_height,
        uint32_t& anchor_height_out,
        std::vector<std::pair<uint256, uint32_t>>& out_ascending) const {
    // Resolve + walk + copy entirely under mutex_: EvictBranch (also under
    // mutex_) can free side-branch entries, so neither the anchor pointer nor
    // its parent links may escape this critical section.
    std::lock_guard<std::mutex> lock(mutex_);
    out_ascending.clear();

    auto it = header_index_.find(anchor_hash);
    if (it == header_index_.end()) {
        return false;
    }
    const HeaderIndexEntry* anchor = it->second.get();
    anchor_height_out = anchor->height;
    if (start_height > anchor->height) {
        return true;  // anchor known, range degenerate — caller decides
    }

    out_ascending.reserve(anchor->height - start_height + 1);
    for (const HeaderIndexEntry* e = anchor; e && e->height >= start_height;
         e = e->parent) {
        out_ascending.emplace_back(e->hash, e->height);
        if (e->height == start_height) {
            break;  // height-0 guard: `e->height >= 0` is never false (unsigned)
        }
    }
    std::reverse(out_ascending.begin(), out_ascending.end());
    return true;
}

const HeaderIndexEntry* HeaderChainSelector::GetHeaderAtHeight(uint32_t height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!best_header_) {
        return nullptr;
    }

    // Walk back from best header to requested height
    return best_header_->GetAncestor(height);
}

const HeaderIndexEntry* HeaderChainSelector::FindForkPoint(
    const HeaderIndexEntry* a,
    const HeaderIndexEntry* b
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!a || !b) {
        return nullptr;
    }

    // Bring both to same height
    while (a->height > b->height) {
        a = a->parent;
        if (!a) return nullptr;
    }

    while (b->height > a->height) {
        b = b->parent;
        if (!b) return nullptr;
    }

    // Walk back together until we find common ancestor
    while (a != b) {
        a = a->parent;
        b = b->parent;
        if (!a || !b) return nullptr;
    }

    return a;  // Common ancestor
}

void HeaderChainSelector::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    header_index_.clear();
    best_header_ = nullptr;
    evictable_tips_.clear();          // 4d-2: runtime tip set
    side_branch_cap_warned_ = false;

    // Phase N.1: Clear storage if present
    if (header_store_) {
        header_store_->ClearAll();
    }
}

bool HeaderChainSelector::LoadFromStorage() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!header_store_) {
        return false;
    }

    // Phase N.1: Load all headers from storage
    std::vector<HeaderIndexEntry> stored_headers;
    if (!header_store_->LoadAllHeaders(stored_headers)) {
        return false;
    }

    // Clear current state
    header_index_.clear();
    best_header_ = nullptr;

    // Load all headers first (without parent pointers)
    for (const auto& entry : stored_headers) {
        auto new_entry = std::make_unique<HeaderIndexEntry>(entry);
        new_entry->parent = nullptr;  // Will be set in second pass
        header_index_[entry.hash] = std::move(new_entry);
    }

    // Second pass: rebuild parent pointers
    for (auto& pair : header_index_) {
        HeaderIndexEntry* entry = pair.second.get();

        if (!entry->prev_hash.IsNull()) {
            // Find parent
            auto parent_it = header_index_.find(entry->prev_hash);
            if (parent_it != header_index_.end()) {
                entry->parent = parent_it->second.get();
            }
        }
    }

    // Restore best header
    uint256 best_hash;
    if (header_store_->LoadBestHeader(best_hash)) {
        auto best_it = header_index_.find(best_hash);
        if (best_it != header_index_.end()) {
            best_header_ = best_it->second.get();
        }
    }

    // Always recalculate best header from chainwork — the persisted marker
    // may be stale if fork headers were added after it was last saved.
    if (!header_index_.empty()) {
        best_header_ = nullptr;  // Reset to force full scan
        for (const auto& pair : header_index_) {
            UpdateBestHeader(pair.second.get());
        }
    }

    // 4d-2 (issue #181): rebuild the runtime child_count + eviction-eligible tip
    // set (neither is persisted), then enforce the side-branch budget so a
    // persisted flood is bounded the same way as the live path. Order matters:
    // parent pointers (above) → child_count → best_header_ (above) → tips → trim.
    for (auto& pair : header_index_) {
        pair.second->child_count = 0;
    }
    for (auto& pair : header_index_) {
        const HeaderIndexEntry* e = pair.second.get();
        if (e->parent != nullptr) {
            // Entries are owned non-const; parent is exposed const for callers.
            const_cast<HeaderIndexEntry*>(e->parent)->child_count++;
        }
    }
    evictable_tips_.clear();
    side_branch_cap_warned_ = false;
    for (const auto& pair : header_index_) {
        const HeaderIndexEntry* e = pair.second.get();
        if (e->child_count == 0 && e != best_header_) {
            evictable_tips_.insert(e);
        }
    }
    // Trim a persisted over-budget store down to the cap by evicting the
    // lowest-work tips (reuses the live-path eviction so behavior matches). A
    // correctly written store is already within budget (we DeleteHeader on
    // eviction), so this is defensive.
    if (best_header_ != nullptr) {
        const size_t active_len = static_cast<size_t>(best_header_->height) + 1;
        while (!evictable_tips_.empty() &&
               header_index_.size() > active_len + MAX_SIDE_BRANCH_HEADERS) {
            EvictBranch(*evictable_tips_.begin());
        }
    }

    return true;
}

bool HeaderChainSelector::SaveBestHeader() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!header_store_ || !best_header_) {
        return false;
    }

    return header_store_->StoreBestHeader(best_header_->hash);
}

size_t HeaderChainSelector::GetHeaderCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return header_index_.size();
}

// ============================================================================
// Private Methods
// ============================================================================

bool HeaderChainSelector::ValidateHeader(
    const BlockHeader& header,
    const HeaderIndexEntry* prev
) {
    // Phase N.1: Stateless header validation only

    // 1. Version sanity
    if (header.version < 1) {
        return false;
    }

    // 2. Timestamp rules
    if (header.timestamp == 0) {
        return false;
    }

    // For non-genesis blocks, enforce the same header-level time rule the
    // active chain accepts: a timestamp must be greater than median-time-past.
    // It does not have to be monotonic relative to the direct parent.
    if (prev != nullptr) {
        if (header.timestamp <= prev->GetMedianTimePast()) {
            return false;
        }
    }

    // 3. Difficulty target validation (if not genesis)
    if (prev != nullptr) {
        // In production, this would validate difficulty adjustment
        // For now, just check bits field is non-zero
        if (header.difficulty == 0) {
            return false;
        }
    }

    // 4. Proof-of-work validity — the header hash must meet the claimed target.
    //
    // SECURITY (header-PoW verification): without this, a peer can submit headers
    // claiming arbitrarily hard difficulty bits (hence arbitrarily large
    // GetBlockProof() chainwork) backed by NO real work, win fork-choice in
    // UpdateBestHeader(), and steer block download toward a forged chain — a
    // zero-cost sync-stall / eclipse vector. Full blocks are still rejected at
    // connect (block_acceptor), so this is availability, not theft; this check
    // closes the header-level hole and restores the "chainwork is lower-bounded
    // by real work" invariant fork-choice relies on.
    //
    // Regtest policy mirrors block_acceptor's PATH A exactly: regtest blocks are
    // mined deterministically (nonce=0, instant) and skip PoW at connect, so we
    // must skip it here too — otherwise a header for a block the node *would*
    // accept could fail header validation (startup-replay divergence / regtest
    // breakage). On mainnet/testnet, enforce hash <= target.
    //
    // require_standard=FALSE is deliberate and load-bearing. The live block-connect
    // PoW gate (pow_consensus_engine: CheckProofOfWork(blockHash, bits)) verifies
    // hash <= target but does NOT call CheckDifficultyBits. The ASERT schedule
    // legitimately eased early-block difficulty below MAX_BITS (0x1d31ffce) — e.g.
    // mainnet block 1 has bits 0x1E00C7FF, an easier target than MAX_BITS — so
    // CheckDifficultyBits()/require_standard=true would REJECT real historical
    // headers (difficulty<1) that block-connect accepted, bricking header sync and
    // startup replay. require_standard=false keeps the real hash <= target check
    // (NOT a stub) while dropping the min-difficulty floor block-connect never
    // imposed. Soundness is preserved: hash <= target still lower-bounds chainwork
    // by real work, and the ASERT check below pins bits to the exact required value
    // (a stronger constraint than any floor).
    uint256 hash = header.GetHash();
    if (hash.IsNull()) {
        return false;
    }
    if (Params().name != "regtest") {
        if (!CheckProofOfWork(header, /*require_standard=*/false)) {
            return false;
        }
    }

    // 4b. Expected difficulty (ASERT schedule) — defense-in-depth.
    //
    // Mirrors block_acceptor's bad-diffbits check at the header level, using the
    // SAME shared computation (GetNextWorkRequiredForCandidate) so header
    // acceptance and block connect can never drift on the difficulty rule. A
    // header whose claimed bits != the bits required by the ASERT schedule for
    // its height is rejected before its (claimed) chainwork is credited.
    //
    // Computed from THIS header's own parent (`prev`) — prev->GetMedianTimePast()
    // walks prev's own ancestry, so side branches validate against their own
    // anchor context, not the active tip.
    //
    // Gating: skip genesis (prev == nullptr; handled by the PoW check above) and
    // skip regtest (block_acceptor PATH A skips difficulty there too — same-rule).
    // expected == 0 means "uncomputable" (pre-ASERT height / missing context):
    // skip rather than reject, so honest persisted headers replay cleanly at
    // startup and block_acceptor remains the backstop. Compact bits are compared
    // for equality against the canonical encoding (never ordered numerically).
    if (prev != nullptr && Params().name != "regtest") {
        const Consensus consensus = GetConsensusForCurrentNetwork();
        const uint32_t expected_bits = GetNextWorkRequiredForCandidate(
            static_cast<int32_t>(prev->height) + 1,
            static_cast<int64_t>(header.timestamp),
            consensus,
            /*parent_index=*/static_cast<const CBlockIndex*>(nullptr),
            /*parent_entry=*/prev,
            /*chain_db=*/static_cast<dinero::NoChainDb*>(nullptr));
        if (expected_bits != 0 && header.difficulty != expected_bits) {
            std::cerr << "[HeaderChainSelector] ❌ bad-diffbits-header at height "
                      << (prev->height + 1) << ": header has "
                      << std::hex << header.difficulty << ", required "
                      << expected_bits << std::dec
                      << " (hash " << hash.GetHex().substr(0, 16) << "...)"
                      << std::endl;
            return false;
        }
    }

    // 5. Linkage - prev_hash must match parent (already checked in AddHeader)

    // ❌ NOT validated here:
    // - Merkle root (requires transactions)
    // - UTXO validity
    // - Transaction rules

    return true;
}

void HeaderChainSelector::UpdateBestHeader(const HeaderIndexEntry* new_entry) {
    if (!new_entry) {
        return;
    }

    // Fork-choice rules (Phase N.2):
    // 1. Max chainwork wins
    // 2. Height is informational only
    // 3. Ties break deterministically by hash

    if (!best_header_) {
        // First header
        std::cerr << "[HeaderChainSelector] 🏆 FIRST best_header: height=" << new_entry->height
                  << " hash=" << new_entry->hash.GetHex().substr(0, 16) << "..." << std::endl;
        best_header_ = new_entry;
        return;
    }

    if (new_entry->chainwork > best_header_->chainwork) {
        // New chain has more work
        std::cerr << "[HeaderChainSelector] 🏆 NEW BEST: height=" << new_entry->height
                  << " beats " << best_header_->height << std::endl;
        best_header_ = new_entry;
    } else if (new_entry->chainwork == best_header_->chainwork) {
        // Tie-breaker: lower hash wins (deterministic)
        if (new_entry->hash < best_header_->hash) {
            best_header_ = new_entry;
        }
    }
    // else: current best remains
}

arith_uint256 HeaderChainSelector::ComputeChainwork(
    const BlockHeader& header,
    const arith_uint256& parent_chainwork
) {
    arith_uint256 header_work = GetBlockProof(header.difficulty);
    return parent_chainwork + header_work;
}

} // namespace consensus
} // namespace dinero
