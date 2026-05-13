#pragma once

#include "peer_simulator.h"
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace dinero::p2p::test {

// ============================================================================
// Ring 3 Phase 2: Property Test Framework (QuickCheck/Hypothesis Style)
// ============================================================================
//
// This framework enables property-based testing of P2P protocol:
//   - ConnectionSequenceGenerator: Generate random event sequences
//   - PropertyAssertion: DSL for expressing properties
//   - InvariantChecker: Verify state machine invariants
//   - Statistical validation: Run thousands of iterations
//
// Goal: Prove properties hold for ALL inputs (∀x, property(x) = true)

// ============================================================================
// Connection Event Types
// ============================================================================

/// Events that can occur in a connection lifecycle
enum class ConnectionEventType {
    CONNECT,          // Initiate connection
    SEND_VERSION,     // Send VERSION message
    RECV_VERSION,     // Receive VERSION from peer
    SEND_VERACK,      // Send VERACK message
    RECV_VERACK,      // Receive VERACK from peer
    SEND_PING,        // Send PING message
    RECV_PONG,        // Receive PONG response
    SEND_MESSAGE,     // Send generic message
    RECV_MESSAGE,     // Receive generic message
    DISCONNECT,       // Close connection
    TIMEOUT,          // Timeout event (e.g., ping timeout)
};

/// String representation for debugging
inline std::string to_string(ConnectionEventType type) {
    switch (type) {
        case ConnectionEventType::CONNECT: return "CONNECT";
        case ConnectionEventType::SEND_VERSION: return "SEND_VERSION";
        case ConnectionEventType::RECV_VERSION: return "RECV_VERSION";
        case ConnectionEventType::SEND_VERACK: return "SEND_VERACK";
        case ConnectionEventType::RECV_VERACK: return "RECV_VERACK";
        case ConnectionEventType::SEND_PING: return "SEND_PING";
        case ConnectionEventType::RECV_PONG: return "RECV_PONG";
        case ConnectionEventType::SEND_MESSAGE: return "SEND_MESSAGE";
        case ConnectionEventType::RECV_MESSAGE: return "RECV_MESSAGE";
        case ConnectionEventType::DISCONNECT: return "DISCONNECT";
        case ConnectionEventType::TIMEOUT: return "TIMEOUT";
        default: return "UNKNOWN";
    }
}

/// Single connection event
struct ConnectionEvent {
    ConnectionEventType type;
    uint64_t timestamp_ms;          // When event occurs
    std::optional<uint64_t> nonce;  // For PING/PONG
    std::optional<std::vector<uint8_t>> data;  // For generic messages

    // Default constructor (for Violation struct)
    ConnectionEvent()
        : type(ConnectionEventType::CONNECT), timestamp_ms(0) {}

    ConnectionEvent(ConnectionEventType t, uint64_t ts)
        : type(t), timestamp_ms(ts) {}
};

// ============================================================================
// Connection Sequence Generator
// ============================================================================

/// Generates random sequences of connection events for property testing
class ConnectionSequenceGenerator {
public:
    struct Config {
        size_t min_events;       // Minimum events in sequence
        size_t max_events;       // Maximum events in sequence
        double timeout_probability;    // Probability of timeout event
        double disconnect_probability; // Probability of disconnect event
        bool allow_protocol_violations; // Generate invalid sequences

        Config()
            : min_events(5)
            , max_events(50)
            , timeout_probability(0.1)
            , disconnect_probability(0.1)
            , allow_protocol_violations(false) {}
    };

    ConnectionSequenceGenerator(PropertyTestRNG& rng, const Config& config = Config())
        : rng_(rng), config_(config) {}

    /// Generate valid handshake sequence (for honest peers)
    std::vector<ConnectionEvent> generate_valid_handshake() {
        std::vector<ConnectionEvent> events;
        uint64_t time = 0;

        // Handshake sequence: CONNECT → VERSION → VERACK (both directions)
        events.push_back({ConnectionEventType::CONNECT, time});
        time += rng_.uint64(10, 100);

        events.push_back({ConnectionEventType::SEND_VERSION, time});
        time += rng_.uint64(10, 100);

        events.push_back({ConnectionEventType::RECV_VERSION, time});
        time += rng_.uint64(10, 100);

        events.push_back({ConnectionEventType::SEND_VERACK, time});
        time += rng_.uint64(10, 100);

        events.push_back({ConnectionEventType::RECV_VERACK, time});

        return events;
    }

    /// Generate invalid handshake sequence (for protocol violation testing)
    std::vector<ConnectionEvent> generate_invalid_handshake() {
        std::vector<ConnectionEvent> events;
        uint64_t time = 0;

        events.push_back({ConnectionEventType::CONNECT, time});
        time += rng_.uint64(10, 100);

        // Invalid: Send regular message before VERSION
        events.push_back({ConnectionEventType::SEND_MESSAGE, time});
        time += rng_.uint64(10, 100);

        events.push_back({ConnectionEventType::SEND_VERSION, time});

        return events;
    }

    /// Generate random connection sequence
    std::vector<ConnectionEvent> generate_random_sequence() {
        size_t num_events = rng_.uint32(config_.min_events, config_.max_events);
        std::vector<ConnectionEvent> events;
        uint64_t time = 0;

        // Always start with CONNECT
        events.push_back({ConnectionEventType::CONNECT, time});
        time += rng_.uint64(10, 100);

        for (size_t i = 1; i < num_events; i++) {
            ConnectionEventType type = generate_random_event_type();
            events.push_back({type, time});
            time += rng_.uint64(10, 100);

            // Random disconnect or timeout
            if (rng_.boolean(config_.disconnect_probability)) {
                events.push_back({ConnectionEventType::DISCONNECT, time});
                break;
            }
            if (rng_.boolean(config_.timeout_probability)) {
                events.push_back({ConnectionEventType::TIMEOUT, time});
            }
        }

        return events;
    }

    /// Generate ping/pong sequence (for liveness testing)
    std::vector<ConnectionEvent> generate_ping_pong_sequence(size_t num_pings) {
        std::vector<ConnectionEvent> events;
        uint64_t time = 0;

        // Start with valid handshake
        auto handshake = generate_valid_handshake();
        events.insert(events.end(), handshake.begin(), handshake.end());
        time = events.back().timestamp_ms + 100;

        // Generate ping/pong pairs
        for (size_t i = 0; i < num_pings; i++) {
            uint64_t nonce = rng_.uint64(0, UINT64_MAX);

            ConnectionEvent ping{ConnectionEventType::SEND_PING, time};
            ping.nonce = nonce;
            events.push_back(ping);
            time += rng_.uint64(50, 200);

            ConnectionEvent pong{ConnectionEventType::RECV_PONG, time};
            pong.nonce = nonce;
            events.push_back(pong);
            time += rng_.uint64(100, 500);
        }

        return events;
    }

    /// Generate stale peer sequence (for timeout testing)
    std::vector<ConnectionEvent> generate_stale_peer_sequence() {
        std::vector<ConnectionEvent> events;
        uint64_t time = 0;

        // Handshake completes normally
        auto handshake = generate_valid_handshake();
        events.insert(events.end(), handshake.begin(), handshake.end());
        time = events.back().timestamp_ms + 100;

        // Send PING but never receive PONG
        ConnectionEvent ping{ConnectionEventType::SEND_PING, time};
        ping.nonce = rng_.uint64(0, UINT64_MAX);
        events.push_back(ping);
        time += 10000;  // 10 seconds - exceeds timeout

        // Timeout event
        events.push_back({ConnectionEventType::TIMEOUT, time});

        return events;
    }

private:
    ConnectionEventType generate_random_event_type() {
        uint32_t choice = rng_.uint32(0, 8);
        switch (choice) {
            case 0: return ConnectionEventType::SEND_VERSION;
            case 1: return ConnectionEventType::RECV_VERSION;
            case 2: return ConnectionEventType::SEND_VERACK;
            case 3: return ConnectionEventType::RECV_VERACK;
            case 4: return ConnectionEventType::SEND_PING;
            case 5: return ConnectionEventType::RECV_PONG;
            case 6: return ConnectionEventType::SEND_MESSAGE;
            case 7: return ConnectionEventType::RECV_MESSAGE;
            case 8: return ConnectionEventType::DISCONNECT;
            default: return ConnectionEventType::SEND_MESSAGE;
        }
    }

    PropertyTestRNG& rng_;
    Config config_;
};

// ============================================================================
// Connection State (for Invariant Checking)
// ============================================================================

/// Connection states (from Ring 3 specification)
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    HANDSHAKING,
    ESTABLISHED,
    DISCONNECTING
};

inline std::string to_string(ConnectionState state) {
    switch (state) {
        case ConnectionState::DISCONNECTED: return "DISCONNECTED";
        case ConnectionState::CONNECTING: return "CONNECTING";
        case ConnectionState::HANDSHAKING: return "HANDSHAKING";
        case ConnectionState::ESTABLISHED: return "ESTABLISHED";
        case ConnectionState::DISCONNECTING: return "DISCONNECTING";
        default: return "UNKNOWN";
    }
}

/// Connection state machine for invariant checking
class ConnectionStateMachine {
public:
    ConnectionStateMachine() : state_(ConnectionState::DISCONNECTED) {}

    ConnectionState state() const { return state_; }

    /// Process event and update state
    bool process_event(const ConnectionEvent& event) {
        ConnectionState old_state = state_;

        switch (event.type) {
            case ConnectionEventType::CONNECT:
                if (state_ == ConnectionState::DISCONNECTED) {
                    state_ = ConnectionState::CONNECTING;
                    return true;
                }
                return false;  // Invalid: already connected

            case ConnectionEventType::SEND_VERSION:
                if (state_ == ConnectionState::CONNECTING) {
                    state_ = ConnectionState::HANDSHAKING;
                    sent_version_ = true;
                    return true;
                }
                if (state_ == ConnectionState::HANDSHAKING) {
                    sent_version_ = true;
                    return true;
                }
                return false;

            case ConnectionEventType::RECV_VERSION:
                if (state_ == ConnectionState::CONNECTING) {
                    state_ = ConnectionState::HANDSHAKING;
                    received_version_ = true;
                    return true;
                }
                if (state_ == ConnectionState::HANDSHAKING) {
                    received_version_ = true;
                    return true;
                }
                return false;

            case ConnectionEventType::SEND_VERACK:
                if (state_ == ConnectionState::HANDSHAKING) {
                    sent_verack_ = true;
                    // Transition to ESTABLISHED only after both VERACKs
                    if (sent_verack_ && received_verack_) {
                        state_ = ConnectionState::ESTABLISHED;
                    }
                    return true;
                }
                return false;

            case ConnectionEventType::RECV_VERACK:
                if (state_ == ConnectionState::HANDSHAKING) {
                    received_verack_ = true;
                    // Transition to ESTABLISHED only after both VERACKs
                    if (sent_verack_ && received_verack_) {
                        state_ = ConnectionState::ESTABLISHED;
                    }
                    return true;
                }
                return false;

            case ConnectionEventType::SEND_PING:
            case ConnectionEventType::RECV_PONG:
            case ConnectionEventType::SEND_MESSAGE:
            case ConnectionEventType::RECV_MESSAGE:
                // Messages only allowed in ESTABLISHED state
                return state_ == ConnectionState::ESTABLISHED;

            case ConnectionEventType::DISCONNECT:
                if (state_ != ConnectionState::DISCONNECTED) {
                    state_ = ConnectionState::DISCONNECTING;
                    return true;
                }
                return false;

            case ConnectionEventType::TIMEOUT:
                // Timeout always valid (can happen in any state)
                return true;

            default:
                return false;
        }
    }

    /// Check if state transition is valid
    bool is_valid_transition(ConnectionState from, ConnectionState to) const {
        // Valid transitions from Ring 3 specification
        if (from == ConnectionState::DISCONNECTED && to == ConnectionState::CONNECTING) return true;
        if (from == ConnectionState::CONNECTING && to == ConnectionState::HANDSHAKING) return true;
        if (from == ConnectionState::HANDSHAKING && to == ConnectionState::ESTABLISHED) return true;
        if (to == ConnectionState::DISCONNECTING) return true;  // Can disconnect from any state
        if (from == to) return true;  // Staying in same state is valid
        return false;
    }

    void mark_version_sent() { sent_version_ = true; }
    void mark_version_received() { received_version_ = true; }

private:
    ConnectionState state_;
    bool sent_version_ = false;
    bool received_version_ = false;
    bool sent_verack_ = false;
    bool received_verack_ = false;
};

// ============================================================================
// Invariant Checker
// ============================================================================

/// Verifies connection state machine invariants
class InvariantChecker {
public:
    struct Violation {
        std::string description;
        ConnectionEvent event;
        ConnectionState state_before;
        ConnectionState state_after;
    };

    InvariantChecker() : state_machine_() {}

    /// Check if event maintains invariants
    std::optional<Violation> check_event(const ConnectionEvent& event) {
        ConnectionState state_before = state_machine_.state();

        // Process event
        bool valid = state_machine_.process_event(event);

        if (!valid) {
            Violation v;
            v.description = "Invalid event in current state";
            v.event = event;
            v.state_before = state_before;
            v.state_after = state_machine_.state();
            return v;
        }

        ConnectionState state_after = state_machine_.state();

        // Check state transition validity
        if (!state_machine_.is_valid_transition(state_before, state_after)) {
            Violation v;
            v.description = "Invalid state transition";
            v.event = event;
            v.state_before = state_before;
            v.state_after = state_after;
            return v;
        }

        return std::nullopt;  // No violation
    }

    /// Check sequence of events
    std::vector<Violation> check_sequence(const std::vector<ConnectionEvent>& events) {
        std::vector<Violation> violations;

        for (const auto& event : events) {
            auto violation = check_event(event);
            if (violation.has_value()) {
                violations.push_back(violation.value());
            }
        }

        return violations;
    }

    /// Verify monotonicity: state cannot go backward
    bool verify_monotonicity(const std::vector<ConnectionEvent>& events) {
        ConnectionState prev_state = ConnectionState::DISCONNECTED;

        for (const auto& event : events) {
            state_machine_.process_event(event);
            ConnectionState curr_state = state_machine_.state();

            // Check monotonicity (simplified - actual check more complex)
            if (curr_state == ConnectionState::CONNECTING &&
                prev_state == ConnectionState::ESTABLISHED) {
                return false;  // Cannot go back from ESTABLISHED to CONNECTING
            }

            prev_state = curr_state;
        }

        return true;
    }

    ConnectionState current_state() const { return state_machine_.state(); }

private:
    ConnectionStateMachine state_machine_;
};

// ============================================================================
// Property Assertion DSL
// ============================================================================

/// Property test result
struct PropertyTestResult {
    bool passed;
    size_t num_iterations;
    size_t num_failures;
    std::vector<std::string> failure_messages;

    PropertyTestResult() : passed(true), num_iterations(0), num_failures(0) {}

    void record_failure(const std::string& message) {
        passed = false;
        num_failures++;
        failure_messages.push_back(message);
    }

    std::string summary() const {
        std::ostringstream oss;
        oss << "Iterations: " << num_iterations << ", ";
        oss << "Failures: " << num_failures << " ";
        oss << (passed ? "[PASS]" : "[FAIL]");
        return oss.str();
    }
};

/// Property test builder (fluent interface)
class PropertyTest {
public:
    PropertyTest(const std::string& name) : name_(name), iterations_(100) {}

    /// Set number of iterations (default 100)
    PropertyTest& iterations(size_t n) {
        iterations_ = n;
        return *this;
    }

    /// Define property to test
    template<typename Generator, typename Property>
    PropertyTestResult forAll(Generator gen, Property prop) {
        PropertyTestResult result;
        result.num_iterations = iterations_;

        for (size_t i = 0; i < iterations_; i++) {
            auto input = gen();

            try {
                if (!prop(input)) {
                    std::ostringstream oss;
                    oss << "Property violated at iteration " << i;
                    result.record_failure(oss.str());
                }
            } catch (const std::exception& e) {
                std::ostringstream oss;
                oss << "Exception at iteration " << i << ": " << e.what();
                result.record_failure(oss.str());
            }
        }

        return result;
    }

private:
    std::string name_;
    size_t iterations_;
};

// ============================================================================
// Test Utilities
// ============================================================================

/// Execute event sequence and collect violations
inline PropertyTestResult execute_sequence_test(
    const std::string& test_name,
    std::function<std::vector<ConnectionEvent>()> generator,
    size_t iterations = 100)
{
    PropertyTest test(test_name);
    test.iterations(iterations);

    return test.forAll(
        generator,
        [](const std::vector<ConnectionEvent>& events) {
            InvariantChecker checker;
            auto violations = checker.check_sequence(events);
            return violations.empty();
        }
    );
}

} // namespace dinero::p2p::test
