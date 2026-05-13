#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <vector>

namespace dinero {
namespace p2p {

/**
 * Peer misbehavior types for scoring
 */
enum class MisbehaviorType {
    INVALID_BLOCK = 100,        // Sent invalid block
    INVALID_TRANSACTION = 10,   // Sent invalid transaction
    INVALID_HEADER = 50,        // Sent invalid header
    PROTOCOL_VIOLATION = 20,    // Protocol violation
    EXCESSIVE_REQUESTS = 5,     // Too many requests
    TIMEOUT = 1,                // Request timeout
    DUPLICATE_MESSAGE = 2,      // Duplicate message
    OVERSIZED_MESSAGE = 10,     // Message too large
    UNSOLICITED_DATA = 5,       // Unsolicited block/tx
    VERSION_MISMATCH = 10,      // Incompatible version
    NETWORK_MISMATCH = 100,     // Wrong network
    SPAM_BEHAVIOR = 15          // Spam-like behavior
};

/**
 * Peer scoring entry
 */
struct PeerScore {
    std::string peer_id;
    int32_t score;              // Current DoS score
    int32_t lifetime_score;     // Cumulative lifetime score
    uint32_t misbehavior_count; // Number of misbehaviors
    uint32_t ban_count;         // Number of times this peer has been banned (for escalation)
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_misbehavior;
    std::chrono::system_clock::time_point ban_until;
    bool is_banned;

    // Misbehavior history
    std::vector<std::pair<MisbehaviorType, std::chrono::system_clock::time_point>> history;

    PeerScore() : score(0), lifetime_score(0), misbehavior_count(0), ban_count(0), is_banned(false) {}
    
    void addMisbehavior(MisbehaviorType type);
    bool shouldBan(int32_t ban_threshold) const;
    void decay(double decay_rate);
};

/**
 * Peer scoring and DoS protection system
 * 
 * Tracks peer behavior and implements banscore-based DoS protection:
 * - Assigns scores for various misbehaviors
 * - Automatically bans peers exceeding thresholds
 * - Provides score decay over time
 * - Maintains ban lists and statistics
 */
class PeerScoringManager {
public:
    PeerScoringManager();
    ~PeerScoringManager();
    
    /**
     * Add misbehavior score for a peer
     */
    void addMisbehavior(const std::string& peer_id, MisbehaviorType type);
    
    /**
     * Get current score for a peer
     */
    int32_t getScore(const std::string& peer_id) const;
    
    /**
     * Check if peer is banned
     */
    bool isBanned(const std::string& peer_id) const;
    
    /**
     * Manually ban a peer
     */
    void banPeer(const std::string& peer_id, std::chrono::seconds duration);
    
    /**
     * Unban a peer
     */
    void unbanPeer(const std::string& peer_id);
    
    /**
     * Get peer score details
     */
    PeerScore getPeerScore(const std::string& peer_id) const;
    
    /**
     * Get all banned peers
     */
    std::vector<std::string> getBannedPeers() const;
    
    /**
     * Statistics
     */
    struct ScoringStats {
        size_t total_peers;
        size_t banned_peers;
        size_t misbehaving_peers;  // Score > 0
        int32_t avg_score;
        uint64_t total_misbehaviors;
        std::chrono::system_clock::time_point oldest_ban;
    };
    
    ScoringStats getStats() const;
    
    /**
     * Maintenance operations
     */
    void performMaintenance();  // Decay scores, cleanup expired bans
    void clearExpiredBans();
    void clearAllBans();
    void resetScores();
    
    /**
     * Configuration
     */
    void setBanThreshold(int32_t threshold);
    void setDecayRate(double rate);           // Score decay per hour
    void setDefaultBanDuration(std::chrono::seconds duration);
    void setMaxBanDuration(std::chrono::seconds duration);
    void setHistorySize(size_t max_history);
    
    /**
     * Persistence
     */
    bool saveToFile(const std::string& filename) const;
    bool loadFromFile(const std::string& filename);

private:
    // Internal scoring logic
    void updatePeerScore(const std::string& peer_id, MisbehaviorType type);
    void checkForBan(const std::string& peer_id);
    void decayScores();
    void decayBanCounts(std::chrono::system_clock::time_point now);
    void cleanupOldEntries();
    
    // Score calculation helpers
    int32_t getMisbehaviorScore(MisbehaviorType type) const;
    std::chrono::seconds calculateBanDuration(int32_t score) const;
    
    mutable std::mutex mutex_;
    
    // Peer scoring data
    std::unordered_map<std::string, PeerScore> peer_scores_;
    
    // Configuration
    int32_t ban_threshold_;
    double decay_rate_;                    // Score decay per hour
    std::chrono::seconds default_ban_duration_;
    std::chrono::seconds max_ban_duration_;
    size_t max_history_size_;
    
    // Statistics
    std::atomic<uint64_t> total_misbehaviors_{0};
    std::atomic<uint64_t> total_bans_{0};
    std::chrono::system_clock::time_point last_maintenance_;
    
    // Constants
    static constexpr int32_t DEFAULT_BAN_THRESHOLD = 100;
    static constexpr double DEFAULT_DECAY_RATE = 0.1;  // 10% per hour
    static constexpr std::chrono::hours DEFAULT_BAN_DURATION{24};
    static constexpr std::chrono::hours MAX_BAN_DURATION{30 * 24}; // 30 days
    static constexpr size_t DEFAULT_HISTORY_SIZE = 100;
    static constexpr std::chrono::hours MAINTENANCE_INTERVAL{1};
};

/**
 * DoS protection utilities
 */
class DoSProtection {
public:
    /**
     * Rate limiting for peer requests
     */
    static bool checkRateLimit(const std::string& peer_id, 
                              const std::string& request_type,
                              size_t max_per_minute = 60);
    
    /**
     * Message size validation
     */
    static bool validateMessageSize(const std::string& message_type, 
                                   size_t message_size);
    
    /**
     * Connection limit enforcement
     */
    static bool checkConnectionLimit(const std::string& peer_ip, 
                                    size_t max_per_ip = 8);
    
    /**
     * Bandwidth monitoring
     */
    static void trackBandwidth(const std::string& peer_id, 
                              size_t bytes_sent, 
                              size_t bytes_received);
    
    /**
     * Get bandwidth stats for peer
     */
    struct BandwidthStats {
        uint64_t bytes_sent;
        uint64_t bytes_received;
        double send_rate;    // bytes per second
        double recv_rate;    // bytes per second
    };
    
    static BandwidthStats getBandwidthStats(const std::string& peer_id);

private:
    static std::mutex rate_limit_mutex_;
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_request_times_;
    static std::unordered_map<std::string, uint32_t> request_counts_;
    static std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> connection_attempts_;
    
    static std::mutex bandwidth_mutex_;
    static std::unordered_map<std::string, BandwidthStats> bandwidth_stats_;
};

// Architecture V3: No globals
// PeerScoringManager is now managed via DaemonContext
// Access via: ctx->peer_scoring or P2PService::getPeerScoring()

} // namespace p2p
} // namespace dinero
