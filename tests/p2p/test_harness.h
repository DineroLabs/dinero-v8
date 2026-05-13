/**
 * Phase G.1: P2P Protocol Verification - Test Harness
 *
 * Multi-node test infrastructure for P2P protocol verification.
 *
 * Components:
 * 1. TestNode - Wrapper around P2PManager with message inspection
 * 2. TestNetwork - Multi-node coordinator
 * 3. MessageInspector - Message capture and analysis
 *
 * Design Philosophy:
 * - Real P2P manager (no mocking)
 * - Real sockets (no simulation)
 * - Deterministic verification
 * - Proper cleanup (no port conflicts)
 */

#pragma once

#include "../../src/daemon/p2p_manager.h"
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <unordered_map>

namespace dinero {
namespace test {

//=============================================================================
// MessageInspector: Captures and analyzes P2P messages
//=============================================================================

struct CapturedMessage {
    std::string command;
    std::string peer_address;
    std::chrono::steady_clock::time_point timestamp;
    size_t payload_size;
    std::vector<uint8_t> payload;

    enum Direction {
        SENT,
        RECEIVED
    };
    Direction direction;
};

class MessageInspector {
public:
    MessageInspector() = default;

    // Capture messages
    void on_message_sent(const std::string& peer_address, const P2PMessage& message);
    void on_message_received(const std::string& peer_address, const P2PMessage& message);

    // Query captured messages
    std::vector<CapturedMessage> get_sent_messages() const;
    std::vector<CapturedMessage> get_received_messages() const;
    std::vector<CapturedMessage> get_all_messages() const;

    // Query by command
    std::vector<CapturedMessage> get_messages_by_command(const std::string& command) const;
    size_t count_messages_by_command(const std::string& command) const;

    // Query by peer
    std::vector<CapturedMessage> get_messages_from_peer(const std::string& peer_address) const;
    std::vector<CapturedMessage> get_messages_to_peer(const std::string& peer_address) const;

    // Verification helpers
    bool has_received_command(const std::string& command) const;
    bool has_sent_command(const std::string& command) const;

    // Clear captured messages
    void clear();

    // Statistics
    size_t get_total_sent() const;
    size_t get_total_received() const;

private:
    mutable std::mutex mutex_;
    std::vector<CapturedMessage> captured_messages_;
};

//=============================================================================
// TestNode: Wrapper around P2PManager for testing
//=============================================================================

class TestNode {
public:
    TestNode(const std::string& node_name, uint16_t listen_port);
    ~TestNode();

    // Lifecycle control
    bool start();
    void stop();
    void stop_and_drain();  // Synchronous shutdown with callback drain
    bool is_running() const;

    // Connection management
    bool connect_to(const std::string& address, uint16_t port);
    bool connect_to(TestNode& other_node);
    void disconnect_from(const std::string& peer_address);

    // Message sending
    bool send_message(const std::string& peer_address, const P2PMessage& message);
    void broadcast_message(const P2PMessage& message);

    // State queries
    std::vector<PeerInfo> get_connected_peers() const;
    size_t get_peer_count() const;
    bool is_connected_to(const std::string& peer_address) const;
    void set_local_height(uint32_t height);

    // Message inspection
    const MessageInspector& get_inspector() const { return inspector_; }
    MessageInspector& get_inspector() { return inspector_; }

    // Wait helpers (for async operations)
    bool wait_for_peer_connection(const std::string& peer_address, std::chrono::milliseconds timeout);
    bool wait_for_peer_disconnection(const std::string& peer_address, std::chrono::milliseconds timeout);
    bool wait_for_message(const std::string& command, std::chrono::milliseconds timeout);
    bool wait_for_peer_count(size_t expected_count, std::chrono::milliseconds timeout);

    // Node info
    std::string get_name() const { return node_name_; }
    uint16_t get_port() const { return listen_port_; }
    std::string get_address() const { return "127.0.0.1"; }
    std::string get_address_port() const { return "127.0.0.1:" + std::to_string(listen_port_); }

private:
    std::string node_name_;
    uint16_t listen_port_;
    std::unique_ptr<P2PManager> p2p_manager_;
    MessageInspector inspector_;

    // Synchronization for deterministic waits (no polling)
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    // Cached state for safe predicate evaluation (avoids calling into P2PManager while holding lock)
    std::atomic<size_t> cached_peer_count_{0};
    std::atomic<uint32_t> local_height_{0};

    // Callback lifetime tracking for clean teardown
    std::atomic<int> active_callbacks_{0};
    std::atomic<bool> stopping_{false};

    // Message handler callbacks
    void on_message_received(const std::string& peer_address, const P2PMessage& message);
    void on_peer_connected(const std::string& peer_address);
    void on_peer_disconnected(const std::string& peer_address);
};

//=============================================================================
// TestNetwork: Multi-node coordinator
//=============================================================================

class TestNetwork {
public:
    TestNetwork() = default;
    ~TestNetwork();

    // Node management
    TestNode* add_node(const std::string& name, uint16_t listen_port);
    TestNode* get_node(const std::string& name);
    size_t get_node_count() const;

    // Lifecycle control
    bool start_all();
    void stop_all();

    // Topology creation
    bool connect_nodes(const std::string& node1_name, const std::string& node2_name);
    bool connect_all();  // Full mesh topology
    bool connect_chain();  // Linear chain: node[0] -> node[1] -> node[2] -> ...
    bool connect_star(const std::string& hub_name);  // Star: all nodes connect to hub

    // Wait for convergence
    bool wait_for_all_connected(std::chrono::milliseconds timeout);
    bool wait_for_message_propagation(const std::string& command, std::chrono::milliseconds timeout);

    // Network-wide queries
    size_t get_total_connections() const;
    size_t get_total_messages_sent() const;
    size_t get_total_messages_received() const;

    // Statistics
    void print_network_stats() const;
    void print_message_stats() const;

private:
    std::unordered_map<std::string, std::unique_ptr<TestNode>> nodes_;
    uint16_t next_port_ = 21000;  // Auto-assign ports starting from 21000
};

//=============================================================================
// Test Utilities
//=============================================================================

// Wait helper with condition variable (NO POLLING)
// This is a free function for TestNetwork use
template<typename Predicate>
bool wait_until(Predicate pred, std::chrono::milliseconds timeout) {
    // Note: This simple polling version is kept for TestNetwork which doesn't have state_cv_
    // TestNode uses its own condition variable-based waits
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= timeout) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// Port conflict avoidance
uint16_t get_random_test_port();

// Cleanup helpers
void cleanup_test_ports();

} // namespace test
} // namespace dinero
