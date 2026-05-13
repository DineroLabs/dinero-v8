#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>

// Forward declaration for sqlite3
struct sqlite3;
struct sqlite3_stmt;

namespace dinero {

// Misbehavior types for tracking peer violations
enum class MisbehaviorType {
    INVALID_BLOCK = 1,
    INVALID_TX = 2,
    PROTOCOL_VIOLATION = 3,
    TIMEOUT = 4,
    DUPLICATE_MESSAGE = 5,
    EXCESSIVE_INV = 6,
    STALE_DATA = 7
};

// Peer reputation entry
struct PeerReputation {
    std::string ip;
    uint16_t port;
    int32_t score;
    uint32_t connection_attempts;
    uint32_t successful_connections;
    uint32_t failed_connections;
    uint32_t ban_count;
    bool is_banned;
    std::chrono::system_clock::time_point ban_until;
    std::chrono::system_clock::time_point last_seen;
    std::chrono::system_clock::time_point created_at;

    // Misbehavior counts by type
    uint32_t invalid_blocks;
    uint32_t invalid_txs;
    uint32_t protocol_violations;

    // Week 7: Positive reputation metrics
    uint64_t total_uptime_seconds;      // Cumulative uptime across all connections
    uint64_t connection_start_time;     // Timestamp when current connection started (0 if disconnected)
    double avg_latency_ms;              // Average ping latency (exponential moving average)
    double reliability_ratio;          // successful_connections / connection_attempts (0.0-1.0)

    PeerReputation() : port(0), score(0), connection_attempts(0),
                       successful_connections(0), failed_connections(0),
                       ban_count(0), is_banned(false),
                       invalid_blocks(0), invalid_txs(0), protocol_violations(0),
                       total_uptime_seconds(0), connection_start_time(0),
                       avg_latency_ms(0.0), reliability_ratio(0.0) {
        auto now = std::chrono::system_clock::now();
        last_seen = now;
        created_at = now;
        ban_until = now;
    }
};

// SQLite-backed peer reputation database
class PeerReputationDB {
public:
    PeerReputationDB();
    ~PeerReputationDB();

    // Database lifecycle
    bool open(const std::string& db_path);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    // Peer lifecycle
    bool addOrUpdatePeer(const std::string& ip, uint16_t port);
    bool getPeer(const std::string& ip, uint16_t port, PeerReputation& out) const;
    bool removePeer(const std::string& ip, uint16_t port);

    // Connection tracking
    bool recordAttempt(const std::string& ip, uint16_t port);
    bool recordSuccess(const std::string& ip, uint16_t port);
    bool recordFailure(const std::string& ip, uint16_t port);

    // Misbehavior tracking
    bool recordMisbehavior(const std::string& ip, uint16_t port, MisbehaviorType type);
    bool incrementMisbehaviorCount(const std::string& ip, uint16_t port, MisbehaviorType type);

    // Ban management with escalation
    bool banPeer(const std::string& ip, uint16_t port, std::chrono::seconds duration);
    bool unbanPeer(const std::string& ip, uint16_t port);
    bool isPeerBanned(const std::string& ip, uint16_t port) const;
    std::chrono::seconds getNextBanDuration(uint32_t ban_count) const;

    // Score management
    bool adjustScore(const std::string& ip, uint16_t port, int32_t delta);
    bool resetScore(const std::string& ip, uint16_t port);

    // Week 7: Positive reputation metrics
    bool recordConnectionStart(const std::string& ip, uint16_t port);
    bool recordConnectionEnd(const std::string& ip, uint16_t port);
    bool updateLatency(const std::string& ip, uint16_t port, double latency_ms);
    double calculateReliability(const std::string& ip, uint16_t port) const;
    uint64_t getUptime(const std::string& ip, uint16_t port) const; // Returns current uptime in seconds

    // Query operations
    std::vector<PeerReputation> getGoodPeers(size_t limit = 100) const;
    std::vector<PeerReputation> getBannedPeers() const;
    std::vector<PeerReputation> getPeersByScore(int32_t min_score, size_t limit = 100) const;

    // Maintenance
    bool cleanupStalePeers(std::chrono::seconds max_age);
    bool expireBans();
    bool vacuum();

    // Statistics
    struct Stats {
        uint64_t total_peers;
        uint64_t banned_peers;
        uint64_t good_peers;
        uint64_t total_misbehaviors;
    };
    Stats getStats() const;

    // Ban escalation configuration
    static constexpr std::chrono::seconds FIRST_BAN_DURATION{60 * 60};        // 1 hour
    static constexpr std::chrono::seconds SECOND_BAN_DURATION{24 * 60 * 60};  // 1 day
    static constexpr std::chrono::seconds THIRD_BAN_DURATION{7 * 24 * 60 * 60}; // 1 week
    static constexpr std::chrono::seconds PERMANENT_BAN_DURATION{365 * 24 * 60 * 60}; // 1 year

    // Score thresholds
    static constexpr int32_t MAX_SCORE = 100;
    static constexpr int32_t MIN_SCORE = -100;
    static constexpr int32_t GOOD_PEER_THRESHOLD = 10;
    static constexpr int32_t AUTO_BAN_THRESHOLD = -50;

    // Misbehavior penalties
    static constexpr int32_t INVALID_BLOCK_PENALTY = -25;
    static constexpr int32_t INVALID_TX_PENALTY = -10;
    static constexpr int32_t PROTOCOL_VIOLATION_PENALTY = -15;
    static constexpr int32_t TIMEOUT_PENALTY = -5;
    static constexpr int32_t EXCESSIVE_INV_PENALTY = -8;

private:
    // Internal helpers
    bool createSchema();
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
    std::string getPeerKey(const std::string& ip, uint16_t port) const;

    // Prepared statements
    bool prepareStatements();
    void finalizeStatements();

    sqlite3* m_db;

    // Cached prepared statements for performance
    sqlite3_stmt* m_stmt_insert_peer;
    sqlite3_stmt* m_stmt_get_peer;
    sqlite3_stmt* m_stmt_update_score;
    sqlite3_stmt* m_stmt_record_attempt;
    sqlite3_stmt* m_stmt_record_success;
    sqlite3_stmt* m_stmt_record_failure;
    sqlite3_stmt* m_stmt_ban_peer;
    sqlite3_stmt* m_stmt_unban_peer;
    sqlite3_stmt* m_stmt_check_ban;
};

} // namespace dinero
