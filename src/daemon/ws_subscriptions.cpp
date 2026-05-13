#include "daemon/ws_subscriptions.hpp"
#include "daemon/websocket_metrics.hpp"
#include "common/logger.h"
#include <algorithm>
#include <cstring>

extern WebSocketMetrics g_websocket_metrics;

void Subscriptions::add_connection(int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    conns_.insert(fd);
    queues_[fd]; // Default construct in place
    g_websocket_metrics.ws_connections_current++;
    g_websocket_metrics.ws_connections_accepted_total++;
}

void Subscriptions::remove_connection(int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    conns_.erase(fd);
    queues_.erase(fd);
    
    // Remove from all channel subscriptions
    for (auto& [channel, fds] : subscribers_) {
        fds.erase(fd);
    }
    
    g_websocket_metrics.ws_connections_current--;
    g_websocket_metrics.ws_connections_closed_total++;
}

void Subscriptions::subscribe(int fd, const std::string& channel) {
    std::lock_guard<std::mutex> lk(mu_);
    subscribers_[channel].insert(fd);
}

void Subscriptions::unsubscribe(int fd, const std::string& channel) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = subscribers_.find(channel);
    if (it != subscribers_.end()) {
        it->second.erase(fd);
    }
}

void Subscriptions::enqueue(const std::string& channel, const std::string& json) {
    std::lock_guard<std::mutex> lk(mu_);

    // Apply compile-time sampling for mempoolTx if enabled
    if (channel == "mempoolTx" && DIN_MEMPOOL_SAMPLE_RATE > 0) {
        static std::atomic<uint64_t> counter{0};
        uint64_t rate = DIN_MEMPOOL_SAMPLE_RATE;
        if (++counter % rate != 0) {
            return;
        }
    }

    // Phase 2.1: Store in replay buffer for historical replay
    // IMPORTANT: Always buffer events, even when no WebSocket clients are connected
    // This allows wsReplay RPC to return historical events
    // Note: Sequence is managed by publisher via get_next_topic_seq()
    store_in_replay_buffer(channel, json, 0);  // seq param unused, kept for API compat

    // Only send to WebSocket clients if subscribers exist
    auto it = subscribers_.find(channel);
    if (it == subscribers_.end()) return;  // No subscribers, but event is now buffered for replay

    // Create the JSON-RPC push envelope
    std::string msg = "{\"jsonrpc\":\"2.0\",\"method\":\"subscription\",\"params\":{"
        "\"subId\":\"" + channel + "\","
        "\"channel\":\"" + channel + "\","
        "\"result\":" + json + "}}";

    for (int fd : it->second) {
        auto qit = queues_.find(fd);
        if (qit == queues_.end()) continue;

        ConnQ& q = qit->second;
        size_t needed = msg.size() + 64; // Frame overhead estimate

        // Special handling for miningInfo (coalesce)
        if (channel == "miningInfo") {
            // Remove any existing miningInfo from queue
            if (q.lastMiningIdx >= 0) {
                auto& existing = q.q[q.lastMiningIdx];
                if (existing.channel == "miningInfo") {
                    q.bytes -= existing.payload.size() + 64;
                    q.q.erase(q.q.begin() + q.lastMiningIdx);
                    q.lastMiningIdx = -1;
                }
            }
        }

        // Check if we can fit this message
        if (q.bytes + needed > DIN_WS_MAX_QUEUE_BYTES) {
            // Try to shed non-critical messages first
            if (!shed_noncritical_messages(q)) {
                // Still can't fit, drop this message
                on_event_dropped(channel, "backpressure");
                g_websocket_metrics.ws_backpressure_drops++;
                continue;
            }
        }

        // Add to queue
        q.q.push_back({channel, msg});
        q.bytes += needed;

        // Track miningInfo position for coalescing
        if (channel == "miningInfo") {
            q.lastMiningIdx = static_cast<int>(q.q.size()) - 1;
        }
    }
}

bool Subscriptions::shed_noncritical_messages(ConnQ& q) {
    // Priority order: newBlocks > mempoolTx > miningInfo
    // Try to drop from lowest priority first
    
    // First try to drop miningInfo
    for (int i = static_cast<int>(q.q.size()) - 1; i >= 0; --i) {
        if (q.q[i].channel == "miningInfo") {
            q.bytes -= q.q[i].payload.size() + 64;
            q.q.erase(q.q.begin() + i);
            if (q.lastMiningIdx == i) q.lastMiningIdx = -1;
            else if (q.lastMiningIdx > i) q.lastMiningIdx--;
            on_event_dropped("miningInfo", "backpressure");
            g_websocket_metrics.dropped_miningInfo++;
            return true;
        }
    }
    
    // Then try mempoolTx
    for (int i = static_cast<int>(q.q.size()) - 1; i >= 0; --i) {
        if (q.q[i].channel == "mempoolTx") {
            q.bytes -= q.q[i].payload.size() + 64;
            q.q.erase(q.q.begin() + i);
            if (q.lastMiningIdx > i) q.lastMiningIdx--;
            on_event_dropped("mempoolTx", "backpressure");
            g_websocket_metrics.dropped_mempoolTx++;
            return true;
        }
    }
    
    // Can't shed any more (only newBlocks left)
    return false;
}

void Subscriptions::drain_once() {
    std::lock_guard<std::mutex> lk(mu_);
    
    // Round-robin across connections to be fair
    static size_t next_conn = 0;
    if (conns_.empty()) return;
    
    // Convert set to vector for indexing
    std::vector<int> conn_list(conns_.begin(), conns_.end());
    if (next_conn >= conn_list.size()) next_conn = 0;
    
    int fd = conn_list[next_conn++];
    auto qit = queues_.find(fd);
    if (qit == queues_.end()) return;
    
    ConnQ& q = qit->second;
    if (q.q.empty()) return;
    
    // Try to send one message
    OutItem& item = q.q.front();
    
    if (ws_send_text(fd, item.payload)) {
        // Success - remove from queue
        q.bytes -= item.payload.size() + 64;
        q.q.pop_front();
        
        // Update miningInfo index if needed
        if (item.channel == "miningInfo" && q.lastMiningIdx == 0) {
            q.lastMiningIdx = -1;
        } else if (q.lastMiningIdx > 0) {
            q.lastMiningIdx--;
        }
        
        // Update metrics
        on_sent_bytes(item.payload.size());
        on_event_sent(item.channel);
        
        // Update legacy counters for backward compatibility
        if (item.channel == "newBlocks") {
            g_websocket_metrics.out_newBlocks++;
            g_websocket_metrics.ws_out_total_newBlocks++;
        } else if (item.channel == "miningInfo") {
            g_websocket_metrics.out_miningInfo++;
            g_websocket_metrics.ws_out_total_miningInfo++;
        } else if (item.channel == "mempoolTx") {
            g_websocket_metrics.out_mempoolTx++;
            g_websocket_metrics.ws_out_total_mempoolTx++;
        }
        
        g_websocket_metrics.ws_events_sent_total++;
    }
}

void Subscriptions::on_sent_bytes(uint64_t n) {
    g_websocket_metrics.ws_sent_bytes_total += n;
}

void Subscriptions::on_event_sent(const std::string& ch) {
    // Channel-specific metrics are handled in drain_once()
}

void Subscriptions::on_event_dropped(const std::string& ch, const char* reason) {
    g_websocket_metrics.ws_dropped_events_total++;
    // Channel-specific drop counters are handled in shed_noncritical_messages()
}

// ============================================================================
// Phase 2.1: Replay Support Implementation
// ============================================================================

void Subscriptions::store_in_replay_buffer(const std::string& topic, const std::string& json, uint64_t seq) {
    // Get or create replay buffer for this topic
    auto& buffer = replay_buffers_[topic];

    // Store the event with sequence number embedded
    buffer.events.push_back(json);

    // Maintain max depth - trim oldest if exceeded
    if (buffer.events.size() > buffer.max_depth) {
        buffer.events.pop_front();
    }
}

std::vector<std::string> Subscriptions::replay(const std::string& topic, size_t count) {
    std::lock_guard<std::mutex> lk(mu_);

    std::vector<std::string> result;

    auto it = replay_buffers_.find(topic);
    if (it == replay_buffers_.end()) {
        return result;  // No replay buffer for this topic yet
    }

    const auto& buffer = it->second;

    // Return last 'count' events
    size_t start_idx = 0;
    if (buffer.events.size() > count) {
        start_idx = buffer.events.size() - count;
    }

    for (size_t i = start_idx; i < buffer.events.size(); ++i) {
        result.push_back(buffer.events[i]);
    }

    return result;
}

std::vector<std::string> Subscriptions::replay_range(const std::string& topic, uint64_t from_seq, uint64_t to_seq) {
    std::lock_guard<std::mutex> lk(mu_);

    std::vector<std::string> result;

    auto it = replay_buffers_.find(topic);
    if (it == replay_buffers_.end()) {
        return result;
    }

    const auto& buffer = it->second;
    uint64_t current_seq = buffer.seq.load();

    // Calculate oldest sequence we have in buffer
    uint64_t oldest_seq = 0;
    if (current_seq > buffer.events.size()) {
        oldest_seq = current_seq - buffer.events.size();
    }

    // Clamp range to available data
    if (from_seq < oldest_seq) from_seq = oldest_seq;
    if (to_seq > current_seq) to_seq = current_seq;

    if (from_seq >= to_seq) return result;

    // Convert sequence range to buffer indices
    size_t start_idx = from_seq - oldest_seq;
    size_t end_idx = to_seq - oldest_seq;

    if (end_idx > buffer.events.size()) end_idx = buffer.events.size();

    for (size_t i = start_idx; i < end_idx && i < buffer.events.size(); ++i) {
        result.push_back(buffer.events[i]);
    }

    return result;
}

uint64_t Subscriptions::get_topic_seq(const std::string& topic) {
    std::lock_guard<std::mutex> lk(mu_);

    auto it = replay_buffers_.find(topic);
    if (it == replay_buffers_.end()) {
        return 0;
    }

    return it->second.seq.load();
}

uint64_t Subscriptions::get_next_topic_seq(const std::string& topic) {
    std::lock_guard<std::mutex> lk(mu_);

    // Get or create replay buffer for this topic
    auto& buffer = replay_buffers_[topic];

    // Return current value and increment for next call
    return buffer.seq.fetch_add(1);
}

void Subscriptions::send_replay_to_client(int fd, const std::string& topic, const std::vector<std::string>& events) {
    std::lock_guard<std::mutex> lk(mu_);

    // Check if connection still exists
    if (conns_.find(fd) == conns_.end()) {
        return;
    }

    auto qit = queues_.find(fd);
    if (qit == queues_.end()) {
        return;
    }

    ConnQ& q = qit->second;

    // Build replay response
    std::string replay_msg = "{\"type\":\"replay_data\",\"topic\":\"" + topic + "\",\"events\":[";

    for (size_t i = 0; i < events.size(); ++i) {
        if (i > 0) replay_msg += ",";
        replay_msg += events[i];
    }

    replay_msg += "]}";

    // Add to queue (with size check)
    size_t needed = replay_msg.size() + 64;

    if (q.bytes + needed > DIN_WS_MAX_QUEUE_BYTES) {
        // Try to shed messages to make room
        if (!shed_noncritical_messages(q)) {
            // Can't fit replay data - drop it
            on_event_dropped(topic, "replay_backpressure");
            return;
        }
    }

    q.q.push_back({"replay", replay_msg});
    q.bytes += needed;
}

// ============================================================================
// Phase 2.2: Authentication and Connection Management Implementation
// ============================================================================

void Subscriptions::init_topic_permissions() {
    // Initialize default topic permissions
    // Public topics - anyone can subscribe
    topic_auth_requirements_["newBlocks"] = AuthLevel::NONE;

    // Authenticated topics - require valid credentials
    topic_auth_requirements_["mempoolTx"] = AuthLevel::AUTHENTICATED;
    topic_auth_requirements_["miningInfo"] = AuthLevel::AUTHENTICATED;
    topic_auth_requirements_["activity"] = AuthLevel::AUTHENTICATED;

    // Future admin-only topics
    topic_auth_requirements_["wallet"] = AuthLevel::ADMIN;
    topic_auth_requirements_["admin"] = AuthLevel::ADMIN;
}

AuthLevel Subscriptions::get_topic_auth_requirement(const std::string& topic) const {
    auto it = topic_auth_requirements_.find(topic);
    if (it != topic_auth_requirements_.end()) {
        return it->second;
    }

    // Default: require authentication for unknown topics
    return AuthLevel::AUTHENTICATED;
}

void Subscriptions::set_connection_metadata(int fd, const std::string& client_ip,
                                            AuthLevel auth_level, const std::string& username) {
    std::lock_guard<std::mutex> lk(mu_);

    // Initialize topic permissions if not done yet
    if (topic_auth_requirements_.empty()) {
        init_topic_permissions();
    }

    auto& metadata = conn_metadata_[fd];
    metadata.client_ip = client_ip;
    metadata.auth_level = auth_level;
    metadata.username = username;
    metadata.connected_at = std::time(nullptr);
}

ConnectionMetadata Subscriptions::get_connection_metadata(int fd) const {
    std::lock_guard<std::mutex> lk(mu_);

    auto it = conn_metadata_.find(fd);
    if (it != conn_metadata_.end()) {
        return it->second;
    }

    // Return empty metadata if not found
    return ConnectionMetadata{};
}

bool Subscriptions::check_topic_auth(int fd, const std::string& topic) const {
    std::lock_guard<std::mutex> lk(mu_);

    // Get required auth level for topic
    AuthLevel required_level = get_topic_auth_requirement(topic);

    // Get connection metadata
    auto it = conn_metadata_.find(fd);
    if (it == conn_metadata_.end()) {
        // No metadata = not authenticated
        return required_level == AuthLevel::NONE;
    }

    // Check if connection's auth level meets requirement
    return static_cast<int>(it->second.auth_level) >= static_cast<int>(required_level);
}

bool Subscriptions::subscribe_with_auth(int fd, const std::string& topic) {
    std::lock_guard<std::mutex> lk(mu_);

    // Initialize topic permissions if not done yet
    if (topic_auth_requirements_.empty()) {
        const_cast<Subscriptions*>(this)->init_topic_permissions();
    }

    // Check authentication
    AuthLevel required_level = get_topic_auth_requirement(topic);

    auto meta_it = conn_metadata_.find(fd);
    if (meta_it == conn_metadata_.end()) {
        // No metadata = not authenticated
        if (required_level != AuthLevel::NONE) {
            return false;  // Auth required but not provided
        }
    } else {
        // Check auth level
        if (static_cast<int>(meta_it->second.auth_level) < static_cast<int>(required_level)) {
            return false;  // Insufficient auth level
        }

        // Update metadata subscriptions
        meta_it->second.subscriptions.insert(topic);
    }

    // Add to subscribers (same as subscribe())
    subscribers_[topic].insert(fd);

    return true;
}

std::vector<std::pair<int, ConnectionMetadata>> Subscriptions::get_all_connections() const {
    std::lock_guard<std::mutex> lk(mu_);

    std::vector<std::pair<int, ConnectionMetadata>> result;
    result.reserve(conn_metadata_.size());

    for (const auto& [fd, metadata] : conn_metadata_) {
        result.push_back({fd, metadata});
    }

    return result;
}

std::vector<Subscriptions::TopicStats> Subscriptions::get_topic_stats() const {
    std::lock_guard<std::mutex> lk(mu_);

    std::vector<TopicStats> result;

    // Build stats for all topics that have subscribers or replay buffers
    std::unordered_set<std::string> all_topics;

    // Collect from subscribers
    for (const auto& [topic, _] : subscribers_) {
        all_topics.insert(topic);
    }

    // Collect from replay buffers
    for (const auto& [topic, _] : replay_buffers_) {
        all_topics.insert(topic);
    }

    // Build stats for each topic
    for (const std::string& topic : all_topics) {
        TopicStats stats;
        stats.topic = topic;

        // Get subscriber count
        auto sub_it = subscribers_.find(topic);
        if (sub_it != subscribers_.end()) {
            stats.subscriber_count = sub_it->second.size();
        }

        // Get stats counters
        auto stats_it = topic_stats_.find(topic);
        if (stats_it != topic_stats_.end()) {
            stats.events_sent = stats_it->second.events_sent.load();
            stats.events_dropped = stats_it->second.events_dropped.load();
        }

        // Get current sequence
        auto replay_it = replay_buffers_.find(topic);
        if (replay_it != replay_buffers_.end()) {
            stats.current_seq = replay_it->second.seq.load();
        }

        result.push_back(stats);
    }

    return result;
}
