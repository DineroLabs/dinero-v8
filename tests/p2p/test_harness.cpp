/**
 * Phase G.1: P2P Protocol Verification - Test Harness Implementation
 */

#include "test_harness.h"
#include <gtest/gtest.h>
#include <iostream>
#include <algorithm>
#include <random>
#include <thread>

namespace dinero {
namespace test {

//=============================================================================
// MessageInspector Implementation
//=============================================================================

void MessageInspector::on_message_sent(const std::string& peer_address, const P2PMessage& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    CapturedMessage captured;
    captured.command = message.command;
    captured.peer_address = peer_address;
    captured.timestamp = std::chrono::steady_clock::now();
    captured.payload_size = message.payload.size();
    captured.payload = message.payload;
    captured.direction = CapturedMessage::SENT;
    captured_messages_.push_back(captured);
}

void MessageInspector::on_message_received(const std::string& peer_address, const P2PMessage& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    CapturedMessage captured;
    captured.command = message.command;
    captured.peer_address = peer_address;
    captured.timestamp = std::chrono::steady_clock::now();
    captured.payload_size = message.payload.size();
    captured.payload = message.payload;
    captured.direction = CapturedMessage::RECEIVED;
    captured_messages_.push_back(captured);
}

std::vector<CapturedMessage> MessageInspector::get_sent_messages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CapturedMessage> result;
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::SENT) {
            result.push_back(msg);
        }
    }
    return result;
}

std::vector<CapturedMessage> MessageInspector::get_received_messages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CapturedMessage> result;
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::RECEIVED) {
            result.push_back(msg);
        }
    }
    return result;
}

std::vector<CapturedMessage> MessageInspector::get_all_messages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return captured_messages_;
}

std::vector<CapturedMessage> MessageInspector::get_messages_by_command(const std::string& command) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CapturedMessage> result;
    for (const auto& msg : captured_messages_) {
        if (msg.command == command) {
            result.push_back(msg);
        }
    }
    return result;
}

size_t MessageInspector::count_messages_by_command(const std::string& command) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& msg : captured_messages_) {
        if (msg.command == command) {
            count++;
        }
    }
    return count;
}

std::vector<CapturedMessage> MessageInspector::get_messages_from_peer(const std::string& peer_address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CapturedMessage> result;
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::RECEIVED && msg.peer_address == peer_address) {
            result.push_back(msg);
        }
    }
    return result;
}

std::vector<CapturedMessage> MessageInspector::get_messages_to_peer(const std::string& peer_address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CapturedMessage> result;
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::SENT && msg.peer_address == peer_address) {
            result.push_back(msg);
        }
    }
    return result;
}

bool MessageInspector::has_received_command(const std::string& command) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::RECEIVED && msg.command == command) {
            return true;
        }
    }
    return false;
}

bool MessageInspector::has_sent_command(const std::string& command) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::SENT && msg.command == command) {
            return true;
        }
    }
    return false;
}

void MessageInspector::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    captured_messages_.clear();
}

size_t MessageInspector::get_total_sent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::SENT) {
            count++;
        }
    }
    return count;
}

size_t MessageInspector::get_total_received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& msg : captured_messages_) {
        if (msg.direction == CapturedMessage::RECEIVED) {
            count++;
        }
    }
    return count;
}

//=============================================================================
// TestNode Implementation
//=============================================================================

TestNode::TestNode(const std::string& node_name, uint16_t listen_port)
    : node_name_(node_name)
    , listen_port_(listen_port)
    , p2p_manager_(std::make_unique<P2PManager>(listen_port, ""))  // Empty external IP disables self-connection check
{
    p2p_manager_->set_height_provider([this]() -> uint32_t {
        return local_height_.load(std::memory_order_relaxed);
    });

    // Set message handler to capture received messages
    p2p_manager_->set_message_handler([this](const std::string& peer_address, const P2PMessage& message) {
        this->on_message_received(peer_address, message);
    });

    // Set peer connected/disconnected handlers
    p2p_manager_->set_peer_connected_handler([this](const std::string& peer_address) {
        this->on_peer_connected(peer_address);
    });

    p2p_manager_->set_peer_disconnected_handler([this](const std::string& peer_address) {
        this->on_peer_disconnected(peer_address);
    });
}

TestNode::~TestNode() {
    if (is_running()) {
        stop();
    }
}

bool TestNode::start() {
    std::cerr << "DEBUG: TestNode::start() called for " << node_name_ << std::endl;
    std::cerr.flush();

    bool success = p2p_manager_->start();
    std::cerr << "DEBUG: p2p_manager_->start() returned " << success << std::endl;
    std::cerr.flush();

    if (!success) {
        std::cerr << "[" << node_name_ << "] ERROR: P2PManager::start() failed" << std::endl;
        return false;
    }

    std::cerr << "DEBUG: About to call WaitUntilListening()..." << std::endl;
    std::cerr.flush();

    // Wait for socket to be listening (deterministic, no races)
    bool ready = p2p_manager_->WaitUntilListening(std::chrono::seconds(10));

    std::cerr << "DEBUG: WaitUntilListening() returned " << ready << std::endl;
    std::cerr.flush();

    if (!ready) {
        std::cerr << "[" << node_name_ << "] ERROR: Socket never became ready on port " << listen_port_ << " (timeout after 10s)" << std::endl;
        return false;
    }

    return true;
}

void TestNode::stop() {
    std::cout << "[" << node_name_ << "] Stopping..." << std::endl;

    // Stop P2P manager (joins threads internally)
    p2p_manager_->stop();

    // Verify stopped (no guessing with sleep)
    if (!is_running()) {
        std::cout << "[" << node_name_ << "] Stopped cleanly" << std::endl;
    } else {
        std::cout << "[" << node_name_ << "] Warning: stop() returned but still running" << std::endl;
    }
}

void TestNode::stop_and_drain() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        stopping_.store(true, std::memory_order_release);
    }

    p2p_manager_->stop();

    // Wait until all callbacks have completed (no callbacks can fire after this)
    std::unique_lock<std::mutex> lk(state_mutex_);
    bool drained = state_cv_.wait_for(
        lk,
        std::chrono::seconds(5),
        [this] { return active_callbacks_.load(std::memory_order_acquire) == 0; }
    );

    if (!drained) {
        std::cerr << "[" << node_name_ << "] WARNING: Callbacks did not drain within timeout (active="
                  << active_callbacks_.load() << ")" << std::endl;
    }
}

bool TestNode::is_running() const {
    return p2p_manager_->is_running();
}

bool TestNode::connect_to(const std::string& address, uint16_t port) {
    std::cout << "[" << node_name_ << "] Connecting to " << address << ":" << port << std::endl;
    return p2p_manager_->connect_to_peer(address, port);
}

bool TestNode::connect_to(TestNode& other_node) {
    return connect_to(other_node.get_address(), other_node.get_port());
}

void TestNode::disconnect_from(const std::string& peer_address) {
    std::cout << "[" << node_name_ << "] Disconnecting from " << peer_address << std::endl;
    p2p_manager_->disconnect_peer(peer_address);
}

bool TestNode::send_message(const std::string& peer_address, const P2PMessage& message) {
    inspector_.on_message_sent(peer_address, message);
    return p2p_manager_->send_to_peer(peer_address, message);
}

void TestNode::broadcast_message(const P2PMessage& message) {
    // Capture message for each connected peer
    auto peers = get_connected_peers();
    for (const auto& peer : peers) {
        inspector_.on_message_sent(peer.to_string(), message);
    }
    p2p_manager_->broadcast_message(message);
}

std::vector<PeerInfo> TestNode::get_connected_peers() const {
    return p2p_manager_->get_connected_peers();
}

size_t TestNode::get_peer_count() const {
    return p2p_manager_->get_peer_count();
}

bool TestNode::is_connected_to(const std::string& peer_address) const {
    auto peers = get_connected_peers();
    for (const auto& peer : peers) {
        if (peer.to_string() == peer_address || peer.address == peer_address) {
            return true;
        }
    }
    return false;
}

void TestNode::set_local_height(uint32_t height) {
    local_height_.store(height, std::memory_order_relaxed);
}

bool TestNode::wait_for_peer_connection(const std::string& peer_address, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    return state_cv_.wait_for(lock, timeout, [this, &peer_address]() {
        return this->is_connected_to(peer_address);
    });
}

bool TestNode::wait_for_peer_disconnection(const std::string& peer_address, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    return state_cv_.wait_for(lock, timeout, [this, &peer_address]() {
        return !this->is_connected_to(peer_address);
    });
}

bool TestNode::wait_for_message(const std::string& command, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    return state_cv_.wait_for(lock, timeout, [this, &command]() {
        return this->inspector_.has_received_command(command);
    });
}

bool TestNode::wait_for_peer_count(size_t expected_count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    return state_cv_.wait_for(lock, timeout, [this, expected_count]() {
        // Use cached count to avoid calling into P2PManager while holding lock
        return this->cached_peer_count_.load() == expected_count;
    });
}

void TestNode::on_message_received(const std::string& peer_address, const P2PMessage& message) {
    // Track active callback (RAII pattern for exception safety)
    active_callbacks_.fetch_add(1, std::memory_order_acquire);
    struct CallbackGuard {
        std::atomic<int>& counter;
        std::condition_variable& cv;
        ~CallbackGuard() {
            counter.fetch_sub(1, std::memory_order_release);
            cv.notify_all();
        }
    } guard{active_callbacks_, state_cv_};

    inspector_.on_message_received(peer_address, message);
    std::cout << "[" << node_name_ << "] Received " << message.command
              << " from " << peer_address << std::endl;

    // Notify waiters (deterministic, no polling)
    state_cv_.notify_all();
}

void TestNode::on_peer_connected(const std::string& peer_address) {
    // Track active callback (RAII pattern for exception safety)
    active_callbacks_.fetch_add(1, std::memory_order_acquire);
    struct CallbackGuard {
        std::atomic<int>& counter;
        std::condition_variable& cv;
        ~CallbackGuard() {
            counter.fetch_sub(1, std::memory_order_release);
            cv.notify_all();
        }
    } guard{active_callbacks_, state_cv_};

    std::cout << "[" << node_name_ << "] Peer connected: " << peer_address << std::endl;

    // Update cached peer count (safe for predicate evaluation)
    cached_peer_count_ = get_peer_count();

    // Notify waiters (deterministic, no polling)
    state_cv_.notify_all();
}

void TestNode::on_peer_disconnected(const std::string& peer_address) {
    // Track active callback (RAII pattern for exception safety)
    active_callbacks_.fetch_add(1, std::memory_order_acquire);
    struct CallbackGuard {
        std::atomic<int>& counter;
        std::condition_variable& cv;
        ~CallbackGuard() {
            counter.fetch_sub(1, std::memory_order_release);
            cv.notify_all();
        }
    } guard{active_callbacks_, state_cv_};

    std::cout << "[" << node_name_ << "] Peer disconnected: " << peer_address << std::endl;

    // Update cached peer count (safe for predicate evaluation)
    cached_peer_count_ = get_peer_count();

    // Notify waiters (deterministic, no polling)
    state_cv_.notify_all();
}

//=============================================================================
// TestNetwork Implementation
//=============================================================================

TestNetwork::~TestNetwork() {
    stop_all();
}

TestNode* TestNetwork::add_node(const std::string& name, uint16_t listen_port) {
    if (listen_port == 0) {
        listen_port = next_port_++;
    }

    auto node = std::make_unique<TestNode>(name, listen_port);
    auto* node_ptr = node.get();
    nodes_[name] = std::move(node);
    return node_ptr;
}

TestNode* TestNetwork::get_node(const std::string& name) {
    auto it = nodes_.find(name);
    if (it != nodes_.end()) {
        return it->second.get();
    }
    return nullptr;
}

size_t TestNetwork::get_node_count() const {
    return nodes_.size();
}

bool TestNetwork::start_all() {
    std::cerr << "DEBUG: TestNetwork::start_all() called with " << nodes_.size() << " nodes" << std::endl;
    std::cerr.flush();
    bool all_started = true;
    for (auto& [name, node] : nodes_) {
        std::cerr << "DEBUG: Starting node " << name << std::endl;
        std::cerr.flush();
        if (!node->start()) {
            std::cerr << "[TestNetwork] ERROR: Failed to start node: " << name << std::endl;
            all_started = false;
        }
    }
    std::cerr << "DEBUG: TestNetwork::start_all() returning " << all_started << std::endl;
    std::cerr.flush();
    return all_started;
}

void TestNetwork::stop_all() {
    for (auto& [name, node] : nodes_) {
        node->stop_and_drain();
    }
}

bool TestNetwork::connect_nodes(const std::string& node1_name, const std::string& node2_name) {
    auto* node1 = get_node(node1_name);
    auto* node2 = get_node(node2_name);

    if (!node1 || !node2) {
        std::cerr << "[TestNetwork] One or both nodes not found" << std::endl;
        return false;
    }

    return node1->connect_to(*node2);
}

bool TestNetwork::connect_all() {
    std::cout << "[TestNetwork] Creating full mesh topology..." << std::endl;
    std::vector<std::string> node_names;
    for (const auto& [name, _] : nodes_) {
        node_names.push_back(name);
    }

    bool all_connected = true;
    for (size_t i = 0; i < node_names.size(); i++) {
        for (size_t j = i + 1; j < node_names.size(); j++) {
            if (!connect_nodes(node_names[i], node_names[j])) {
                all_connected = false;
            }
        }
    }

    return all_connected;
}

bool TestNetwork::connect_chain() {
    std::cout << "[TestNetwork] Creating chain topology..." << std::endl;
    std::vector<std::string> node_names;
    for (const auto& [name, _] : nodes_) {
        node_names.push_back(name);
    }

    if (node_names.size() < 2) {
        return true;  // Nothing to connect
    }

    bool all_connected = true;
    for (size_t i = 0; i < node_names.size() - 1; i++) {
        if (!connect_nodes(node_names[i], node_names[i + 1])) {
            all_connected = false;
        }
    }

    return all_connected;
}

bool TestNetwork::connect_star(const std::string& hub_name) {
    std::cout << "[TestNetwork] Creating star topology (hub: " << hub_name << ")..." << std::endl;
    auto* hub = get_node(hub_name);
    if (!hub) {
        std::cerr << "[TestNetwork] Hub node not found: " << hub_name << std::endl;
        return false;
    }

    bool all_connected = true;
    for (auto& [name, node] : nodes_) {
        if (name != hub_name) {
            if (!node->connect_to(*hub)) {
                all_connected = false;
            }
        }
    }

    return all_connected;
}

bool TestNetwork::wait_for_all_connected(std::chrono::milliseconds timeout) {
    std::cout << "[TestNetwork] Waiting for all connections to establish..." << std::endl;
    return wait_until([this]() {
        for (const auto& [name, node] : this->nodes_) {
            if (!node->is_running()) {
                return false;
            }
        }
        return true;
    }, timeout);
}

bool TestNetwork::wait_for_message_propagation(const std::string& command, std::chrono::milliseconds timeout) {
    return wait_until([this, &command]() {
        for (const auto& [name, node] : this->nodes_) {
            if (!node->get_inspector().has_received_command(command)) {
                return false;
            }
        }
        return true;
    }, timeout);
}

size_t TestNetwork::get_total_connections() const {
    size_t total = 0;
    for (const auto& [name, node] : nodes_) {
        total += node->get_peer_count();
    }
    return total;
}

size_t TestNetwork::get_total_messages_sent() const {
    size_t total = 0;
    for (const auto& [name, node] : nodes_) {
        total += node->get_inspector().get_total_sent();
    }
    return total;
}

size_t TestNetwork::get_total_messages_received() const {
    size_t total = 0;
    for (const auto& [name, node] : nodes_) {
        total += node->get_inspector().get_total_received();
    }
    return total;
}

void TestNetwork::print_network_stats() const {
    std::cout << "\n=== Network Statistics ===" << std::endl;
    std::cout << "Total nodes: " << nodes_.size() << std::endl;
    std::cout << "Total connections: " << get_total_connections() << std::endl;

    for (const auto& [name, node] : nodes_) {
        std::cout << "  [" << name << "] port=" << node->get_port()
                  << " peers=" << node->get_peer_count() << std::endl;
    }
}

void TestNetwork::print_message_stats() const {
    std::cout << "\n=== Message Statistics ===" << std::endl;
    std::cout << "Total sent: " << get_total_messages_sent() << std::endl;
    std::cout << "Total received: " << get_total_messages_received() << std::endl;

    for (const auto& [name, node] : nodes_) {
        std::cout << "  [" << name << "] sent=" << node->get_inspector().get_total_sent()
                  << " received=" << node->get_inspector().get_total_received() << std::endl;
    }
}

//=============================================================================
// Utilities
//=============================================================================

uint16_t get_random_test_port() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint16_t> dist(21000, 30000);
    return dist(gen);
}

void cleanup_test_ports() {
    // Platform-specific cleanup if needed
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

} // namespace test
} // namespace dinero
