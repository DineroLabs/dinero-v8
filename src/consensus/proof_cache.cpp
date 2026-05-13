#include "consensus/proof_cache.h"
#include <chrono>
#include <algorithm>

namespace dinero {
namespace consensus {

// CachedProof helper methods

uint64_t CachedProof::GetCurrentTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t CachedProof::EstimateSize(const BlockUtreexoData& proof) {
    // Use the built-in size() method from BlockUtreexoData
    return proof.size();
}

// ProofCache implementation

ProofCache::ProofCache() {
    // Initialize empty cache
}

std::optional<BlockUtreexoData> ProofCache::Get(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(block_hash);
    if (it == cache_.end()) {
        misses_++;
        return std::nullopt;
    }

    auto& entry = it->second;

    // Check TTL expiration
    uint64_t now = CachedProof::GetCurrentTimestamp();
    if (now - entry.timestamp > ttl_secs_) {
        // Expired entry - remove and return miss
        RemoveFromLRU(block_hash);
        total_size_bytes_ -= entry.size_bytes;
        cache_.erase(it);
        misses_++;
        return std::nullopt;
    }

    // Update access tracking
    entry.access_count++;
    UpdateLRU(block_hash);
    hits_++;

    return entry.proof;
}

std::optional<BlockUtreexoData> ProofCache::GetWithRoot(const uint256& block_hash, uint256& out_root) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(block_hash);
    if (it == cache_.end()) {
        misses_++;
        return std::nullopt;
    }

    auto& entry = it->second;

    // Check TTL expiration
    uint64_t now = CachedProof::GetCurrentTimestamp();
    if (now - entry.timestamp > ttl_secs_) {
        // Expired entry - remove and return miss
        RemoveFromLRU(block_hash);
        total_size_bytes_ -= entry.size_bytes;
        cache_.erase(it);
        misses_++;
        return std::nullopt;
    }

    // Update access tracking
    entry.access_count++;
    UpdateLRU(block_hash);
    hits_++;

    out_root = entry.root_hash;
    return entry.proof;
}

void ProofCache::Put(const uint256& block_hash, const BlockUtreexoData& proof, const uint256& root_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already cached (update case)
    auto it = cache_.find(block_hash);
    if (it != cache_.end()) {
        // Update existing entry
        total_size_bytes_ -= it->second.size_bytes;
        it->second = CachedProof(block_hash, proof, root_hash);
        total_size_bytes_ += it->second.size_bytes;
        UpdateLRU(block_hash);
        return;
    }

    // Create new entry
    CachedProof entry(block_hash, proof, root_hash);
    total_size_bytes_ += entry.size_bytes;
    cache_[block_hash] = entry;

    // Add to LRU (front = most recently used)
    lru_order_.push_front(block_hash);
    lru_lookup_[block_hash] = lru_order_.begin();

    // Check if eviction needed
    CheckAndEvict();
}

size_t ProofCache::Evict(EvictionPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t initial_size = cache_.size();

    switch (policy) {
        case EvictionPolicy::LRU:
            EvictLRU();
            break;
        case EvictionPolicy::TTL:
            EvictTTL();
            break;
        case EvictionPolicy::OLDEST:
            EvictOldest();
            break;
    }

    return initial_size - cache_.size();
}

bool ProofCache::Evict(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(block_hash);
    if (it == cache_.end()) {
        return false;
    }

    // Remove from cache
    total_size_bytes_ -= it->second.size_bytes;
    cache_.erase(it);

    // Remove from LRU
    RemoveFromLRU(block_hash);

    return true;
}

void ProofCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    cache_.clear();
    lru_order_.clear();
    lru_lookup_.clear();
    total_size_bytes_ = 0;
    hits_ = 0;
    misses_ = 0;
}

size_t ProofCache::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

size_t ProofCache::TotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_size_bytes_;
}

double ProofCache::HitRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = hits_ + misses_;
    if (total == 0) return 0.0;
    return static_cast<double>(hits_) / total;
}

// Private helpers

void ProofCache::EvictLRU() {
    // Remove least recently used (back of list)
    if (lru_order_.empty()) return;

    uint256 to_evict = lru_order_.back();
    auto it = cache_.find(to_evict);
    if (it != cache_.end()) {
        total_size_bytes_ -= it->second.size_bytes;
        cache_.erase(it);
    }

    lru_order_.pop_back();
    lru_lookup_.erase(to_evict);
}

void ProofCache::EvictTTL() {
    // Remove expired entries
    uint64_t now = CachedProof::GetCurrentTimestamp();
    std::vector<uint256> to_evict;

    for (const auto& [hash, entry] : cache_) {
        if (now - entry.timestamp > ttl_secs_) {
            to_evict.push_back(hash);
        }
    }

    for (const auto& hash : to_evict) {
        auto it = cache_.find(hash);
        if (it != cache_.end()) {
            total_size_bytes_ -= it->second.size_bytes;
            cache_.erase(it);
        }
        RemoveFromLRU(hash);
    }
}

void ProofCache::EvictOldest() {
    // Find oldest entry by timestamp
    if (cache_.empty()) return;

    uint256 oldest_hash;
    uint64_t oldest_timestamp = UINT64_MAX;

    for (const auto& [hash, entry] : cache_) {
        if (entry.timestamp < oldest_timestamp) {
            oldest_timestamp = entry.timestamp;
            oldest_hash = hash;
        }
    }

    // Remove oldest
    auto it = cache_.find(oldest_hash);
    if (it != cache_.end()) {
        total_size_bytes_ -= it->second.size_bytes;
        cache_.erase(it);
    }
    RemoveFromLRU(oldest_hash);
}

void ProofCache::CheckAndEvict() {
    // If cache exceeds max size, evict LRU entries
    while (total_size_bytes_ > MAX_CACHE_SIZE && !cache_.empty()) {
        EvictLRU();
    }
}

void ProofCache::UpdateLRU(const uint256& block_hash) {
    // Move to front (most recently used)
    auto it = lru_lookup_.find(block_hash);
    if (it != lru_lookup_.end()) {
        lru_order_.erase(it->second);
    }

    lru_order_.push_front(block_hash);
    lru_lookup_[block_hash] = lru_order_.begin();
}

void ProofCache::RemoveFromLRU(const uint256& block_hash) {
    auto it = lru_lookup_.find(block_hash);
    if (it != lru_lookup_.end()) {
        lru_order_.erase(it->second);
        lru_lookup_.erase(it);
    }
}

} // namespace consensus
} // namespace dinero
