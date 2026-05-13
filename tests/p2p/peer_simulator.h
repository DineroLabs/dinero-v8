#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <functional>
#include <random>
#include <chrono>
#include <string>
#include <optional>
#include <cstdint>

namespace dinero::p2p::test {

// ============================================================================
// Ring 3 Phase 1: Deterministic Peer Simulator Infrastructure
// ============================================================================
//
// This file provides test infrastructure for P2P protocol verification:
//   - MockSocket: Simulated TCP socket (no real network I/O)
//   - PeerSimulator: Simulated peer with scripted behaviors
//   - DeterministicScheduler: Controlled event ordering
//   - PropertyTestRNG: Reproducible randomness
//
// Goal: Enable 10,000+ connection simulations in milliseconds, not hours.

// ============================================================================
// Deterministic RNG (Property Test Foundation)
// ============================================================================

/// Deterministic random number generator for property-based testing
/// Fixed seed ensures reproducible test failures
class PropertyTestRNG {
public:
    explicit PropertyTestRNG(uint64_t seed = 42) : rng_(seed) {}

    // Generate random uint32_t in range [min, max]
    // Why portable mod instead of std::uniform_int_distribution: the STL
    // distribution is not specified to be reproducible across libstdc++,
    // libc++, and MSVC's STL. Tests that assert on specific sampled values
    // (e.g. PropertyTest.ExceptionHandling) drift between platforms otherwise.
    uint32_t uint32(uint32_t min, uint32_t max) {
        if (max <= min) return min;
        return min + static_cast<uint32_t>(rng_() % (static_cast<uint64_t>(max) - min + 1));
    }

    // Generate random uint64_t in range [min, max]
    uint64_t uint64(uint64_t min, uint64_t max) {
        if (max <= min) return min;
        uint64_t range = max - min + 1;
        if (range == 0) return rng_(); // full uint64 range
        return min + (rng_() % range);
    }

    // Generate random bool with probability p
    bool boolean(double p = 0.5) {
        if (p <= 0.0) return false;
        if (p >= 1.0) return true;
        // std::bernoulli_distribution is also not cross-STL reproducible;
        // map the engine output through a fixed 2^53 mantissa scale instead.
        constexpr uint64_t scale = 1ULL << 53;
        return (rng_() & (scale - 1)) < static_cast<uint64_t>(p * scale);
    }

    // Generate random bytes
    std::vector<uint8_t> bytes(size_t n) {
        std::vector<uint8_t> result(n);
        for (size_t i = 0; i < n; i++) {
            result[i] = static_cast<uint8_t>(uint32(0, 255));
        }
        return result;
    }

    // Reseed for new test case
    void reseed(uint64_t seed) {
        rng_.seed(seed);
    }

private:
    std::mt19937_64 rng_;
};

// ============================================================================
// Mock Socket (Simulated TCP)
// ============================================================================

/// Simulated TCP socket for deterministic P2P testing
/// No real network I/O - all data queued in memory
class MockSocket {
public:
    struct Config {
        uint64_t latency_ms;           // Simulated network latency
        double packet_loss_rate;       // Probability of dropping packet
        bool is_connected;             // Socket state
        size_t max_send_buffer;        // Max send buffer size

        Config() : latency_ms(0), packet_loss_rate(0.0), is_connected(true), max_send_buffer(10 * 1024 * 1024) {}
    };

    MockSocket(PropertyTestRNG& rng, const Config& config = Config())
        : rng_(rng), config_(config) {}

    // Simulated socket operations
    bool is_connected() const { return config_.is_connected; }
    void disconnect() { config_.is_connected = false; }

    // Write message to send buffer (simulated TCP send)
    bool write(const std::vector<uint8_t>& data) {
        if (!config_.is_connected) return false;
        if (send_buffer_bytes_ + data.size() > config_.max_send_buffer) {
            return false;  // Backpressure: buffer full
        }

        // Simulate packet loss
        if (rng_.boolean(config_.packet_loss_rate)) {
            return true;  // Packet "sent" but dropped by network
        }

        // Queue message with simulated latency
        uint64_t delivery_time = current_time_ms_ + config_.latency_ms;
        send_buffer_.push_back({data, delivery_time});
        send_buffer_bytes_ += data.size();
        return true;
    }

    // Read next available message from receive buffer
    std::optional<std::vector<uint8_t>> read() {
        if (!config_.is_connected) return std::nullopt;
        if (recv_buffer_.empty()) return std::nullopt;

        auto msg = recv_buffer_.front();
        recv_buffer_.pop();
        return msg;
    }

    // Inject message into receive buffer (simulate peer sending to us)
    void inject_message(const std::vector<uint8_t>& data) {
        recv_buffer_.push(data);
    }

    // Drain send buffer (simulate network delivering our messages to peer)
    std::vector<std::vector<uint8_t>> drain_send_buffer() {
        std::vector<std::vector<uint8_t>> delivered;

        while (!send_buffer_.empty()) {
            auto& msg = send_buffer_.front();
            if (msg.delivery_time <= current_time_ms_) {
                delivered.push_back(msg.data);
                send_buffer_bytes_ -= msg.data.size();
                send_buffer_.pop_front();
            } else {
                break;  // Not ready yet (latency)
            }
        }

        return delivered;
    }

    // Advance simulated time (for latency simulation)
    void advance_time(uint64_t ms) {
        current_time_ms_ += ms;
    }

    // Inject network failures
    void inject_latency(uint64_t ms) { config_.latency_ms = ms; }
    void inject_packet_loss(double rate) { config_.packet_loss_rate = rate; }
    void inject_disconnect() { disconnect(); }

    // Inspection for tests
    size_t send_buffer_size() const { return send_buffer_.size(); }
    size_t recv_buffer_size() const { return recv_buffer_.size(); }

private:
    struct QueuedMessage {
        std::vector<uint8_t> data;
        uint64_t delivery_time;  // When message becomes available
    };

    PropertyTestRNG& rng_;
    Config config_;
    std::deque<QueuedMessage> send_buffer_;
    std::queue<std::vector<uint8_t>> recv_buffer_;
    uint64_t current_time_ms_ = 0;
    size_t send_buffer_bytes_ = 0;  // Total bytes in send buffer
};

// ============================================================================
// Peer Behavior Models
// ============================================================================

/// Defines how a simulated peer responds to messages
enum class PeerBehavior {
    HONEST,          // Follows protocol correctly (VERSION, VERACK, PONG)
    STALE,           // Never responds to PING (timeout test)
    PROTOCOL_VIOLATOR, // Sends messages out of order (handshake test)
    MALICIOUS,       // Sends junk data, invalid messages
    SLOW,            // Delays all responses (latency test)
};

// ============================================================================
// Peer Simulator
// ============================================================================

/// Simulated peer that responds to P2P messages
/// Used to test connection lifecycle, handshake, message ordering
class PeerSimulator {
public:
    PeerSimulator(PropertyTestRNG& rng, PeerBehavior behavior = PeerBehavior::HONEST)
        : rng_(rng), behavior_(behavior), socket_(rng) {}

    // Process incoming message and generate response(s)
    void on_message(const std::vector<uint8_t>& msg) {
        // Parse message type (simplified - real implementation would deserialize)
        if (msg.empty()) return;

        MessageType type = parse_message_type(msg);

        switch (behavior_) {
            case PeerBehavior::HONEST:
                handle_honest(type, msg);
                break;
            case PeerBehavior::STALE:
                handle_stale(type, msg);
                break;
            case PeerBehavior::PROTOCOL_VIOLATOR:
                handle_protocol_violator(type, msg);
                break;
            case PeerBehavior::MALICIOUS:
                handle_malicious(type, msg);
                break;
            case PeerBehavior::SLOW:
                handle_slow(type, msg);
                break;
        }
    }

    // Get socket for reading responses
    MockSocket& socket() { return socket_; }
    const MockSocket& socket() const { return socket_; }

    // Change behavior mid-test (simulate peer going stale)
    void set_behavior(PeerBehavior b) { behavior_ = b; }

private:
    enum class MessageType {
        VERSION,
        VERACK,
        PING,
        PONG,
        INV,
        GETDATA,
        BLOCK,
        TX,
        UNKNOWN
    };

    MessageType parse_message_type(const std::vector<uint8_t>& msg) {
        // Simplified parsing - real implementation would check command string
        if (msg.size() < 4) return MessageType::UNKNOWN;

        // Mock parsing (in real code, deserialize Bitcoin message header)
        if (msg[0] == 'V' && msg[1] == 'E' && msg[2] == 'R') {
            if (msg[3] == 'S') return MessageType::VERSION;
            if (msg[3] == 'A') return MessageType::VERACK;
        }
        if (msg[0] == 'P' && msg[1] == 'I') return MessageType::PING;
        if (msg[0] == 'P' && msg[1] == 'O') return MessageType::PONG;

        return MessageType::UNKNOWN;
    }

    void handle_honest(MessageType type, const std::vector<uint8_t>& msg) {
        switch (type) {
            case MessageType::VERSION:
                // Send VERSION reply
                socket_.inject_message({'V', 'E', 'R', 'S', 'I', 'O', 'N'});
                // Send VERACK
                socket_.inject_message({'V', 'E', 'R', 'A', 'C', 'K'});
                handshake_complete_ = true;
                break;

            case MessageType::VERACK:
                // Handshake complete (peer acknowledged our VERSION)
                break;

            case MessageType::PING:
                // Extract nonce and send PONG with same nonce
                // Simplified: echo the message as PONG
                {
                    auto pong = msg;
                    pong[0] = 'P';
                    pong[1] = 'O';
                    socket_.inject_message(pong);
                }
                break;

            default:
                // Ignore other messages
                break;
        }
    }

    void handle_stale(MessageType type, const std::vector<uint8_t>& msg) {
        // Respond to VERSION (complete handshake) but ignore PING
        if (type == MessageType::VERSION) {
            socket_.inject_message({'V', 'E', 'R', 'S', 'I', 'O', 'N'});
            socket_.inject_message({'V', 'E', 'R', 'A', 'C', 'K'});
        }
        // Never respond to PING (simulate stale peer)
    }

    void handle_protocol_violator(MessageType type, const std::vector<uint8_t>& msg) {
        // Send messages in wrong order
        if (type == MessageType::VERSION) {
            // Wrong: Send VERACK before VERSION reply
            socket_.inject_message({'V', 'E', 'R', 'A', 'C', 'K'});
            socket_.inject_message({'V', 'E', 'R', 'S', 'I', 'O', 'N'});
        }
    }

    void handle_malicious(MessageType type, const std::vector<uint8_t>& msg) {
        // Send junk data
        auto junk = rng_.bytes(rng_.uint32(10, 1000));
        socket_.inject_message(junk);
    }

    void handle_slow(MessageType type, const std::vector<uint8_t>& msg) {
        // Same as HONEST but with artificial delay
        socket_.inject_latency(1000);  // 1 second delay
        handle_honest(type, msg);
    }

    PropertyTestRNG& rng_;
    PeerBehavior behavior_;
    MockSocket socket_;
    bool handshake_complete_ = false;
};

// ============================================================================
// Deterministic Scheduler
// ============================================================================

/// Controlled event scheduler for deterministic P2P tests
/// Ensures reproducible thread interleaving
class DeterministicScheduler {
public:
    using Event = std::function<void()>;

    explicit DeterministicScheduler(uint64_t seed = 42) : rng_(seed) {}

    // Schedule event to run at specific time
    void schedule_at(uint64_t time_ms, Event event) {
        events_.push({time_ms, std::move(event)});
    }

    // Schedule event to run after delay
    void schedule_after(uint64_t delay_ms, Event event) {
        schedule_at(current_time_ + delay_ms, std::move(event));
    }

    // Schedule event to run immediately
    void schedule_now(Event event) {
        schedule_at(current_time_, std::move(event));
    }

    // Run all events until specified time
    void run_until(uint64_t time_ms) {
        while (!events_.empty() && events_.top().time <= time_ms) {
            auto event = events_.top();
            events_.pop();

            current_time_ = event.time;
            event.callback();
        }
        current_time_ = time_ms;
    }

    // Run all pending events
    void run_until_idle() {
        while (!events_.empty()) {
            auto event = events_.top();
            events_.pop();

            current_time_ = event.time;
            event.callback();
        }
    }

    // Get current simulated time
    uint64_t current_time() const { return current_time_; }

    // Check if any events pending
    bool has_pending_events() const { return !events_.empty(); }

private:
    struct ScheduledEvent {
        uint64_t time;
        Event callback;

        bool operator>(const ScheduledEvent& other) const {
            return time > other.time;  // Min-heap (earliest time first)
        }
    };

    PropertyTestRNG rng_;
    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, std::greater<ScheduledEvent>> events_;
    uint64_t current_time_ = 0;
};

// ============================================================================
// Test Utilities
// ============================================================================

/// Create Bitcoin-style VERSION message (simplified)
inline std::vector<uint8_t> make_version_message() {
    // Simplified: real implementation would serialize full VERSION structure
    return {'V', 'E', 'R', 'S', 'I', 'O', 'N', 0x00, 0x00, 0x00, 0x00};
}

/// Create Bitcoin-style VERACK message
inline std::vector<uint8_t> make_verack_message() {
    return {'V', 'E', 'R', 'A', 'C', 'K'};
}

/// Create Bitcoin-style PING message with nonce
inline std::vector<uint8_t> make_ping_message(uint64_t nonce) {
    std::vector<uint8_t> msg = {'P', 'I', 'N', 'G'};
    // Append nonce (8 bytes, little-endian)
    for (int i = 0; i < 8; i++) {
        msg.push_back(static_cast<uint8_t>((nonce >> (i * 8)) & 0xff));
    }
    return msg;
}

/// Create Bitcoin-style PONG message with nonce
inline std::vector<uint8_t> make_pong_message(uint64_t nonce) {
    std::vector<uint8_t> msg = {'P', 'O', 'N', 'G'};
    // Append nonce (8 bytes, little-endian)
    for (int i = 0; i < 8; i++) {
        msg.push_back(static_cast<uint8_t>((nonce >> (i * 8)) & 0xff));
    }
    return msg;
}

} // namespace dinero::p2p::test
