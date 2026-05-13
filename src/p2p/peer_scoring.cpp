#include "p2p/peer_scoring.h"
#include <algorithm>
#include <fstream>
#include <cmath>

namespace dinero {
namespace p2p {

// Architecture V3: No global - PeerScoringManager now managed via DaemonContext

// Static members for DoSProtection
std::mutex DoSProtection::rate_limit_mutex_;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> DoSProtection::last_request_times_;
std::unordered_map<std::string, uint32_t> DoSProtection::request_counts_;
std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> DoSProtection::connection_attempts_;
std::mutex DoSProtection::bandwidth_mutex_;
std::unordered_map<std::string, DoSProtection::BandwidthStats> DoSProtection::bandwidth_stats_;

// PeerScore implementation
void PeerScore::addMisbehavior(MisbehaviorType type) {
    auto now = std::chrono::system_clock::now();
    
    // Add to history
    history.emplace_back(type, now);
    
    // Limit history size
    if (history.size() > 100) {
        history.erase(history.begin());
    }
    
    // Update counters
    misbehavior_count++;
    last_misbehavior = now;
    
    // Add score based on misbehavior type
    int32_t penalty = static_cast<int32_t>(type);
    score += penalty;
    lifetime_score += penalty;
}

bool PeerScore::shouldBan(int32_t ban_threshold) const {
    return score >= ban_threshold;
}

void PeerScore::decay(double decay_rate) {
    if (score > 0) {
        score = static_cast<int32_t>(score * (1.0 - decay_rate));
        if (score < 0) score = 0;
    }
}

// PeerScoringManager implementation
PeerScoringManager::PeerScoringManager()
    : ban_threshold_(DEFAULT_BAN_THRESHOLD)
    , decay_rate_(DEFAULT_DECAY_RATE)
    , default_ban_duration_(DEFAULT_BAN_DURATION)
    , max_ban_duration_(MAX_BAN_DURATION)
    , max_history_size_(DEFAULT_HISTORY_SIZE)
    , last_maintenance_(std::chrono::system_clock::now()) {
}

PeerScoringManager::~PeerScoringManager() = default;

void PeerScoringManager::addMisbehavior(const std::string& peer_id, MisbehaviorType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    updatePeerScore(peer_id, type);
    checkForBan(peer_id);
    total_misbehaviors_++;
}

int32_t PeerScoringManager::getScore(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peer_scores_.find(peer_id);
    return (it != peer_scores_.end()) ? it->second.score : 0;
}

bool PeerScoringManager::isBanned(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peer_scores_.find(peer_id);
    if (it == peer_scores_.end()) return false;
    
    const auto& peer = it->second;
    if (!peer.is_banned) return false;
    
    auto now = std::chrono::system_clock::now();
    return now < peer.ban_until;
}

void PeerScoringManager::banPeer(const std::string& peer_id, std::chrono::seconds duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& peer = peer_scores_[peer_id];
    peer.peer_id = peer_id;
    peer.is_banned = true;
    peer.ban_until = std::chrono::system_clock::now() + duration;
    total_bans_++;
}

void PeerScoringManager::unbanPeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peer_scores_.find(peer_id);
    if (it != peer_scores_.end()) {
        it->second.is_banned = false;
        it->second.ban_until = std::chrono::system_clock::time_point{};
    }
}

PeerScore PeerScoringManager::getPeerScore(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peer_scores_.find(peer_id);
    return (it != peer_scores_.end()) ? it->second : PeerScore{};
}

std::vector<std::string> PeerScoringManager::getBannedPeers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> banned;
    auto now = std::chrono::system_clock::now();
    
    for (const auto& [peer_id, peer] : peer_scores_) {
        if (peer.is_banned && now < peer.ban_until) {
            banned.push_back(peer_id);
        }
    }
    
    return banned;
}

PeerScoringManager::ScoringStats PeerScoringManager::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ScoringStats stats{};
    stats.total_peers = peer_scores_.size();
    
    int64_t total_score = 0;
    auto now = std::chrono::system_clock::now();
    auto oldest_ban = now;
    
    for (const auto& [peer_id, peer] : peer_scores_) {
        if (peer.score > 0) {
            stats.misbehaving_peers++;
        }
        
        if (peer.is_banned && now < peer.ban_until) {
            stats.banned_peers++;
            if (peer.ban_until < oldest_ban) {
                oldest_ban = peer.ban_until;
            }
        }
        
        total_score += peer.score;
    }
    
    stats.avg_score = stats.total_peers > 0 ? static_cast<int32_t>(total_score / stats.total_peers) : 0;
    stats.total_misbehaviors = total_misbehaviors_.load();
    stats.oldest_ban = (stats.banned_peers > 0) ? oldest_ban : now;
    
    return stats;
}

void PeerScoringManager::performMaintenance() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    auto time_since_maintenance = std::chrono::duration_cast<std::chrono::hours>(now - last_maintenance_);

    if (time_since_maintenance >= MAINTENANCE_INTERVAL) {
        decayScores();
        decayBanCounts(now);
        cleanupOldEntries();
        last_maintenance_ = now;
    }
}

void PeerScoringManager::clearExpiredBans() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::system_clock::now();
    
    for (auto& [peer_id, peer] : peer_scores_) {
        if (peer.is_banned && now >= peer.ban_until) {
            peer.is_banned = false;
            peer.ban_until = std::chrono::system_clock::time_point{};
        }
    }
}

void PeerScoringManager::clearAllBans() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& [peer_id, peer] : peer_scores_) {
        peer.is_banned = false;
        peer.ban_until = std::chrono::system_clock::time_point{};
    }
}

void PeerScoringManager::resetScores() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& [peer_id, peer] : peer_scores_) {
        peer.score = 0;
        peer.misbehavior_count = 0;
        peer.history.clear();
    }
}

void PeerScoringManager::setBanThreshold(int32_t threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    ban_threshold_ = threshold;
}

void PeerScoringManager::setDecayRate(double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    decay_rate_ = std::clamp(rate, 0.0, 1.0);
}

void PeerScoringManager::setDefaultBanDuration(std::chrono::seconds duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_ban_duration_ = duration;
}

void PeerScoringManager::setMaxBanDuration(std::chrono::seconds duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_ban_duration_ = duration;
}

void PeerScoringManager::setHistorySize(size_t max_history) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_history_size_ = max_history;
}

// Private methods
void PeerScoringManager::updatePeerScore(const std::string& peer_id, MisbehaviorType type) {
    auto& peer = peer_scores_[peer_id];
    
    if (peer.peer_id.empty()) {
        peer.peer_id = peer_id;
        peer.first_seen = std::chrono::system_clock::now();
    }
    
    peer.addMisbehavior(type);
}

void PeerScoringManager::checkForBan(const std::string& peer_id) {
    auto& peer = peer_scores_[peer_id];

    if (peer.shouldBan(ban_threshold_) && !peer.is_banned) {
        peer.ban_count++;
        auto base_duration = calculateBanDuration(peer.score);

        // Escalation curve (NAT-friendly, decays over time via decayBanCounts):
        //   ban_count 1 → 1x base (24h default)
        //   ban_count 2 → 4x     (4 days)
        //   ban_count 3+→ 16x    (16 days, capped at max_ban_duration_ = 30 days)
        // ban_count decays by 1 per 7 clean days, so shared IPs recover.
        uint32_t escalation = std::min(peer.ban_count, static_cast<uint32_t>(3));
        double escalation_factor = std::pow(4.0, static_cast<double>(escalation - 1));
        auto final_duration = std::chrono::seconds(static_cast<int64_t>(
            base_duration.count() * escalation_factor));
        final_duration = std::min(final_duration, max_ban_duration_);

        peer.is_banned = true;
        peer.ban_until = std::chrono::system_clock::now() + final_duration;
        total_bans_++;
    }
}

void PeerScoringManager::decayScores() {
    for (auto& [peer_id, peer] : peer_scores_) {
        peer.decay(decay_rate_);
    }
}

void PeerScoringManager::decayBanCounts(std::chrono::system_clock::time_point now) {
    // Decay ban_count by 1 for every 7 days of clean behavior (no misbehavior).
    // This ensures peers behind NAT/shared IPs aren't punished forever for a
    // single bad actor that used the IP months ago.
    constexpr auto DECAY_INTERVAL = std::chrono::hours(7 * 24);  // 7 days

    for (auto& [peer_id, peer] : peer_scores_) {
        if (peer.ban_count == 0) continue;
        if (peer.is_banned) continue;  // Don't decay while actively banned

        auto since_last_trouble = now - peer.last_misbehavior;
        if (since_last_trouble >= DECAY_INTERVAL) {
            // Decay by number of full 7-day periods since last misbehavior
            auto hours_clean = std::chrono::duration_cast<std::chrono::hours>(since_last_trouble).count();
            uint32_t periods = static_cast<uint32_t>(hours_clean / (7 * 24));
            uint32_t decay = (periods < peer.ban_count) ? periods : peer.ban_count;
            peer.ban_count -= decay;
        }
    }
}

void PeerScoringManager::cleanupOldEntries() {
    auto now = std::chrono::system_clock::now();
    auto cleanup_threshold = now - std::chrono::hours(24 * 30); // 30 days
    
    for (auto it = peer_scores_.begin(); it != peer_scores_.end();) {
        const auto& peer = it->second;
        
        // Remove old entries with no recent activity and no ban
        if (peer.score == 0 && !peer.is_banned && 
            peer.last_misbehavior < cleanup_threshold) {
            it = peer_scores_.erase(it);
        } else {
            ++it;
        }
    }
}

int32_t PeerScoringManager::getMisbehaviorScore(MisbehaviorType type) const {
    return static_cast<int32_t>(type);
}

std::chrono::seconds PeerScoringManager::calculateBanDuration(int32_t score) const {
    // Exponential ban duration escalation based on score AND repeat ban count
    // 1st ban: 1h, 2nd: 4h, 3rd: 24h, 4th+: 7d (capped at max_ban_duration_)
    //
    // Lookup the peer's ban_count for escalation. Since calculateBanDuration
    // is called from checkForBan which already incremented ban_count, we use
    // the current count as the escalation level.
    //
    // Score-based multiplier still applies on top of ban_count escalation.
    double score_multiplier = std::log2(std::max(score / ban_threshold_, 1));
    auto base = std::chrono::seconds(static_cast<int64_t>(
        default_ban_duration_.count() * (1.0 + score_multiplier)));

    return std::min(base, max_ban_duration_);
}

// DoSProtection implementation
bool DoSProtection::checkRateLimit(const std::string& peer_id, 
                                  const std::string& request_type,
                                  size_t max_per_minute) {
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    std::string key = peer_id + ":" + request_type;
    
    auto& last_time = last_request_times_[key];
    auto& count = request_counts_[key];
    
    // Reset counter if more than a minute has passed
    if (now - last_time > std::chrono::minutes(1)) {
        count = 0;
        last_time = now;
    }
    
    count++;
    return count <= max_per_minute;
}

bool DoSProtection::validateMessageSize(const std::string& message_type, size_t message_size) {
    // Define maximum sizes for different message types
    static const std::unordered_map<std::string, size_t> max_sizes = {
        {"version", 1024},
        {"addr", 30000},      // ~1000 addresses
        {"inv", 50000},       // ~1250 inventory items
        {"getdata", 50000},   // ~1250 requests
        {"headers", 162000},  // ~2000 headers
        {"block", 8000000},   // 8MB max block size
        {"tx", 1000000},      // 1MB max transaction
        {"ping", 8},
        {"pong", 8}
    };
    
    auto it = max_sizes.find(message_type);
    if (it == max_sizes.end()) {
        return message_size <= 1000000; // Default 1MB limit
    }
    
    return message_size <= it->second;
}

bool DoSProtection::checkConnectionLimit(const std::string& peer_ip, size_t max_per_ip) {
    if (peer_ip.empty() || max_per_ip == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    auto now = std::chrono::steady_clock::now();
    auto& attempts = connection_attempts_[peer_ip];

    // Sliding window for connection attempts from one IP.
    attempts.erase(
        std::remove_if(
            attempts.begin(),
            attempts.end(),
            [now](const std::chrono::steady_clock::time_point& ts) {
                return (now - ts) > std::chrono::minutes(5);
            }),
        attempts.end());

    if (attempts.size() >= max_per_ip) {
        return false;
    }

    attempts.push_back(now);
    return true;
}

void DoSProtection::trackBandwidth(const std::string& peer_id, 
                                  size_t bytes_sent, 
                                  size_t bytes_received) {
    std::lock_guard<std::mutex> lock(bandwidth_mutex_);
    
    auto& stats = bandwidth_stats_[peer_id];
    stats.bytes_sent += bytes_sent;
    stats.bytes_received += bytes_received;
    
    // Update rates (simplified - would need proper time-based calculation)
    static auto last_update = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_update).count();
    
    if (elapsed > 0) {
        stats.send_rate = static_cast<double>(bytes_sent) / elapsed;
        stats.recv_rate = static_cast<double>(bytes_received) / elapsed;
        last_update = now;
    }
}

DoSProtection::BandwidthStats DoSProtection::getBandwidthStats(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(bandwidth_mutex_);
    
    auto it = bandwidth_stats_.find(peer_id);
    return (it != bandwidth_stats_.end()) ? it->second : BandwidthStats{};
}

// Persistence implementation
bool PeerScoringManager::saveToFile(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        return false;
    }

    // Write version (v2: adds ban_count persistence)
    uint32_t version = 2;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Write number of entries
    size_t num_entries = peer_scores_.size();
    file.write(reinterpret_cast<const char*>(&num_entries), sizeof(num_entries));

    // Write each peer entry
    for (const auto& [peer_id, peer] : peer_scores_) {
        // Only save banned peers or peers with significant scores
        if (!peer.is_banned && peer.score < 50) {
            continue;
        }

        // Write peer ID length and data
        size_t id_len = peer_id.size();
        file.write(reinterpret_cast<const char*>(&id_len), sizeof(id_len));
        file.write(peer_id.c_str(), id_len);

        // Write peer data
        file.write(reinterpret_cast<const char*>(&peer.score), sizeof(peer.score));
        file.write(reinterpret_cast<const char*>(&peer.lifetime_score), sizeof(peer.lifetime_score));
        file.write(reinterpret_cast<const char*>(&peer.misbehavior_count), sizeof(peer.misbehavior_count));
        file.write(reinterpret_cast<const char*>(&peer.is_banned), sizeof(peer.is_banned));
        file.write(reinterpret_cast<const char*>(&peer.ban_count), sizeof(peer.ban_count));

        // Write timestamps as time_t
        auto first_seen_t = std::chrono::system_clock::to_time_t(peer.first_seen);
        auto last_misbehavior_t = std::chrono::system_clock::to_time_t(peer.last_misbehavior);
        auto ban_until_t = std::chrono::system_clock::to_time_t(peer.ban_until);

        file.write(reinterpret_cast<const char*>(&first_seen_t), sizeof(first_seen_t));
        file.write(reinterpret_cast<const char*>(&last_misbehavior_t), sizeof(last_misbehavior_t));
        file.write(reinterpret_cast<const char*>(&ban_until_t), sizeof(ban_until_t));
    }

    return file.good();
}

bool PeerScoringManager::loadFromFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        return false;
    }

    // Read version (v1 = original, v2 = adds ban_count)
    uint32_t version = 0;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1 && version != 2) {
        return false;  // Unsupported version
    }

    // Read number of entries
    size_t num_entries = 0;
    file.read(reinterpret_cast<char*>(&num_entries), sizeof(num_entries));

    auto now = std::chrono::system_clock::now();

    // Read each peer entry
    for (size_t i = 0; i < num_entries && file.good(); ++i) {
        // Read peer ID
        size_t id_len = 0;
        file.read(reinterpret_cast<char*>(&id_len), sizeof(id_len));

        std::string peer_id(id_len, '\0');
        file.read(&peer_id[0], id_len);

        PeerScore peer;
        peer.peer_id = peer_id;

        // Read peer data
        file.read(reinterpret_cast<char*>(&peer.score), sizeof(peer.score));
        file.read(reinterpret_cast<char*>(&peer.lifetime_score), sizeof(peer.lifetime_score));
        file.read(reinterpret_cast<char*>(&peer.misbehavior_count), sizeof(peer.misbehavior_count));
        file.read(reinterpret_cast<char*>(&peer.is_banned), sizeof(peer.is_banned));

        // v2: read ban_count (v1 files default to 0)
        if (version >= 2) {
            file.read(reinterpret_cast<char*>(&peer.ban_count), sizeof(peer.ban_count));
        } else {
            peer.ban_count = 0;
        }

        // Read timestamps
        std::time_t first_seen_t, last_misbehavior_t, ban_until_t;
        file.read(reinterpret_cast<char*>(&first_seen_t), sizeof(first_seen_t));
        file.read(reinterpret_cast<char*>(&last_misbehavior_t), sizeof(last_misbehavior_t));
        file.read(reinterpret_cast<char*>(&ban_until_t), sizeof(ban_until_t));

        peer.first_seen = std::chrono::system_clock::from_time_t(first_seen_t);
        peer.last_misbehavior = std::chrono::system_clock::from_time_t(last_misbehavior_t);
        peer.ban_until = std::chrono::system_clock::from_time_t(ban_until_t);

        // Only restore if ban is still active or score is significant
        if ((peer.is_banned && now < peer.ban_until) || peer.score >= 50) {
            peer_scores_[peer_id] = peer;
        }
    }

    return file.good() || file.eof();
}

// Architecture V3: Initialization now handled via P2PService in DaemonContext

} // namespace p2p
} // namespace dinero
