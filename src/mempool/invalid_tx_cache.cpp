#include "mempool/invalid_tx_cache.h"
#include <algorithm>

namespace dinero {
namespace mempool {

InvalidTxCache::InvalidTxCache(const Config& config)
    : config_(config)
    , hits_(0)
    , misses_(0)
    , evictions_(0)
    , expiries_(0)
{
}

void InvalidTxCache::add(const TxId& txid, const std::string& reason, uint64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already in cache (update reason + timestamp)
    auto it = cache_.find(txid);
    if (it != cache_.end()) {
        it->second.reason = reason;
        it->second.timestamp = timestamp;
        touchLRU(txid);
        return;
    }

    // Evict LRU if cache is full
    if (cache_.size() >= config_.max_entries) {
        evictLRU();
    }

    // Add new entry
    CacheEntry entry;
    entry.reason = reason;
    entry.timestamp = timestamp;
    cache_[txid] = entry;

    // Add to LRU list (most recently used at back)
    lru_list_.push_back(txid);
    lru_map_[txid] = std::prev(lru_list_.end());
}

std::optional<std::string> InvalidTxCache::lookup(const TxId& txid, uint64_t current_time) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(txid);
    if (it == cache_.end()) {
        misses_++;
        return std::nullopt;
    }

    // Check if expired
    if (isExpired(it->second, current_time)) {
        // Remove expired entry
        lru_list_.erase(lru_map_[txid]);
        lru_map_.erase(txid);
        cache_.erase(it);
        expiries_++;
        misses_++;
        return std::nullopt;
    }

    // Cache hit - update LRU
    hits_++;
    touchLRU(txid);
    return it->second.reason;
}

void InvalidTxCache::remove(const TxId& txid) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(txid);
    if (it == cache_.end()) {
        return;
    }

    // Remove from LRU
    lru_list_.erase(lru_map_[txid]);
    lru_map_.erase(txid);

    // Remove from cache
    cache_.erase(it);
}

void InvalidTxCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    cache_.clear();
    lru_list_.clear();
    lru_map_.clear();
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
    expiries_ = 0;
}

InvalidTxCache::Stats InvalidTxCache::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.size = cache_.size();
    stats.max_size = config_.max_entries;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.evictions = evictions_;
    stats.expiries = expiries_;
    return stats;
}

// Private helpers

void InvalidTxCache::touchLRU(const TxId& txid) {
    // Move to back of LRU list (most recently used)
    auto lru_it = lru_map_.find(txid);
    if (lru_it != lru_map_.end()) {
        lru_list_.erase(lru_it->second);
    }

    lru_list_.push_back(txid);
    lru_map_[txid] = std::prev(lru_list_.end());
}

void InvalidTxCache::evictLRU() {
    if (lru_list_.empty()) {
        return;
    }

    // Remove least recently used (front of list) - Phase M.4.3-C: now TxId
    TxId lru_txid = lru_list_.front();
    lru_list_.pop_front();
    lru_map_.erase(lru_txid);
    cache_.erase(lru_txid);
    evictions_++;
}

bool InvalidTxCache::isExpired(const CacheEntry& entry, uint64_t current_time) const {
    return (current_time - entry.timestamp) > config_.expiry_seconds;
}

} // namespace mempool
} // namespace dinero
