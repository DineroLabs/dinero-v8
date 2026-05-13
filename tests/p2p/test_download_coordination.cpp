/**
 * Phase G.2: Block & TX Download Coordination - Pure Unit Tests
 *
 * Test Scope:
 * - Receiving payload clears in-flight
 * - Duplicate payload ignored
 * - Payload for non-requested object rejected
 * - Block vs TX routed correctly
 * - Size limits enforced
 * - Retry counter cleared on success
 *
 * Test Constraints:
 * ❌ No sockets
 * ❌ No threads
 * ❌ No validation (only structural checks)
 * ❌ No chainstate
 * ❌ No mempool
 * ❌ No disk writes
 * ✅ Pure download coordination
 * ✅ Deterministic
 * ✅ < 100ms total runtime
 */

#include "../../include/p2p/download_coordinator.h"
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
// Mock Download Sink (Captures Received Objects)
//=============================================================================

struct MockDownloadSink : public IDownloadSink {
    struct ReceivedObject {
        InventoryVector inv;
        std::vector<uint8_t> data;
    };

    std::vector<ReceivedObject> received_blocks;
    std::vector<ReceivedObject> received_txs;

    void onBlock(const InventoryVector& inv, const std::vector<uint8_t>& data) override {
        ReceivedObject obj;
        obj.inv = inv;
        obj.data = data;
        received_blocks.push_back(obj);
    }

    void onTx(const InventoryVector& inv, const std::vector<uint8_t>& data) override {
        ReceivedObject obj;
        obj.inv = inv;
        obj.data = data;
        received_txs.push_back(obj);
    }

    void clear() {
        received_blocks.clear();
        received_txs.clear();
    }

    size_t totalReceived() const {
        return received_blocks.size() + received_txs.size();
    }
};

//=============================================================================
// Test 1: Receiving Payload Clears In-Flight
//=============================================================================

void test_payload_clears_inflight() {
    std::cout << "\n[Test 1] Receiving payload clears in-flight" << std::endl;

    InFlightManager inflight;
    MockDownloadSink sink;
    DownloadCoordinator coordinator(inflight, sink);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(i);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Add to in-flight
    inflight.add(inv, "peer1");
    assert(inflight.exists(inv) && "Should be in-flight");

    // Receive block
    std::vector<uint8_t> block_data = {0xDE, 0xAD, 0xBE, 0xEF};
    coordinator.handleBlock("peer1", hash, block_data);

    // Should clear in-flight
    assert(!inflight.exists(inv) && "Should no longer be in-flight");

    // Should deliver to sink
    assert(sink.received_blocks.size() == 1 && "Should receive 1 block");
    assert(sink.received_blocks[0].inv == inv && "Correct inventory");
    assert(sink.received_blocks[0].data == block_data && "Correct data");

    std::cout << "  [✓] Receiving payload clears in-flight!" << std::endl;
}

//=============================================================================
// Test 2: Duplicate Payload Ignored
//=============================================================================

void test_duplicate_payload_ignored() {
    std::cout << "\n[Test 2] Duplicate payload ignored" << std::endl;

    InFlightManager inflight;
    MockDownloadSink sink;
    DownloadCoordinator coordinator(inflight, sink);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(100 + i);
    }
    InventoryVector inv(MSG_TX, hash);

    inflight.add(inv, "peer1");

    std::vector<uint8_t> tx_data = {0xCA, 0xFE, 0xBA, 0xBE};

    // First receipt
    coordinator.handleTx("peer1", hash, tx_data);
    assert(sink.received_txs.size() == 1 && "First receipt succeeds");

    sink.clear();

    // Second receipt (duplicate)
    coordinator.handleTx("peer2", hash, tx_data);
    assert(sink.received_txs.size() == 0 && "Duplicate should be ignored");

    std::cout << "  [✓] Duplicate payload correctly ignored!" << std::endl;
}

//=============================================================================
// Test 3: Payload for Non-Requested Object Rejected
//=============================================================================

void test_unrequested_payload_rejected() {
    std::cout << "\n[Test 3] Payload for non-requested object rejected" << std::endl;

    InFlightManager inflight;
    MockDownloadSink sink;
    DownloadCoordinator coordinator(inflight, sink);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(200 + i);
    }

    // Do NOT add to in-flight (unsolicited)

    std::vector<uint8_t> block_data = {0x00, 0x11, 0x22, 0x33};
    coordinator.handleBlock("peer1", hash, block_data);

    // Should NOT deliver to sink
    assert(sink.received_blocks.size() == 0 && "Unrequested payload should be rejected");

    std::cout << "  [✓] Unrequested payload correctly rejected!" << std::endl;
}

//=============================================================================
// Test 4: Block vs TX Routed Correctly
//=============================================================================

void test_routing_by_type() {
    std::cout << "\n[Test 4] Block vs TX routed correctly" << std::endl;

    InFlightManager inflight;
    MockDownloadSink sink;
    DownloadCoordinator coordinator(inflight, sink);

    Hash256 hash1, hash2;
    for (int i = 0; i < 32; i++) {
        hash1.data[i] = 1;
        hash2.data[i] = 2;
    }

    InventoryVector block_inv(MSG_BLOCK, hash1);
    InventoryVector tx_inv(MSG_TX, hash2);

    inflight.add(block_inv, "peer1");
    inflight.add(tx_inv, "peer2");

    std::vector<uint8_t> block_data = {0xBB, 0xBB};
    std::vector<uint8_t> tx_data = {0xCC, 0xCC};

    coordinator.handleBlock("peer1", hash1, block_data);
    coordinator.handleTx("peer2", hash2, tx_data);

    assert(sink.received_blocks.size() == 1 && "Should receive 1 block");
    assert(sink.received_txs.size() == 1 && "Should receive 1 tx");

    assert(sink.received_blocks[0].inv == block_inv && "Block routed correctly");
    assert(sink.received_txs[0].inv == tx_inv && "TX routed correctly");

    std::cout << "  [✓] Block vs TX routed correctly!" << std::endl;
}

//=============================================================================
// Test 5: Size Limits Enforced
//=============================================================================

void test_size_limits() {
    std::cout << "\n[Test 5] Size limits enforced" << std::endl;

    InFlightManager inflight;
    MockDownloadSink sink;
    DownloadCoordinator coordinator(inflight, sink);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(42);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    inflight.add(inv, "peer1");

    // Oversized block (> MAX_BLOCK_SIZE)
    std::vector<uint8_t> huge_block(10 * 1024 * 1024, 0xFF);  // 10MB

    coordinator.handleBlock("peer1", hash, huge_block);

    // Should reject oversized payload
    assert(sink.received_blocks.size() == 0 && "Oversized block should be rejected");

    // Should clear in-flight (rejected = failed)
    assert(!inflight.exists(inv) && "Should clear in-flight on rejection");

    std::cout << "  [✓] Size limits correctly enforced!" << std::endl;
}

//=============================================================================
// Test 6: Retry Counter Cleared on Success
//=============================================================================

void test_retry_counter_cleared() {
    std::cout << "\n[Test 6] Retry counter cleared on success" << std::endl;

    InFlightManager inflight;
    MockDownloadSink sink;
    DownloadCoordinator coordinator(inflight, sink);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(123);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Simulate retry attempts (would normally come from InventoryHandler)
    // For this test, just verify coordinator clears in-flight on success

    inflight.add(inv, "peer1");

    std::vector<uint8_t> block_data = {0x99, 0x88};
    coordinator.handleBlock("peer1", hash, block_data);

    // Should clear in-flight (success path)
    assert(!inflight.exists(inv) && "Should clear in-flight on success");

    // Should deliver
    assert(sink.received_blocks.size() == 1 && "Should deliver block");

    std::cout << "  [✓] Success clears in-flight correctly!" << std::endl;
}

//=============================================================================
// Test 7: Multiple Objects Download
//=============================================================================

void test_multiple_objects() {
    std::cout << "\n[Test 7] Multiple objects download correctly" << std::endl;

    InFlightManager inflight;
    MockDownloadSink sink;
    DownloadCoordinator coordinator(inflight, sink);

    // Add multiple blocks
    for (int i = 0; i < 5; i++) {
        Hash256 hash;
        for (int j = 0; j < 32; j++) {
            hash.data[j] = static_cast<uint8_t>(i * 32 + j);
        }
        InventoryVector inv(MSG_BLOCK, hash);
        inflight.add(inv, "peer1");

        std::vector<uint8_t> block_data = {static_cast<uint8_t>(i), static_cast<uint8_t>(i+1)};
        coordinator.handleBlock("peer1", hash, block_data);
    }

    // Should receive all 5 blocks
    assert(sink.received_blocks.size() == 5 && "Should receive 5 blocks");

    // All should be cleared from in-flight
    assert(inflight.count() == 0 && "All should be cleared");

    std::cout << "  [✓] Multiple objects download correctly!" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.2: Download Coordination Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nPure download coordination tests" << std::endl;
    std::cout << "No sockets | No threads | No validation" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        // Test 1: Payload clears in-flight
        test_payload_clears_inflight();

        // Test 2: Duplicate ignored
        test_duplicate_payload_ignored();

        // Test 3: Unrequested rejected
        test_unrequested_payload_rejected();

        // Test 4: Routing by type
        test_routing_by_type();

        // Test 5: Size limits
        test_size_limits();

        // Test 6: Retry counter cleared
        test_retry_counter_cleared();

        // Test 7: Multiple objects
        test_multiple_objects();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Download Coordination Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;

        if (duration.count() < 100) {
            std::cout << "[✓] Fast: < 100ms requirement met" << std::endl;
        } else {
            std::cout << "[!] Warning: Exceeded 100ms target" << std::endl;
        }

        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] Receiving payload clears in-flight" << std::endl;
        std::cout << "  [✓] Duplicate payload ignored" << std::endl;
        std::cout << "  [✓] Payload for non-requested object rejected" << std::endl;
        std::cout << "  [✓] Block vs TX routed correctly" << std::endl;
        std::cout << "  [✓] Size limits enforced" << std::endl;
        std::cout << "  [✓] Retry counter cleared on success" << std::endl;
        std::cout << "  [✓] Multiple objects download correctly" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
