/**
 * Phase N.0: Header-First Sync - Implementation
 *
 * Header validation and fork-choice WITHOUT requiring full blocks.
 */

#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include "consensus/difficulty.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <iostream>

namespace dinero {
namespace consensus {

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

    if (!prev_hash.IsNull()) {  // Not genesis
        auto parent_it = header_index_.find(prev_hash);
        if (parent_it == header_index_.end()) {
            // Parent not found - can't add this header yet
            std::cerr << "[HeaderChainSelector] ❌ PARENT NOT FOUND for header "
                      << hash.GetHex().substr(0, 16) << "... prev="
                      << prev_hash.GetHex().substr(0, 16) << "..." << std::endl;
            return false;
        }
        parent = parent_it->second.get();
    }

    // Validate header (stateless checks)
    if (!ValidateHeader(header, parent)) {
        return false;
    }

    // Create new header entry
    auto new_entry = std::make_unique<HeaderIndexEntry>(header, parent);
    const HeaderIndexEntry* entry_ptr = new_entry.get();

    // DEBUG: Log chainwork calculation
    std::cerr << "[CHAINWORK] Added header height=" << entry_ptr->height
              << " bits=0x" << std::hex << header.difficulty << std::dec
              << " work=" << entry_ptr->chainwork.GetHex().substr(56)
              << " (parent_work=" << (parent ? parent->chainwork.GetHex().substr(56) : "none") << ")"
              << std::endl;

    // Store in index
    header_index_[hash] = std::move(new_entry);

    // Phase N.1: Persist header to storage
    if (header_store_) {
        header_store_->StoreHeader(*entry_ptr);
    }

    // Update best header if necessary
    UpdateBestHeader(entry_ptr);

    // Phase N.1: Persist best header if changed
    if (header_store_ && best_header_ == entry_ptr) {
        header_store_->StoreBestHeader(best_header_->hash);
    }

    return true;
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

    // For non-genesis blocks, check timestamp is not before parent
    // Bitcoin allows timestamps up to 2 hours in the future and doesn't require
    // strictly increasing timestamps - only that they're >= median of past 11 blocks
    // For simplicity, we just check timestamp >= parent (not strictly greater)
    if (prev != nullptr) {
        if (header.timestamp < prev->header.timestamp) {
            // Timestamp cannot be before parent
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

    // 4. PoW validity - simplified check
    // In production, this would validate hash < target
    // For now, just verify hash exists (non-null)
    uint256 hash = header.GetHash();
    if (hash.IsNull()) {
        return false;
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

    // Compare chainwork - LOUD DEBUG
    printf("\n=== CHAINWORK COMPARE ===\n");
    printf("  candidate: height=%u hash=%s\n", new_entry->height, new_entry->hash.GetHex().substr(0,16).c_str());
    printf("  candidate chainwork: %s\n", new_entry->chainwork.GetHex().c_str());
    printf("  best:      height=%u hash=%s\n", best_header_->height, best_header_->hash.GetHex().substr(0,16).c_str());
    printf("  best chainwork:      %s\n", best_header_->chainwork.GetHex().c_str());
    printf("  comparison: candidate %s best\n",
           (new_entry->chainwork > best_header_->chainwork) ? ">" :
           (new_entry->chainwork == best_header_->chainwork) ? "==" : "<");
    printf("=========================\n\n");
    fflush(stdout);

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
