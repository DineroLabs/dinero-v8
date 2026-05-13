#include <queue>
#include "daemon/tx_mempool.h"
#include "common/logger.h"
#include "crypto/dinero_crypto_minimal.h"
#include <algorithm>
#include <chrono>
#include <queue>
#include <fstream>
#include <sstream>

namespace dinero {

// TxMempoolEntry implementation

TxMempoolEntry::TxMempoolEntry(const Transaction& tx, uint64_t fee, int64_t time)
    : tx(tx), fee(fee), time(time) {
    
    // Calculate basic metrics
    txid = tx.GetTxId();
    size = GetSize();
    vsize = GetVSize();
    weight = GetWeight();
    
    // Calculate fee rate
    feerate = static_cast<double>(fee) / vsize;
    
    // Initialize counters
    ancestor_count = 1;  // Include self
    ancestor_size = vsize;
    ancestor_fees = fee;
    descendant_count = 1;  // Include self
    descendant_size = vsize;
    descendant_fees = fee;
    
    // Check for RBF signaling (BIP 125)
    rbf_enabled = false;
    for (const auto& input : tx.vin) {
        if (input.sequence < 0xfffffffe) {
            rbf_enabled = true;
            break;
        }
    }
    
    is_replacement = false;
    height = 0;  // Will be set when added to mempool
}

uint64_t TxMempoolEntry::GetSize() const {
    // Calculate transaction size based on serialized data
    // For now, estimate based on inputs and outputs
    uint64_t estimated_size = 4; // version
    estimated_size += 1; // input count (assuming < 253)
    estimated_size += tx.vin.size() * 41; // Each input: 36 bytes outpoint + 1 byte script length + 4 bytes sequence
    estimated_size += 1; // output count
    estimated_size += tx.vout.size() * 9; // Each output: 8 bytes value + 1 byte script length (minimum)
    estimated_size += 4; // locktime
    
    // Add script sizes
    for (const auto& input : tx.vin) {
        estimated_size += input.scriptSig.length() / 2; // Hex string to bytes
    }
    for (const auto& output : tx.vout) {
        estimated_size += output.scriptPubKey.length() / 2; // Hex string to bytes
    }
    
    return estimated_size;
}

double TxMempoolEntry::GetAncestorScore() const {
    // Calculate mining priority score based on ancestor fee rate
    if (ancestor_size == 0) return 0.0;
    return static_cast<double>(ancestor_fees) / ancestor_size;
}

double TxMempoolEntry::GetModifiedFeeRate() const {
    // For now, just return the base fee rate
    // Could be modified for priority adjustments
    return feerate;
}

// TxMempool implementation

TxMempool::TxMempool(const MemPoolPolicy& policy) : policy_(policy) {
    stats_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool TxMempool::Exists(const std::string& txid) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return entries_.find(txid) != entries_.end();
}

bool TxMempool::AddUnchecked(const TxMempoolEntry& entry) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Check if already exists
    if (entries_.find(entry.txid) != entries_.end()) {
        return false;
    }
    
    // Check size limits
    if (stats_.total_bytes + entry.vsize > policy_.max_size_bytes) {
        // Try to evict some transactions
        auto evicted = SelectEvictionCandidates(entry.vsize);
        for (const auto& txid : evicted) {
            Remove(txid, "evicted");
        }
        
        // Check again after eviction
        if (stats_.total_bytes + entry.vsize > policy_.max_size_bytes) {
            return false;
        }
    }
    
    // Add to main storage
    auto result = entries_.emplace(entry.txid, entry);
    if (!result.second) {
        return false;
    }
    
    // Update indexes
    UpdateIndexes(entry, true);
    
    // Update statistics
    stats_.tx_count++;
    stats_.total_bytes += entry.vsize;
    stats_.total_fees += entry.fee;
    stats_.accepts_total++;
    stats_.avg_feerate = static_cast<double>(stats_.total_fees) / stats_.total_bytes;
    stats_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    dinero::g_logger.debug("Added transaction to mempool: " + entry.txid + 
                          " (fee: " + std::to_string(entry.fee) + 
                          ", feerate: " + std::to_string(entry.feerate) + ")");
    
    return true;
}

bool TxMempool::Remove(const std::string& txid, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = entries_.find(txid);
    if (it == entries_.end()) {
        return false;
    }
    
    const auto& entry = it->second;
    
    // Update indexes
    UpdateIndexes(entry, false);
    
    // Update statistics
    stats_.tx_count--;
    stats_.total_bytes -= entry.vsize;
    stats_.total_fees -= entry.fee;
    if (stats_.total_bytes > 0) {
        stats_.avg_feerate = static_cast<double>(stats_.total_fees) / stats_.total_bytes;
    } else {
        stats_.avg_feerate = 0.0;
    }
    stats_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Remove from main storage
    entries_.erase(it);
    
    if (!reason.empty()) {
        dinero::g_logger.debug("Removed transaction from mempool: " + txid + " (reason: " + reason + ")");
    }
    
    return true;
}

void TxMempool::Clear() {
    std::lock_guard<std::mutex> lock(mtx_);

    entries_.clear();
    feerate_index_.clear();
    ancestor_index_.clear();
    orphans_.clear();
    orphan_children_.clear();  // Fixed: orphan_peers_ doesn't exist, should be orphan_children_

    // Reset statistics
    stats_ = Stats{};
    stats_.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    dinero::g_logger.info("Cleared mempool");
}

const TxMempoolEntry* TxMempool::Get(const std::string& txid) const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = entries_.find(txid);
    if (it != entries_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<std::string> TxMempool::GetTxIds() const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::vector<std::string> txids;
    txids.reserve(entries_.size());
    
    for (const auto& pair : entries_) {
        txids.push_back(pair.first);
    }
    
    return txids;
}

std::vector<TxMempoolEntry> TxMempool::GetEntries() const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::vector<TxMempoolEntry> entries;
    entries.reserve(entries_.size());
    
    for (const auto& pair : entries_) {
        entries.push_back(pair.second);
    }
    
    return entries;
}

std::vector<TxMempoolEntry> TxMempool::GetEntriesByFeeRate(bool descending) const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::vector<TxMempoolEntry> entries;
    entries.reserve(entries_.size());
    
    if (descending) {
        // Iterate from highest to lowest fee rate
        for (auto it = feerate_index_.rbegin(); it != feerate_index_.rend(); ++it) {
            auto entry_it = entries_.find(it->second);
            if (entry_it != entries_.end()) {
                entries.push_back(entry_it->second);
            }
        }
    } else {
        // Iterate from lowest to highest fee rate
        for (const auto& pair : feerate_index_) {
            auto entry_it = entries_.find(pair.second);
            if (entry_it != entries_.end()) {
                entries.push_back(entry_it->second);
            }
        }
    }
    
    return entries;
}

std::vector<TxMempoolEntry> TxMempool::GetEntriesByAncestorScore(bool descending) const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::vector<TxMempoolEntry> entries;
    entries.reserve(entries_.size());
    
    if (descending) {
        // Iterate from highest to lowest ancestor score
        for (auto it = ancestor_index_.rbegin(); it != ancestor_index_.rend(); ++it) {
            auto entry_it = entries_.find(it->second);
            if (entry_it != entries_.end()) {
                entries.push_back(entry_it->second);
            }
        }
    } else {
        // Iterate from lowest to highest ancestor score
        for (const auto& pair : ancestor_index_) {
            auto entry_it = entries_.find(pair.second);
            if (entry_it != entries_.end()) {
                entries.push_back(entry_it->second);
            }
        }
    }
    
    return entries;
}

size_t TxMempool::Size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return entries_.size();
}

uint64_t TxMempool::Bytes() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_.total_bytes;
}

uint64_t TxMempool::GetTotalFees() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_.total_fees;
}

double TxMempool::GetAverageFeeRate() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_.avg_feerate;
}

bool TxMempool::CheckLimits(const TxMempoolEntry& entry) const {
    // Check transaction size
    if (entry.vsize > 100000) {  // 100KB max transaction size
        return false;
    }
    
    // Check fee rate
    if (entry.feerate < static_cast<double>(policy_.min_relay_feerate) / 1000.0) {
        return false;
    }
    
    // Check if mempool would exceed size limit
    if (stats_.total_bytes + entry.vsize > policy_.max_size_bytes) {
        return false;
    }
    
    return true;
}

std::vector<std::string> TxMempool::EvictForSpace(uint64_t needed_bytes) {
    // This is called with lock already held
    return SelectEvictionCandidates(needed_bytes);
}

bool TxMempool::AddOrphan(const Transaction& tx, const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    std::string txid = tx.GetTxId();

    // Don't add if already exists
    if (orphans_.find(txid) != orphans_.end()) {
        return false;
    }

    // Calculate depth for this orphan
    uint32_t depth = CalculateOrphanDepth(tx);

    // Reject if depth exceeds limit (prevents DoS via deep orphan chains)
    if (depth > policy_.max_orphan_depth) {
        dinero::g_logger.warning("Rejected orphan " + txid + " - exceeds max depth: " +
                                std::to_string(depth) + " > " + std::to_string(policy_.max_orphan_depth));
        return false;
    }

    // Check orphan limits
    if (orphans_.size() >= policy_.max_orphans) {
        EvictExpiredOrphans();  // Try evicting expired first
        if (orphans_.size() >= policy_.max_orphans) {
            LimitOrphans();  // Evict oldest if still over limit
        }
    }

    // Check if orphan pool would exceed size limit
    TxMempoolEntry temp_entry(tx, 0, 0);
    uint64_t orphan_size = temp_entry.GetSize();
    if (stats_.orphan_bytes + orphan_size > policy_.max_orphan_size) {
        dinero::g_logger.warning("Orphan pool size limit reached");
        return false;
    }

    // Create orphan metadata
    OrphanMeta meta;
    meta.tx = tx;
    meta.peer_id = peer_id;
    meta.time_added = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000LL;  // seconds
    meta.depth = depth;

    // Add orphan
    orphans_[txid] = meta;
    stats_.orphan_count++;
    stats_.orphan_bytes += orphan_size;

    // Track parent-child relationships for efficient processing
    for (const auto& input : tx.vin) {
        std::string parent_txid = input.prevout.txid;
        orphan_children_[parent_txid].push_back(txid);
    }

    dinero::g_logger.debug("Added orphan transaction: " + txid +
                          " from peer: " + peer_id +
                          " depth: " + std::to_string(depth));

    return true;
}

bool TxMempool::RemoveOrphan(const std::string& txid) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = orphans_.find(txid);
    if (it == orphans_.end()) {
        return false;
    }

    // Get the transaction before removing
    const Transaction& tx = it->second.tx;

    // Update stats
    stats_.orphan_count--;
    TxMempoolEntry temp_entry(tx, 0, 0);
    stats_.orphan_bytes -= temp_entry.GetSize();

    // Clean up parent-child relationships
    // Remove this orphan from all parent's child lists
    for (const auto& input : tx.vin) {
        std::string parent_txid = input.prevout.txid;
        auto parent_children_it = orphan_children_.find(parent_txid);

        if (parent_children_it != orphan_children_.end()) {
            auto& children = parent_children_it->second;
            children.erase(std::remove(children.begin(), children.end(), txid), children.end());

            // Remove parent entry if no more children
            if (children.empty()) {
                orphan_children_.erase(parent_children_it);
            }
        }
    }

    // Remove all children entries for this orphan
    orphan_children_.erase(txid);

    // Remove the orphan itself
    orphans_.erase(it);

    return true;
}

std::vector<Transaction> TxMempool::GetOrphansForParent(const std::string& parent_txid) {
    std::lock_guard<std::mutex> lock(mtx_);

    std::vector<Transaction> result;

    // Use orphan_children_ map for O(1) lookup instead of O(n) iteration
    auto it = orphan_children_.find(parent_txid);
    if (it == orphan_children_.end()) {
        return result;  // No children for this parent
    }

    // Collect transactions for all children
    const auto& child_txids = it->second;
    result.reserve(child_txids.size());

    for (const auto& child_txid : child_txids) {
        auto orphan_it = orphans_.find(child_txid);
        if (orphan_it != orphans_.end()) {
            result.push_back(orphan_it->second.tx);
        }
    }

    return result;
}

void TxMempool::LimitOrphans() {
    // This is called with lock already held

    // Remove oldest orphans if we exceed the limit (FIFO eviction)
    while (orphans_.size() > policy_.max_orphans) {
        // Find the oldest orphan by timestamp
        auto oldest_it = orphans_.begin();
        int64_t oldest_time = oldest_it->second.time_added;

        for (auto it = orphans_.begin(); it != orphans_.end(); ++it) {
            if (it->second.time_added < oldest_time) {
                oldest_time = it->second.time_added;
                oldest_it = it;
            }
        }

        if (oldest_it != orphans_.end()) {
            std::string txid = oldest_it->first;
            dinero::g_logger.debug("Evicting oldest orphan: " + txid +
                                  " (age: " + std::to_string(
                                      std::chrono::system_clock::now().time_since_epoch().count() / 1000000000LL - oldest_time
                                  ) + " seconds)");
            RemoveOrphan(txid);
        } else {
            break;
        }
    }
}

void TxMempool::EvictExpiredOrphans() {
    // This is called with lock already held

    int64_t current_time = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000LL;  // seconds
    int64_t timeout = static_cast<int64_t>(policy_.orphan_timeout_sec);

    std::vector<std::string> expired_txids;

    // Collect expired orphan txids
    for (const auto& pair : orphans_) {
        int64_t age = current_time - pair.second.time_added;
        if (age > timeout) {
            expired_txids.push_back(pair.first);
        }
    }

    // Remove expired orphans
    for (const auto& txid : expired_txids) {
        dinero::g_logger.debug("Evicting expired orphan: " + txid +
                              " (age: " + std::to_string(current_time - orphans_[txid].time_added) + " seconds)");
        RemoveOrphan(txid);
    }

    if (!expired_txids.empty()) {
        dinero::g_logger.info("Evicted " + std::to_string(expired_txids.size()) + " expired orphan transactions");
    }
}

uint32_t TxMempool::CalculateOrphanDepth(const Transaction& tx) const {
    // This is called with lock already held

    uint32_t max_depth = 0;

    // Check each input to see if it references an orphan
    for (const auto& input : tx.vin) {
        std::string parent_txid = input.prevout.txid;

        auto it = orphans_.find(parent_txid);
        if (it != orphans_.end()) {
            // Parent is an orphan - depth is parent's depth + 1
            uint32_t parent_depth = it->second.depth;
            uint32_t this_depth = parent_depth + 1;

            if (this_depth > max_depth) {
                max_depth = this_depth;
            }
        }
        // If parent is not an orphan, it doesn't contribute to depth
    }

    return max_depth;  // Returns 0 if no orphan parents
}

TxMempool::Stats TxMempool::GetStats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

void TxMempool::IncrementRejects(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mtx_);
    stats_.rejects_total++;
    stats_.reject_reasons[reason]++;
}

void TxMempool::UpdatePolicy(const MemPoolPolicy& policy) {
    std::lock_guard<std::mutex> lock(mtx_);
    policy_ = policy;
}

// Private helper methods

void TxMempool::UpdateIndexes(const TxMempoolEntry& entry, bool add) {
    // This is called with lock already held
    
    if (add) {
        // Add to indexes
        feerate_index_.emplace(entry.feerate, entry.txid);
        ancestor_index_.emplace(entry.GetAncestorScore(), entry.txid);
    } else {
        // Remove from indexes
        auto feerate_pair = std::make_pair(entry.feerate, entry.txid);
        auto feerate_it = feerate_index_.find(feerate_pair);
        if (feerate_it != feerate_index_.end()) {
            feerate_index_.erase(feerate_it);
        }
        
        auto ancestor_pair = std::make_pair(entry.GetAncestorScore(), entry.txid);
        auto ancestor_it = ancestor_index_.find(ancestor_pair);
        if (ancestor_it != ancestor_index_.end()) {
            ancestor_index_.erase(ancestor_it);
        }
    }
}

std::vector<std::string> TxMempool::SelectEvictionCandidates(uint64_t target_bytes) const {
    // This is called with lock already held
    
    std::vector<std::string> candidates;
    uint64_t evicted_bytes = 0;
    
    // Evict transactions with lowest fee rate first
    for (const auto& pair : feerate_index_) {
        if (evicted_bytes >= target_bytes) {
            break;
        }
        
        const std::string& txid = pair.second;
        auto entry_it = entries_.find(txid);
        if (entry_it != entries_.end()) {
            candidates.push_back(txid);
            evicted_bytes += entry_it->second.vsize;
        }
    }
    
    return candidates;
}

// CombinedUTXOView implementation

CombinedUTXOView::CombinedUTXOView(std::shared_ptr<UTXOView> base, const TxMempool& mempool)
    : base_view_(base), mempool_(mempool) {
}

bool CombinedUTXOView::HaveUTXO(const std::string& txid, uint32_t vout) const {
    // First check if it's spent by mempool
    for (const auto& entry : mempool_.GetEntries()) {
        for (const auto& input : entry.tx.vin) {
            if (input.prevout.txid == txid && input.prevout.vout == vout) {
                return false;  // Spent by mempool transaction
            }
        }
    }
    
    // Check if it's created by mempool
    const auto* entry = mempool_.Get(txid);
    if (entry && vout < entry->tx.vout.size()) {
        return true;  // Created by mempool transaction
    }
    
    // Check base view
    return base_view_->HaveUTXO(txid, vout);
}

bool CombinedUTXOView::GetUTXO(const std::string& txid, uint32_t vout, 
                               uint64_t& value, std::string& script) const {
    // Check if it's created by mempool
    const auto* entry = mempool_.Get(txid);
    if (entry && vout < entry->tx.vout.size()) {
        value = entry->tx.vout[vout].value;
        script = entry->tx.vout[vout].scriptPubKey;
        return true;
    }
    
    // Check if it's spent by mempool
    for (const auto& mempool_entry : mempool_.GetEntries()) {
        for (const auto& input : mempool_entry.tx.vin) {
            if (input.prevout.txid == txid && input.prevout.vout == vout) {
                return false;  // Spent by mempool transaction
            }
        }
    }
    
    // Check base view
    return base_view_->GetUTXO(txid, vout, value, script);
}

bool CombinedUTXOView::HaveTransaction(const std::string& txid) const {
    // Check mempool first
    if (mempool_.Exists(txid)) {
        return true;
    }
    
    // Check base view
    return base_view_->HaveTransaction(txid);
}

uint32_t CombinedUTXOView::GetHeight() const {
    return base_view_->GetHeight();
}

std::set<std::string> TxMempool::GetAncestors(const std::string& txid) const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Find the transaction entry
    auto it = entries_.find(txid);
    if (it == entries_.end()) {
        return {}; // Transaction not found
    }
    
    // Build ancestor set by traversing parent dependencies
    std::set<std::string> ancestors;
    std::queue<std::string> to_process;
    to_process.push(txid);
    
    while (!to_process.empty()) {
        std::string current = to_process.front();
        to_process.pop();
        
        auto current_it = entries_.find(current);
        if (current_it == entries_.end()) continue;
        
        // Add parents to ancestor set and processing queue
        for (const auto& parent_txid : current_it->second.depends) {
            if (ancestors.find(parent_txid) == ancestors.end()) {
                ancestors.insert(parent_txid);
                to_process.push(parent_txid);
            }
        }
    }
    
    return ancestors;
}

std::set<std::string> TxMempool::GetDescendants(const std::string& txid) const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Find the transaction entry
    auto it = entries_.find(txid);
    if (it == entries_.end()) {
        return {}; // Transaction not found
    }
    
    // Build descendant set by traversing child dependencies
    std::set<std::string> descendants;
    std::queue<std::string> to_process;
    to_process.push(txid);
    
    while (!to_process.empty()) {
        std::string current = to_process.front();
        to_process.pop();
        
        // Find all transactions that depend on current transaction
        for (const auto& [candidate_txid, entry] : entries_) {
            if (descendants.find(candidate_txid) != descendants.end()) continue;
            
            // Check if this transaction has current as a parent
            for (const auto& parent_txid : entry.depends) {
                if (parent_txid == current) {
                    descendants.insert(candidate_txid);
                    to_process.push(candidate_txid);
                    break;
                }
            }
        }
    }
    
    return descendants;
}

} // namespace dinero
