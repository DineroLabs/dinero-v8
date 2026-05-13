/**
 * CSN Proof Refresh Tests (#6)
 *
 * Verifies that:
 * 1. RequestProofRefresh sends getdata for stale txids
 * 2. Duplicate requests are suppressed (pending set)
 * 3. Batch size is respected
 * 4. Rate limiting (REFRESH_COOLDOWN) works
 * 5. CompleteRefresh allows re-requesting
 * 6. RecordBridgeResponse targets bridge peers
 * 7. No-op when not in CSN mode
 */

#include "daemon/tx_relay_manager.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "common/ilogger.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>

using namespace dinero;

// ============================================================================
// Test Infrastructure
// ============================================================================

static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while(0)

// ============================================================================
// Mock Logger
// ============================================================================

class MockLogger : public ILogger {
public:
    void setLogLevel(LogLevel level) override {}
    void setLogFile(const std::string& filename) override {}
    void shutdown() override {}

    void log(LogLevel level, const std::string& message) override {
        std::cout << "[LOG] " << message << std::endl;
    }
    void debug(const std::string& message) override {}
    void info(const std::string& message) override {
        std::cout << "[INFO] " << message << std::endl;
    }
    void warning(const std::string& message) override {
        std::cout << "[WARN] " << message << std::endl;
    }
    void error(const std::string& message) override {
        std::cout << "[ERROR] " << message << std::endl;
    }
};

// ============================================================================
// Mock P2P Message Sink
// ============================================================================

class MockP2PMessageSink {
public:
    struct Message {
        std::string peer_address;
        std::string command;
        std::vector<uint8_t> payload;
    };

    std::vector<Message> messages;

    void SendMessage(const std::string& peer_addr, const std::string& cmd,
                     const std::vector<uint8_t>& payload) {
        messages.push_back({peer_addr, cmd, payload});
    }

    size_t count_command(const std::string& cmd) const {
        size_t count = 0;
        for (const auto& msg : messages) {
            if (msg.command == cmd) ++count;
        }
        return count;
    }

    void clear() { messages.clear(); }
};

// ============================================================================
// Helpers
// ============================================================================

static uint256 makeTxId(uint32_t seed) {
    uint256 id;
    std::memset(id.data, 0, 32);
    std::memcpy(id.data, &seed, 4);
    return id;
}

// ============================================================================
// Tests
// ============================================================================

// Test 1: RequestProofRefresh sends getdata for stale txids
static void test_sends_getdata_for_stale_txids() {
    std::cout << "Test 1: sends getdata for stale txids..." << std::endl;

    MockLogger logger;
    MockP2PMessageSink sink;
    TxRelayManager relay(&logger);
    relay.SetCsnMode(true);
    relay.SetSendMessageCallback(
        [&sink](const std::string& peer, const std::string& cmd, const std::vector<uint8_t>& payload) {
            sink.SendMessage(peer, cmd, payload);
        });

    std::vector<uint256> stale = {makeTxId(1), makeTxId(2), makeTxId(3)};
    relay.RequestProofRefresh(stale);

    TEST_ASSERT(sink.count_command("getdata") == 3, "Should send 3 getdata messages");

    // All should be broadcast (no bridge peer known)
    for (const auto& msg : sink.messages) {
        TEST_ASSERT(msg.peer_address.empty(), "Should broadcast (no bridge peer known)");
    }

    std::cout << "  PASSED" << std::endl;
}

// Test 2: Skips already-pending txids
static void test_skips_pending_txids() {
    std::cout << "Test 2: skips already-pending txids..." << std::endl;

    MockLogger logger;
    MockP2PMessageSink sink;
    TxRelayManager relay(&logger);
    relay.SetCsnMode(true);
    relay.SetSendMessageCallback(
        [&sink](const std::string& peer, const std::string& cmd, const std::vector<uint8_t>& payload) {
            sink.SendMessage(peer, cmd, payload);
        });

    std::vector<uint256> stale = {makeTxId(10)};
    relay.RequestProofRefresh(stale);
    TEST_ASSERT(sink.count_command("getdata") == 1, "First call: 1 getdata");

    // Wait past cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    sink.clear();
    relay.RequestProofRefresh(stale);
    TEST_ASSERT(sink.count_command("getdata") == 0, "Second call: 0 getdata (already pending)");

    std::cout << "  PASSED" << std::endl;
}

// Test 3: Respects batch_size
static void test_respects_batch_size() {
    std::cout << "Test 3: respects batch_size..." << std::endl;

    MockLogger logger;
    MockP2PMessageSink sink;
    TxRelayManager relay(&logger);
    relay.SetCsnMode(true);
    relay.SetSendMessageCallback(
        [&sink](const std::string& peer, const std::string& cmd, const std::vector<uint8_t>& payload) {
            sink.SendMessage(peer, cmd, payload);
        });

    std::vector<uint256> stale;
    for (uint32_t i = 0; i < 20; i++) {
        stale.push_back(makeTxId(100 + i));
    }

    relay.RequestProofRefresh(stale, 5);
    TEST_ASSERT(sink.count_command("getdata") == 5, "Should only send 5 (batch_size)");

    std::cout << "  PASSED" << std::endl;
}

// Test 4: Rate limits (REFRESH_COOLDOWN)
static void test_rate_limits() {
    std::cout << "Test 4: rate limits..." << std::endl;

    MockLogger logger;
    MockP2PMessageSink sink;
    TxRelayManager relay(&logger);
    relay.SetCsnMode(true);
    relay.SetSendMessageCallback(
        [&sink](const std::string& peer, const std::string& cmd, const std::vector<uint8_t>& payload) {
            sink.SendMessage(peer, cmd, payload);
        });

    relay.RequestProofRefresh({makeTxId(200)});
    TEST_ASSERT(sink.count_command("getdata") == 1, "First batch: 1 getdata");

    sink.clear();
    // Immediately try again with a different txid
    relay.RequestProofRefresh({makeTxId(201)});
    TEST_ASSERT(sink.count_command("getdata") == 0, "Second batch: blocked by cooldown");

    // Wait past cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    sink.clear();
    relay.RequestProofRefresh({makeTxId(201)});
    TEST_ASSERT(sink.count_command("getdata") == 1, "Third batch: allowed after cooldown");

    std::cout << "  PASSED" << std::endl;
}

// Test 5: CompleteRefresh allows re-requesting
static void test_complete_refresh_allows_rerequest() {
    std::cout << "Test 5: CompleteRefresh allows re-requesting..." << std::endl;

    MockLogger logger;
    MockP2PMessageSink sink;
    TxRelayManager relay(&logger);
    relay.SetCsnMode(true);
    relay.SetSendMessageCallback(
        [&sink](const std::string& peer, const std::string& cmd, const std::vector<uint8_t>& payload) {
            sink.SendMessage(peer, cmd, payload);
        });

    uint256 txid = makeTxId(300);
    relay.RequestProofRefresh({txid});
    TEST_ASSERT(sink.count_command("getdata") == 1, "First request: 1 getdata");

    // Complete the refresh
    relay.CompleteRefresh(txid);

    // Wait past cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    sink.clear();
    relay.RequestProofRefresh({txid});
    TEST_ASSERT(sink.count_command("getdata") == 1, "After CompleteRefresh: can re-request");

    std::cout << "  PASSED" << std::endl;
}

// Test 6: RecordBridgeResponse targets bridge peers
static void test_bridge_peer_targeting() {
    std::cout << "Test 6: RecordBridgeResponse targets bridge peers..." << std::endl;

    MockLogger logger;
    MockP2PMessageSink sink;
    TxRelayManager relay(&logger);
    relay.SetCsnMode(true);
    relay.SetSendMessageCallback(
        [&sink](const std::string& peer, const std::string& cmd, const std::vector<uint8_t>& payload) {
            sink.SendMessage(peer, cmd, payload);
        });

    // Record a bridge peer
    relay.RecordBridgeResponse("1.2.3.4:20999");

    relay.RequestProofRefresh({makeTxId(400)});
    TEST_ASSERT(sink.count_command("getdata") == 1, "Should send 1 getdata");
    TEST_ASSERT(sink.messages[0].peer_address == "1.2.3.4:20999",
                "Should target known bridge peer");

    std::cout << "  PASSED" << std::endl;
}

// Test 7: No-op when not in CSN mode
static void test_noop_without_csn_mode() {
    std::cout << "Test 7: no-op when not in CSN mode..." << std::endl;

    MockLogger logger;
    MockP2PMessageSink sink;
    TxRelayManager relay(&logger);
    // CSN mode NOT set
    relay.SetSendMessageCallback(
        [&sink](const std::string& peer, const std::string& cmd, const std::vector<uint8_t>& payload) {
            sink.SendMessage(peer, cmd, payload);
        });

    relay.RequestProofRefresh({makeTxId(500), makeTxId(501)});
    TEST_ASSERT(sink.count_command("getdata") == 0, "Should send nothing without CSN mode");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  CSN Proof Refresh Tests (#6)" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    test_sends_getdata_for_stale_txids();
    test_skips_pending_txids();
    test_respects_batch_size();
    test_rate_limits();
    test_complete_refresh_allows_rerequest();
    test_bridge_peer_targeting();
    test_noop_without_csn_mode();

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total
              << " assertions passed!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    return tests_passed == tests_total ? 0 : 1;
}
