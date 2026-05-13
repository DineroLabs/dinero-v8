/**
 * Phase G.1.4 Step 3: INV/GETDATA/NOTFOUND State Machine - Pure Unit Tests
 *
 * Test Scope:
 * - INV → GETDATA emitted once
 * - Duplicate INV ignored
 * - INV while in-flight ignored
 * - GETDATA unavailable → NOTFOUND
 * - NOTFOUND clears in-flight
 *
 * Test Constraints:
 * ❌ No sockets
 * ❌ No threads
 * ❌ No timers
 * ❌ No validation
 * ❌ No chainstate
 * ❌ No mempool
 * ✅ Pure choreography ("Given message X, emit message Y")
 * ✅ Deterministic
 * ✅ < 100ms total runtime
 */

#include "../../include/p2p/inventory_handler.h"
#include "../../include/p2p/inflight_manager.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <set>
#include <map>
#include <cstring>

using namespace dinero::p2p;

//=============================================================================
// Mock Message Sink (Captures Emitted Messages)
//=============================================================================

struct MockMessageSink : public IMessageSender {
    struct SentMessage {
        std::string peer;
        std::string type;  // "getdata", "notfound", "block", "tx"
        std::vector<InventoryVector> inventory;
    };

    std::vector<SentMessage> sent;

    void sendGetData(const std::string& peer, const GetDataMessage& msg) override {
        SentMessage m;
        m.peer = peer;
        m.type = "getdata";
        m.inventory = msg.inventory;
        sent.push_back(m);
    }

    void sendNotFound(const std::string& peer, const NotFoundMessage& msg) override {
        SentMessage m;
        m.peer = peer;
        m.type = "notfound";
        m.inventory = msg.inventory;
        sent.push_back(m);
    }

    void sendBlock(const std::string& peer, const std::vector<uint8_t>& data) override {
        SentMessage m;
        m.peer = peer;
        m.type = "block";
        sent.push_back(m);
    }

    void sendTx(const std::string& peer, const std::vector<uint8_t>& data) override {
        SentMessage m;
        m.peer = peer;
        m.type = "tx";
        sent.push_back(m);
    }

    void clear() {
        sent.clear();
    }

    size_t countGetData() const {
        size_t count = 0;
        for (const auto& msg : sent) {
            if (msg.type == "getdata") count++;
        }
        return count;
    }

    size_t countNotFound() const {
        size_t count = 0;
        for (const auto& msg : sent) {
            if (msg.type == "notfound") count++;
        }
        return count;
    }
};

//=============================================================================
// Mock Callbacks (Pure Functions)
//=============================================================================

struct MockCallbacks : public ICallbackProvider {
    std::set<InventoryVector> wanted_objects;
    std::map<InventoryVector, std::vector<uint8_t>> available_objects;

    bool wantObject(const InventoryVector& inv) override {
        return wanted_objects.count(inv) > 0;
    }

    std::optional<std::vector<uint8_t>> provideObject(const InventoryVector& inv) override {
        auto it = available_objects.find(inv);
        if (it != available_objects.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<std::string> selectPeerForRetry(const InventoryVector& inv, const std::string& failed_peer) override {
        return std::nullopt;  // Not used in Step 3 tests
    }
};

//=============================================================================
// Test 1: INV → GETDATA Emitted Once
//=============================================================================

void test_inv_to_getdata() {
    std::cout << "\n[Test 1] INV → GETDATA emitted once" << std::endl;

    InFlightManager inflight;
    MockMessageSink sink;
    MockCallbacks callbacks;
    InventoryHandler handler(inflight, sink, callbacks);

    // Create test inventory
    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(i);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Mark as wanted
    callbacks.wanted_objects.insert(inv);

    // Receive INV from peer1
    InvMessage inv_msg;
    inv_msg.add(MSG_BLOCK, hash);

    handler.handleInv("peer1", inv_msg);

    // Should emit GETDATA
    assert(sink.countGetData() == 1 && "Should emit 1 GETDATA");
    assert(sink.sent[0].peer == "peer1" && "GETDATA to peer1");
    assert(sink.sent[0].inventory.size() == 1 && "GETDATA has 1 item");
    assert(sink.sent[0].inventory[0] == inv && "GETDATA requests correct inv");

    // Should be in-flight
    assert(inflight.exists(inv) && "Should be in-flight");

    std::cout << "  [✓] INV → GETDATA works!" << std::endl;
}

//=============================================================================
// Test 2: Duplicate INV Ignored
//=============================================================================

void test_duplicate_inv_ignored() {
    std::cout << "\n[Test 2] Duplicate INV ignored" << std::endl;

    InFlightManager inflight;
    MockMessageSink sink;
    MockCallbacks callbacks;
    InventoryHandler handler(inflight, sink, callbacks);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(100 + i);
    }
    InventoryVector inv(MSG_TX, hash);

    callbacks.wanted_objects.insert(inv);

    // First INV from peer1
    InvMessage inv_msg;
    inv_msg.add(MSG_TX, hash);
    handler.handleInv("peer1", inv_msg);

    assert(sink.countGetData() == 1 && "First INV emits GETDATA");

    sink.clear();

    // Second INV from peer2 (duplicate)
    handler.handleInv("peer2", inv_msg);

    assert(sink.countGetData() == 0 && "Duplicate INV should not emit GETDATA");

    std::cout << "  [✓] Duplicate INV correctly ignored!" << std::endl;
}

//=============================================================================
// Test 3: INV While In-Flight Ignored
//=============================================================================

void test_inv_while_inflight_ignored() {
    std::cout << "\n[Test 3] INV while in-flight ignored" << std::endl;

    InFlightManager inflight;
    MockMessageSink sink;
    MockCallbacks callbacks;
    InventoryHandler handler(inflight, sink, callbacks);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(200 + i);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    callbacks.wanted_objects.insert(inv);

    // Add to in-flight manually (simulating earlier request)
    inflight.add(inv, "peer1");

    // Receive INV from peer2
    InvMessage inv_msg;
    inv_msg.add(MSG_BLOCK, hash);
    handler.handleInv("peer2", inv_msg);

    // Should NOT emit GETDATA (already in-flight)
    assert(sink.countGetData() == 0 && "Should not emit GETDATA when in-flight");

    std::cout << "  [✓] INV while in-flight correctly ignored!" << std::endl;
}

//=============================================================================
// Test 4: GETDATA Unavailable → NOTFOUND
//=============================================================================

void test_getdata_notfound() {
    std::cout << "\n[Test 4] GETDATA unavailable → NOTFOUND" << std::endl;

    InFlightManager inflight;
    MockMessageSink sink;
    MockCallbacks callbacks;
    InventoryHandler handler(inflight, sink, callbacks);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(42);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Don't add to available_objects (object unavailable)

    // Receive GETDATA from peer1
    GetDataMessage getdata;
    getdata.add(MSG_BLOCK, hash);

    handler.handleGetData("peer1", getdata);

    // Should emit NOTFOUND
    assert(sink.countNotFound() == 1 && "Should emit NOTFOUND");
    assert(sink.sent[0].peer == "peer1" && "NOTFOUND to peer1");
    assert(sink.sent[0].inventory.size() == 1 && "NOTFOUND has 1 item");
    assert(sink.sent[0].inventory[0] == inv && "NOTFOUND for correct inv");

    std::cout << "  [✓] GETDATA unavailable → NOTFOUND works!" << std::endl;
}

//=============================================================================
// Test 5: NOTFOUND Clears In-Flight
//=============================================================================

void test_notfound_clears_inflight() {
    std::cout << "\n[Test 5] NOTFOUND clears in-flight" << std::endl;

    InFlightManager inflight;
    MockMessageSink sink;
    MockCallbacks callbacks;
    InventoryHandler handler(inflight, sink, callbacks);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(123);
    }
    InventoryVector inv(MSG_TX, hash);

    // Add to in-flight
    inflight.add(inv, "peer1");
    assert(inflight.exists(inv) && "Should be in-flight initially");

    // Receive NOTFOUND from peer1
    NotFoundMessage notfound;
    notfound.add(MSG_TX, hash);

    handler.handleNotFound("peer1", notfound);

    // Should clear in-flight
    assert(!inflight.exists(inv) && "Should no longer be in-flight");

    std::cout << "  [✓] NOTFOUND clears in-flight correctly!" << std::endl;
}

//=============================================================================
// Test 6: GETDATA Available → Send Object
//=============================================================================

void test_getdata_available_sends_object() {
    std::cout << "\n[Test 6] GETDATA available → send object" << std::endl;

    InFlightManager inflight;
    MockMessageSink sink;
    MockCallbacks callbacks;
    InventoryHandler handler(inflight, sink, callbacks);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(99);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Make object available
    std::vector<uint8_t> block_data = {0xDE, 0xAD, 0xBE, 0xEF};
    callbacks.available_objects[inv] = block_data;

    // Receive GETDATA
    GetDataMessage getdata;
    getdata.add(MSG_BLOCK, hash);

    handler.handleGetData("peer1", getdata);

    // Should send block
    assert(sink.sent.size() == 1 && "Should send 1 message");
    assert(sink.sent[0].type == "block" && "Should send block");
    assert(sink.sent[0].peer == "peer1" && "Block to peer1");

    std::cout << "  [✓] GETDATA available → send object works!" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.1.4 Step 3: INV/GETDATA Handler Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nPure protocol choreography tests" << std::endl;
    std::cout << "No sockets | No threads | No validation" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        // Test 1: INV → GETDATA
        test_inv_to_getdata();

        // Test 2: Duplicate INV ignored
        test_duplicate_inv_ignored();

        // Test 3: INV while in-flight ignored
        test_inv_while_inflight_ignored();

        // Test 4: GETDATA unavailable → NOTFOUND
        test_getdata_notfound();

        // Test 5: NOTFOUND clears in-flight
        test_notfound_clears_inflight();

        // Test 6: GETDATA available → send object
        test_getdata_available_sends_object();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Handler Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;

        if (duration.count() < 100) {
            std::cout << "[✓] Fast: < 100ms requirement met" << std::endl;
        } else {
            std::cout << "[!] Warning: Exceeded 100ms target" << std::endl;
        }

        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] INV → GETDATA emitted once" << std::endl;
        std::cout << "  [✓] Duplicate INV ignored" << std::endl;
        std::cout << "  [✓] INV while in-flight ignored" << std::endl;
        std::cout << "  [✓] GETDATA unavailable → NOTFOUND" << std::endl;
        std::cout << "  [✓] NOTFOUND clears in-flight" << std::endl;
        std::cout << "  [✓] GETDATA available → send object" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
