#include "p2p/addrman.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <ctime>
#include <random>
#include <unordered_set>

namespace dinero {
namespace p2p {

// Architecture V3: Global removed - use AddressManagerService instead
// std::unique_ptr<AddressManager> g_addrman;

// NetworkAddress implementation
std::string NetworkAddress::toString() const {
    return ip + ":" + std::to_string(port);
}

bool NetworkAddress::isValid() const {
    if (ip.empty() || port == 0) return false;
    
    // Basic IP validation (IPv4 for now)
    std::istringstream iss(ip);
    std::string segment;
    int count = 0;
    
    while (std::getline(iss, segment, '.')) {
        if (++count > 4) return false;
        try {
            int val = std::stoi(segment);
            if (val < 0 || val > 255) return false;
        } catch (...) {
            return false;
        }
    }
    
    return count == 4;
}

bool NetworkAddress::isRoutable() const {
    if (!isValid()) return false;
    
    // Check for non-routable addresses
    if (ip.substr(0, 3) == "10." ||
        ip.substr(0, 8) == "192.168." ||
        ip.substr(0, 7) == "172.16." ||
        ip == "127.0.0.1" ||
        ip == "0.0.0.0") {
        return false;
    }
    
    return true;
}

bool NetworkAddress::isLocal() const {
    return ip == "127.0.0.1" || ip == "::1" || ip.substr(0, 3) == "10." || ip.substr(0, 8) == "192.168.";
}

bool NetworkAddress::operator==(const NetworkAddress& other) const {
    return ip == other.ip && port == other.port;
}

bool NetworkAddress::operator<(const NetworkAddress& other) const {
    if (ip != other.ip) return ip < other.ip;
    return port < other.port;
}

// AddressEntry implementation
void AddressEntry::updateSuccessRate() {
    if (attempts == 0) {
        success_rate = 0.0;
    } else {
        success_rate = static_cast<double>(successes) / attempts;
    }
}

bool AddressEntry::shouldRetry() const {
    if (is_banned || is_terrible) return false;
    
    auto now = std::chrono::system_clock::now();
    auto retry_delay = getRetryDelay();
    
    return (now - last_try) >= retry_delay;
}

std::chrono::seconds AddressEntry::getRetryDelay() const {
    // Exponential backoff based on failures
    uint32_t failures = attempts - successes;
    if (failures == 0) return std::chrono::seconds(60);  // 1 minute for successful addresses
    
    // Exponential backoff: 2^failures minutes, capped at 24 hours
    uint32_t delay_minutes = std::min(static_cast<uint32_t>(1 << std::min(failures, 10u)), 24u * 60u);
    return std::chrono::seconds(delay_minutes * 60);
}

// AddressManager implementation
AddressManager::AddressManager() 
    : max_new_addresses_(DEFAULT_MAX_NEW)
    , max_tried_addresses_(DEFAULT_MAX_TRIED)
    , default_ban_duration_(DEFAULT_BAN_DURATION)
    , terrible_threshold_(DEFAULT_TERRIBLE_THRESHOLD)
    , rng_(std::random_device{}()) {
}

AddressManager::~AddressManager() = default;

void AddressManager::addAddresses(const std::vector<NetworkAddress>& addresses, const std::string& source_peer) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& addr : addresses) {
        if (!isValidAddress(addr)) {
            continue;
        }
        const std::string key = getAddressKey(addr);

        // Re-advertisement of a known address: refresh last_seen and,
        // when the source carried real service info, update the stored
        // flags — this is how a peer's NODE_RELAY bit reaches addrman.
        // SET (not OR) so a service a peer drops is reflected; services
        // == 0 is treated as "unknown" (legacy `addr` has no services
        // field) and left alone so it can't wipe known flags.
        if (isDuplicate(addr)) {
            AddressEntry* existing = nullptr;
            if (auto it = new_addresses_.find(key); it != new_addresses_.end()) {
                existing = &it->second;
            } else if (auto it2 = tried_addresses_.find(key);
                       it2 != tried_addresses_.end()) {
                existing = &it2->second;
            }
            if (existing) {
                existing->last_seen = std::chrono::system_clock::now();
                existing->ref_count++;
                if (addr.services != 0) {
                    existing->addr.services = addr.services;
                }
            }
            continue;
        }

        // Eclipse prevention: limit addresses per /16 subnet
        std::string subnet = extractSubnet16(addr.ip);
        if (countInSubnet16(new_addresses_, subnet) >= MAX_PER_SUBNET16_NEW) {
            continue;  // Skip — /16 subnet saturated in new pool
        }

        AddressEntry entry;
        entry.addr = addr;
        entry.first_seen = std::chrono::system_clock::now();
        entry.last_seen = entry.first_seen;
        entry.ref_count = 1;

        new_addresses_[key] = entry;
        total_added_++;

        // Evict oldest if we exceed capacity
        if (new_addresses_.size() > max_new_addresses_) {
            evictOldest(AddressPool::NEW);
        }
    }
}

void AddressManager::addAddress(const NetworkAddress& addr, const std::string& source_peer) {
    addAddresses({addr}, source_peer);
}

std::vector<NetworkAddress> AddressManager::getAddresses(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NetworkAddress> result;
    std::unordered_set<std::string> used_subnets;  // Eclipse prevention: subnet diversity

    // Try to get addresses from both pools
    size_t tried_count = count / 2;
    size_t new_count = count - tried_count;

    // Get from tried pool first (more reliable)
    // Allow up to 3x attempts to find diverse subnets
    for (size_t attempts = 0; attempts < tried_count * 3 && result.size() < tried_count && !tried_addresses_.empty(); ++attempts) {
        try {
            NetworkAddress addr = selectFromTried();
            if (addr.isValid()) {
                std::string subnet = extractSubnet16(addr.ip);
                if (used_subnets.count(subnet) == 0) {
                    used_subnets.insert(subnet);
                    result.push_back(addr);
                }
            }
        } catch (...) {
            break;
        }
    }

    // Fill remaining from new pool
    for (size_t attempts = 0; attempts < new_count * 3 && result.size() < count && !new_addresses_.empty(); ++attempts) {
        try {
            NetworkAddress addr = selectFromNew();
            if (addr.isValid()) {
                std::string subnet = extractSubnet16(addr.ip);
                if (used_subnets.count(subnet) == 0) {
                    used_subnets.insert(subnet);
                    result.push_back(addr);
                }
            }
        } catch (...) {
            break;
        }
    }

    return result;
}

std::vector<NetworkAddress> AddressManager::getAddressesByService(
    uint64_t service_bit, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NetworkAddress> result;
    std::unordered_set<std::string> used_subnets;  // /16 spread

    auto scan = [&](const std::unordered_map<std::string, AddressEntry>& pool) {
        for (const auto& kv : pool) {
            if (result.size() >= count) return;
            const AddressEntry& entry = kv.second;
            if (entry.is_terrible || entry.is_banned) continue;
            if ((entry.addr.services & service_bit) == 0) continue;
            if (!entry.addr.isRoutable()) continue;
            const std::string subnet = extractSubnet16(entry.addr.ip);
            if (used_subnets.count(subnet) != 0) continue;
            used_subnets.insert(subnet);
            result.push_back(entry.addr);
        }
    };
    scan(tried_addresses_);  // prefer previously-connected peers
    scan(new_addresses_);
    return result;
}

size_t AddressManager::countAddressesByService(uint64_t service_bit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;

    auto scan = [&](const std::unordered_map<std::string, AddressEntry>& pool) {
        for (const auto& kv : pool) {
            const AddressEntry& entry = kv.second;
            if (entry.is_terrible || entry.is_banned) continue;
            if ((entry.addr.services & service_bit) == 0) continue;
            if (!entry.addr.isRoutable()) continue;
            ++count;
        }
    };

    scan(tried_addresses_);
    scan(new_addresses_);
    return count;
}

std::vector<NetworkAddress> AddressManager::getAdvertisableAddresses(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NetworkAddress> result;
    
    // Collect good addresses from tried pool
    std::vector<std::pair<double, NetworkAddress>> candidates;
    
    for (const auto& [key, entry] : tried_addresses_) {
        if (!entry.is_terrible && !entry.is_banned && entry.addr.isRoutable()) {
            double priority = calculatePriority(entry);
            candidates.emplace_back(priority, entry.addr);
        }
    }
    
    // Sort by priority and take top candidates
    std::sort(candidates.begin(), candidates.end(), std::greater<>());
    
    size_t take_count = std::min(count, candidates.size());
    for (size_t i = 0; i < take_count; ++i) {
        result.push_back(candidates[i].second);
    }
    
    return result;
}

void AddressManager::markAttempt(const NetworkAddress& addr, bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    AddressEntry* entry = findAddress(addr);
    if (!entry) return;
    
    entry->attempts++;
    entry->last_try = std::chrono::system_clock::now();
    total_attempts_++;
    
    if (success) {
        entry->successes++;
        entry->last_success = entry->last_try;
        total_successes_++;
        
        // Move to tried pool if it was in new pool
        std::string key = getAddressKey(addr);
        if (new_addresses_.find(key) != new_addresses_.end()) {
            moveToTried(addr);
        }
    }
    
    entry->updateSuccessRate();
    
    // Mark as terrible if too many failures
    if (entry->attempts >= terrible_threshold_ && entry->success_rate < 0.1) {
        entry->is_terrible = true;
    }
}

void AddressManager::markGood(const NetworkAddress& addr) {
    markAttempt(addr, true);
}

void AddressManager::markTerrible(const NetworkAddress& addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    AddressEntry* entry = findAddress(addr);
    if (entry) {
        entry->is_terrible = true;
    }
}

void AddressManager::banAddress(const NetworkAddress& addr, std::chrono::seconds duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = getAddressKey(addr);
    auto ban_until = std::chrono::system_clock::now() + duration;
    banned_addresses_[key] = ban_until;
    
    AddressEntry* entry = findAddress(addr);
    if (entry) {
        entry->is_banned = true;
        entry->ban_until = ban_until;
    }
}

bool AddressManager::isBanned(const NetworkAddress& addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = getAddressKey(addr);
    auto it = banned_addresses_.find(key);
    if (it == banned_addresses_.end()) return false;
    
    auto now = std::chrono::system_clock::now();
    return now < it->second;
}

AddressManager::AddrmanStats AddressManager::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    AddrmanStats stats;
    stats.total_addresses = new_addresses_.size() + tried_addresses_.size();
    stats.new_addresses = new_addresses_.size();
    stats.tried_addresses = tried_addresses_.size();
    
    size_t terrible_count = 0;
    size_t banned_count = 0;
    double total_success_rate = 0.0;
    size_t success_rate_count = 0;
    
    auto now = std::chrono::system_clock::now();
    
    for (const auto& [key, entry] : tried_addresses_) {
        if (entry.is_terrible) terrible_count++;
        if (entry.is_banned && now < entry.ban_until) banned_count++;
        if (entry.attempts > 0) {
            total_success_rate += entry.success_rate;
            success_rate_count++;
        }
    }
    
    for (const auto& [key, ban_time] : banned_addresses_) {
        if (now < ban_time) banned_count++;
    }
    
    stats.terrible_addresses = terrible_count;
    stats.banned_addresses = banned_count;
    stats.avg_success_rate = success_rate_count > 0 ? total_success_rate / success_rate_count : 0.0;
    
    return stats;
}

void AddressManager::performMaintenance() {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanupExpired();
    updateStatistics();
}

void AddressManager::clearBanned() {
    std::lock_guard<std::mutex> lock(mutex_);
    banned_addresses_.clear();
    
    for (auto& [key, entry] : new_addresses_) {
        entry.is_banned = false;
    }
    for (auto& [key, entry] : tried_addresses_) {
        entry.is_banned = false;
    }
}

void AddressManager::clearTerrible() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& [key, entry] : new_addresses_) {
        entry.is_terrible = false;
    }
    for (auto& [key, entry] : tried_addresses_) {
        entry.is_terrible = false;
    }
}

// Private methods
void AddressManager::moveToTried(const NetworkAddress& addr) {
    std::string key = getAddressKey(addr);
    auto it = new_addresses_.find(key);
    if (it == new_addresses_.end()) return;

    // Eclipse prevention: limit addresses per /16 subnet in tried pool
    std::string subnet = extractSubnet16(addr.ip);
    if (countInSubnet16(tried_addresses_, subnet) >= MAX_PER_SUBNET16_TRIED) {
        return;  // Keep in new pool — /16 subnet saturated in tried pool
    }

    AddressEntry entry = it->second;
    new_addresses_.erase(it);

    tried_addresses_[key] = entry;

    // Evict oldest if we exceed capacity
    if (tried_addresses_.size() > max_tried_addresses_) {
        evictOldest(AddressPool::TRIED);
    }
}

void AddressManager::evictOldest(AddressPool pool) {
    auto& pool_map = (pool == AddressPool::NEW) ? new_addresses_ : tried_addresses_;
    if (pool_map.empty()) return;
    
    // Find oldest entry
    auto oldest_it = pool_map.begin();
    for (auto it = pool_map.begin(); it != pool_map.end(); ++it) {
        if (it->second.first_seen < oldest_it->second.first_seen) {
            oldest_it = it;
        }
    }
    
    pool_map.erase(oldest_it);
}

NetworkAddress AddressManager::selectFromNew() {
    if (new_addresses_.empty()) return {};
    
    std::uniform_int_distribution<size_t> dist(0, new_addresses_.size() - 1);
    auto it = new_addresses_.begin();
    std::advance(it, dist(rng_));
    return it->second.addr;
}

NetworkAddress AddressManager::selectFromTried() {
    if (tried_addresses_.empty()) return {};
    
    // Weighted selection based on success rate
    std::vector<std::pair<double, NetworkAddress>> candidates;
    
    for (const auto& [key, entry] : tried_addresses_) {
        if (!entry.is_terrible && !entry.is_banned && entry.shouldRetry()) {
            double priority = calculatePriority(entry);
            candidates.emplace_back(priority, entry.addr);
        }
    }
    
    if (candidates.empty()) {
        // Fallback to random selection
        std::uniform_int_distribution<size_t> dist(0, tried_addresses_.size() - 1);
        auto it = tried_addresses_.begin();
        std::advance(it, dist(rng_));
        return it->second.addr;
    }
    
    // Weighted random selection
    double total_weight = 0.0;
    for (const auto& [weight, addr] : candidates) {
        total_weight += weight;
    }
    
    std::uniform_real_distribution<double> weight_dist(0.0, total_weight);
    double target = weight_dist(rng_);
    
    double current_weight = 0.0;
    for (const auto& [weight, addr] : candidates) {
        current_weight += weight;
        if (current_weight >= target) {
            return addr;
        }
    }
    
    return candidates.back().second;
}

double AddressManager::calculatePriority(const AddressEntry& entry) const {
    double priority = 1.0;
    
    // Boost based on success rate
    priority *= (1.0 + entry.success_rate);
    
    // Boost recent successful connections
    auto now = std::chrono::system_clock::now();
    auto hours_since_success = std::chrono::duration_cast<std::chrono::hours>(now - entry.last_success).count();
    if (hours_since_success < 24) {
        priority *= 2.0;
    }
    
    // Penalize old addresses
    auto days_since_seen = std::chrono::duration_cast<std::chrono::hours>(now - entry.last_seen).count() / 24;
    if (days_since_seen > 7) {
        priority *= 0.5;
    }
    
    return priority;
}

bool AddressManager::isValidAddress(const NetworkAddress& addr) const {
    return addr.isValid() && addr.isRoutable();
}

bool AddressManager::isDuplicate(const NetworkAddress& addr) const {
    std::string key = getAddressKey(addr);
    return new_addresses_.find(key) != new_addresses_.end() ||
           tried_addresses_.find(key) != tried_addresses_.end();
}

void AddressManager::cleanupExpired() {
    auto now = std::chrono::system_clock::now();
    
    // Remove expired bans
    for (auto it = banned_addresses_.begin(); it != banned_addresses_.end();) {
        if (now >= it->second) {
            it = banned_addresses_.erase(it);
        } else {
            ++it;
        }
    }
    
    // Remove very old addresses
    auto expire_time = now - ADDRESS_EXPIRE_TIME;
    
    for (auto it = new_addresses_.begin(); it != new_addresses_.end();) {
        if (it->second.last_seen < expire_time) {
            it = new_addresses_.erase(it);
        } else {
            ++it;
        }
    }
}

void AddressManager::updateStatistics() {
    // Statistics are updated in real-time via atomic counters
}

std::string AddressManager::getAddressKey(const NetworkAddress& addr) const {
    return addr.toString();
}

AddressEntry* AddressManager::findAddress(const NetworkAddress& addr) {
    std::string key = getAddressKey(addr);
    
    auto it = new_addresses_.find(key);
    if (it != new_addresses_.end()) {
        return &it->second;
    }
    
    auto it2 = tried_addresses_.find(key);
    if (it2 != tried_addresses_.end()) {
        return &it2->second;
    }
    
    return nullptr;
}

const AddressEntry* AddressManager::findAddress(const NetworkAddress& addr) const {
    return const_cast<AddressManager*>(this)->findAddress(addr);
}

std::string AddressManager::extractSubnet16(const std::string& ip) {
    auto dot1 = ip.find('.');
    if (dot1 == std::string::npos) return ip;
    auto dot2 = ip.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return ip;
    return ip.substr(0, dot2);  // "192.168" from "192.168.1.100"
}

size_t AddressManager::countInSubnet16(
    const std::unordered_map<std::string, AddressEntry>& pool,
    const std::string& subnet) const {
    size_t count = 0;
    for (const auto& [key, entry] : pool) {
        if (extractSubnet16(entry.addr.ip) == subnet) {
            count++;
        }
    }
    return count;
}

// Architecture V3: Init/Shutdown removed - use AddressManagerService lifecycle methods
// Deprecated functions - use AddressManagerService::Init/Start/Stop instead

} // namespace p2p
} // namespace dinero
