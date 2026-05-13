#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <cstdint>

// Per-connection outbound queue limits (can be tuned at runtime later)
#ifndef DIN_WS_MAX_QUEUE_BYTES
#define DIN_WS_MAX_QUEUE_BYTES (2 * 1024 * 1024)  // 2 MB per connection
#endif

// Replay buffer depth per topic (configurable)
#ifndef DIN_WS_REPLAY_DEPTH
#define DIN_WS_REPLAY_DEPTH 100
#endif

// Compile-time sampling for mempoolTx (0 = off)
#ifndef DIN_MEMPOOL_SAMPLE_RATE
#define DIN_MEMPOOL_SAMPLE_RATE 0
#endif

// Forward decl (implemented in ws_frame_handler.hpp)
bool ws_send_text(int fd, const std::string& s);

// Phase 2.2: Authentication levels for topic access control
enum class AuthLevel {
    NONE = 0,           // Not authenticated
    AUTHENTICATED = 1,  // Valid cookie or static credentials
    ADMIN = 2           // Admin-level access (future use)
};

// Phase 2.2: Per-connection metadata for authentication and tracking
struct ConnectionMetadata {
    std::string client_ip;
    AuthLevel auth_level = AuthLevel::NONE;
    std::string username;  // Empty if not authenticated
    int64_t connected_at = 0;  // Unix timestamp
    std::unordered_set<std::string> subscriptions;  // Active topic subscriptions
};

class Subscriptions {
private:
    struct OutItem {
        std::string channel;
        std::string payload; // JSON (already serialized)
    };
    struct ConnQ {
        std::deque<OutItem> q;
        std::atomic<size_t> bytes{0};
        // Track the newest miningInfo index to coalesce
        // (-1 means none queued)
        int lastMiningIdx = -1;
    };

    // Per-topic replay buffer for historical event replay
    struct ReplayBuffer {
        std::deque<std::string> events;  // Recent JSON events
        std::atomic<uint64_t> seq{0};     // Per-topic sequence counter
        size_t max_depth = DIN_WS_REPLAY_DEPTH;

        ReplayBuffer() = default;
        ReplayBuffer(const ReplayBuffer& other)
            : events(other.events), seq(other.seq.load()), max_depth(other.max_depth) {}
    };

public:
    // Register/Unregister connections & channel membership
    void add_connection(int fd);
    void remove_connection(int fd);
    void subscribe(int fd, const std::string& channel);
    void unsubscribe(int fd, const std::string& channel);

    // Enqueue (non-blocking). Coalesces lossy channels, bounds memory, may drop.
    void enqueue(const std::string& channel, const std::string& json);

    // Drain queued frames to sockets (non-blocking; safe to call from a loop)
    void drain_once();

    // Phase 2.1: Replay support
    // Replay recent events for a topic (count-based)
    std::vector<std::string> replay(const std::string& topic, size_t count);

    // Replay events by sequence range
    std::vector<std::string> replay_range(const std::string& topic, uint64_t from_seq, uint64_t to_seq);

    // Get current sequence number for a topic
    uint64_t get_topic_seq(const std::string& topic);

    // Get next sequence for a topic (reserves it for use)
    uint64_t get_next_topic_seq(const std::string& topic);

    // Send replay data to specific connection
    void send_replay_to_client(int fd, const std::string& topic, const std::vector<std::string>& events);

    // Phase 2.2: Authentication and connection management
    // Set connection metadata (IP, auth level, username)
    void set_connection_metadata(int fd, const std::string& client_ip, AuthLevel auth_level, const std::string& username);

    // Get connection metadata
    ConnectionMetadata get_connection_metadata(int fd) const;

    // Check if connection has required auth level for topic
    bool check_topic_auth(int fd, const std::string& topic) const;

    // Subscribe with auth check
    bool subscribe_with_auth(int fd, const std::string& topic);

    // Get list of all active connections with metadata
    std::vector<std::pair<int, ConnectionMetadata>> get_all_connections() const;

    // Get per-topic statistics
    struct TopicStats {
        std::string topic;
        size_t subscriber_count = 0;
        uint64_t events_sent = 0;
        uint64_t events_dropped = 0;
        uint64_t current_seq = 0;
    };
    std::vector<TopicStats> get_topic_stats() const;

private:
    // Helper method for shedding non-critical messages when backpressure occurs
    bool shed_noncritical_messages(ConnQ& q);

    // Helper to store event in replay buffer
    void store_in_replay_buffer(const std::string& topic, const std::string& json, uint64_t seq);

    mutable std::mutex mu_;  // Mutable allows locking in const methods
    std::unordered_map<std::string, std::unordered_set<int>> subscribers_;
    std::unordered_set<int> conns_;
    std::unordered_map<int, ConnQ> queues_; // fd -> queue state

    // Phase 2.1: Replay buffers per topic
    std::unordered_map<std::string, ReplayBuffer> replay_buffers_;

    // Phase 2.2: Connection metadata for authentication tracking
    std::unordered_map<int, ConnectionMetadata> conn_metadata_;

    // Phase 2.2: Per-topic statistics
    struct TopicStatsInternal {
        std::atomic<uint64_t> events_sent{0};
        std::atomic<uint64_t> events_dropped{0};
    };
    std::unordered_map<std::string, TopicStatsInternal> topic_stats_;

    // Phase 2.2: Topic permission requirements
    // Map of topic -> required auth level
    std::unordered_map<std::string, AuthLevel> topic_auth_requirements_;

    // Helper to initialize default topic permissions
    void init_topic_permissions();

    // Helper to get required auth level for topic
    AuthLevel get_topic_auth_requirement(const std::string& topic) const;

    // Metrics hooks (provided by metrics singleton)
    void on_sent_bytes(uint64_t n);
    void on_event_sent(const std::string& ch);
    void on_event_dropped(const std::string& ch, const char* reason);
};
