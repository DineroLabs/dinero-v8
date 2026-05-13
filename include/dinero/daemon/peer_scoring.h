#pragma once
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <chrono>
#include <atomic>
#include <vector>

namespace dinero {

// Peer scoring events
enum class PeerEvent {
    CONNECTION_ATTEMPT,
    CONNECTION_SUCCESS,
    CONNECTION_FAILURE,
    MESSAGE_RECEIVED,
    MESSAGE_SENT,
    INVALID_MESSAGE,
    BLOCK_RECEIVED,
    BLOCK_VALID,
    BLOCK_INVALID,
    TRANSACTION_RECEIVED,
    TRANSACTION_VALID,
    TRANSACTION_INVALID,
    PING_RECEIVED,
    PONG_RECEIVED,
    TIMEOUT,
    BAN_TRIGGER,
    // Confidential transaction events
    CONFIDENTIAL_TX_RECEIVED,
    CONFIDENTIAL_TX_VALID,
    CONFIDENTIAL_TX_INVALID,
    CONFIDENTIAL_TX_OVERSIZED,
    CONFIDENTIAL_TX_INVALID_PROOF,
    CONFIDENTIAL_TX_MALFORMED_PROOF,
    CONFIDENTIAL_TX_TOO_MANY_OUTPUTS,
    CONFIDENTIAL_TX_FLOOD_ATTEMPT
};

// Peer score information
struct PeerScore {
    std::string peer_id;
    int32_t score;
    uint32_t connection_attempts;
    uint32_t connection_successes;
    uint32_t messages_received;
    uint32_t messages_sent;
    uint32_t invalid_messages;
    uint32_t blocks_received;
    uint32_t valid_blocks;
    uint32_t invalid_blocks;
    uint32_t transactions_received;
    uint32_t valid_transactions;
    uint32_t invalid_transactions;
    uint32_t timeouts;

    // Confidential transaction tracking
    uint32_t confidential_txs_received;
    uint32_t confidential_txs_valid;
    uint32_t confidential_txs_invalid;
    uint32_t confidential_txs_oversized;
    uint32_t confidential_invalid_proofs;
    uint32_t confidential_malformed_proofs;
    uint32_t confidential_flood_attempts;
    std::chrono::time_point<std::chrono::steady_clock> last_confidential_tx;

    std::chrono::time_point<std::chrono::steady_clock> last_activity;
    std::chrono::time_point<std::chrono::steady_clock> last_ban;
    bool banned;
    std::chrono::time_point<std::chrono::steady_clock> ban_until;

    PeerScore() : score(0), connection_attempts(0), connection_successes(0),
                  messages_received(0), messages_sent(0), invalid_messages(0),
                  blocks_received(0), valid_blocks(0), invalid_blocks(0),
                  transactions_received(0), valid_transactions(0), invalid_transactions(0),
                  timeouts(0),
                  confidential_txs_received(0), confidential_txs_valid(0),
                  confidential_txs_invalid(0), confidential_txs_oversized(0),
                  confidential_invalid_proofs(0), confidential_malformed_proofs(0),
                  confidential_flood_attempts(0),
                  banned(false) {
        auto now = std::chrono::steady_clock::now();
        last_activity = now;
        last_ban = now;
        ban_until = now;
        last_confidential_tx = now;
    }
};

// Peer scoring and banning manager
class PeerScoring {
public:
    PeerScoring();
    ~PeerScoring();
    
    // Peer management
    void addPeer(const std::string& peer_id);
    void removePeer(const std::string& peer_id);
    bool hasPeer(const std::string& peer_id) const;
    
    // Event handling
    void recordEvent(const std::string& peer_id, PeerEvent event);
    void recordEvent(const std::string& peer_id, PeerEvent event, int32_t score_delta);
    
    // Scoring
    int32_t getScore(const std::string& peer_id) const;
    PeerScore getPeerScore(const std::string& peer_id) const;
    std::vector<std::string> getTopPeers(size_t count = 10) const;
    std::vector<std::string> getBannedPeers() const;
    
    // Banning
    bool isBanned(const std::string& peer_id) const;
    void banPeer(const std::string& peer_id, std::chrono::seconds duration);
    void unbanPeer(const std::string& peer_id);
    void unbanAllPeers();
    
    // Maintenance
    void cleanup();
    void updateScores();
    
    // Statistics
    struct ScoringStats {
        uint32_t total_peers;
        uint32_t banned_peers;
        uint32_t active_peers;
        int32_t avg_score;
        uint32_t total_events;
        uint32_t ban_events;
    };
    ScoringStats getStats() const;
    
    // Confidential transaction rate limiting
    bool checkConfidentialTxRateLimit(const std::string& peer_id);
    bool isConfidentialTxFlood(const std::string& peer_id);
    void recordConfidentialTx(const std::string& peer_id, bool valid, bool oversized,
                               bool invalid_proof, bool malformed_proof);

    // Configuration
    static constexpr int32_t MAX_SCORE = 1000;
    static constexpr int32_t MIN_SCORE = -1000;
    static constexpr int32_t BAN_THRESHOLD = -100;
    static constexpr std::chrono::seconds DEFAULT_BAN_DURATION{3600}; // 1 hour
    static constexpr std::chrono::seconds CLEANUP_INTERVAL{300}; // 5 minutes

    // Confidential transaction rate limits
    static constexpr uint32_t MAX_CONFIDENTIAL_TX_PER_MINUTE = 10;
    static constexpr uint32_t MAX_INVALID_PROOFS_PER_HOUR = 5;
    static constexpr uint32_t MAX_MALFORMED_PROOFS_BEFORE_BAN = 3;
    static constexpr std::chrono::seconds CONFIDENTIAL_TX_RATE_WINDOW{60}; // 1 minute

private:
    // Helper methods
    void updateScore(PeerScore& peer_score, int32_t delta);
    bool shouldBan(const PeerScore& peer_score) const;
    void applyBan(PeerScore& peer_score, std::chrono::seconds duration);
    void removeBan(PeerScore& peer_score);
    int32_t getEventScore(PeerEvent event) const;
    
    // Thread safety
    mutable std::mutex m_mutex;
    
    // Peer scores
    std::map<std::string, PeerScore> m_peer_scores;
    
    // Statistics
    ScoringStats m_stats;
    mutable std::mutex m_stats_mutex;
    
    // Configuration
    std::atomic<bool> m_enabled{true};
    std::atomic<int32_t> m_ban_threshold{BAN_THRESHOLD};
    std::atomic<std::chrono::seconds> m_default_ban_duration{DEFAULT_BAN_DURATION};
};

} // namespace dinero
