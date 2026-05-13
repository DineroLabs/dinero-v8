/**
 * Phase G.3.1: Validation Sink Wiring - Pure Unit Tests
 *
 * Test Scope:
 * - Payload reaches validation queue
 * - Block vs TX queued separately
 * - Queue ordering preserved
 * - Queue can be drained
 *
 * Test Constraints:
 * ❌ NO validation logic
 * ❌ NO disk writes
 * ❌ NO mempool
 * ❌ NO chainstate
 * ❌ No sockets
 * ❌ No threads
 * ✅ Pure wiring test (data in → data queued)
 * ✅ Deterministic
 * ✅ < 100ms total runtime
 */

#include "../../include/p2p/validation_sink.h"
#include "../../include/p2p/download_coordinator.h"
#include "../../include/p2p/inflight_manager.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero::p2p;

//=============================================================================
// Test 1: Block Payload Reaches Queue
//=============================================================================

void test_block_reaches_queue() {
    std::cout << "\n[Test 1] Block payload reaches validation queue" << std::endl;

    ValidationQueue queue;
    ValidationSink sink(queue);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(i);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    std::vector<uint8_t> block_data = {0xDE, 0xAD, 0xBE, 0xEF};

    // Send to sink
    sink.onBlock(inv, block_data);

    // Should be in queue
    assert(queue.blockCount() == 1 && "Queue should have 1 block");
    assert(queue.txCount() == 0 && "Queue should have 0 txs");

    // Drain and verify
    auto blocks = queue.drainBlocks();
    assert(blocks.size() == 1 && "Should drain 1 block");
    assert(blocks[0].inv == inv && "Inventory should match");
    assert(blocks[0].data == block_data && "Data should match");

    std::cout << "  [✓] Block reaches queue!" << std::endl;
}

//=============================================================================
// Test 2: TX Payload Reaches Queue
//=============================================================================

void test_tx_reaches_queue() {
    std::cout << "\n[Test 2] TX payload reaches validation queue" << std::endl;

    ValidationQueue queue;
    ValidationSink sink(queue);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(100 + i);
    }
    InventoryVector inv(MSG_TX, hash);

    std::vector<uint8_t> tx_data = {0xCA, 0xFE, 0xBA, 0xBE};

    // Send to sink
    sink.onTx(inv, tx_data);

    // Should be in queue
    assert(queue.txCount() == 1 && "Queue should have 1 tx");
    assert(queue.blockCount() == 0 && "Queue should have 0 blocks");

    // Drain and verify
    auto txs = queue.drainTxs();
    assert(txs.size() == 1 && "Should drain 1 tx");
    assert(txs[0].inv == inv && "Inventory should match");
    assert(txs[0].data == tx_data && "Data should match");

    std::cout << "  [✓] TX reaches queue!" << std::endl;
}

//=============================================================================
// Test 3: Block vs TX Queued Separately
//=============================================================================

void test_separate_queues() {
    std::cout << "\n[Test 3] Block vs TX queued separately" << std::endl;

    ValidationQueue queue;
    ValidationSink sink(queue);

    Hash256 hash1, hash2;
    for (int i = 0; i < 32; i++) {
        hash1.data[i] = 1;
        hash2.data[i] = 2;
    }

    InventoryVector block_inv(MSG_BLOCK, hash1);
    InventoryVector tx_inv(MSG_TX, hash2);

    std::vector<uint8_t> block_data = {0xBB};
    std::vector<uint8_t> tx_data = {0xCC};

    // Send both
    sink.onBlock(block_inv, block_data);
    sink.onTx(tx_inv, tx_data);

    // Both queued separately
    assert(queue.blockCount() == 1 && "Should have 1 block");
    assert(queue.txCount() == 1 && "Should have 1 tx");

    // Drain blocks
    auto blocks = queue.drainBlocks();
    assert(blocks.size() == 1 && "Should drain 1 block");
    assert(queue.blockCount() == 0 && "Block queue should be empty");
    assert(queue.txCount() == 1 && "TX queue should still have 1");

    // Drain txs
    auto txs = queue.drainTxs();
    assert(txs.size() == 1 && "Should drain 1 tx");
    assert(queue.txCount() == 0 && "TX queue should be empty");

    std::cout << "  [✓] Separate queues work correctly!" << std::endl;
}

//=============================================================================
// Test 4: Queue Ordering Preserved
//=============================================================================

void test_queue_ordering() {
    std::cout << "\n[Test 4] Queue ordering preserved (FIFO)" << std::endl;

    ValidationQueue queue;
    ValidationSink sink(queue);

    // Add 5 blocks in order
    for (int i = 0; i < 5; i++) {
        Hash256 hash;
        for (int j = 0; j < 32; j++) {
            hash.data[j] = static_cast<uint8_t>(i * 32 + j);
        }
        InventoryVector inv(MSG_BLOCK, hash);
        std::vector<uint8_t> data = {static_cast<uint8_t>(i)};

        sink.onBlock(inv, data);
    }

    assert(queue.blockCount() == 5 && "Should have 5 blocks");

    // Drain and verify order
    auto blocks = queue.drainBlocks();
    assert(blocks.size() == 5 && "Should drain 5 blocks");

    for (int i = 0; i < 5; i++) {
        assert(blocks[i].data[0] == static_cast<uint8_t>(i) && "Order should be preserved");
    }

    std::cout << "  [✓] Queue ordering preserved!" << std::endl;
}

//=============================================================================
// Test 5: Multiple Drains
//=============================================================================

void test_multiple_drains() {
    std::cout << "\n[Test 5] Multiple drains work correctly" << std::endl;

    ValidationQueue queue;
    ValidationSink sink(queue);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = 42;
    }
    InventoryVector inv(MSG_BLOCK, hash);
    std::vector<uint8_t> data = {0x99};

    sink.onBlock(inv, data);

    // First drain
    auto blocks1 = queue.drainBlocks();
    assert(blocks1.size() == 1 && "First drain should get 1 block");

    // Second drain (empty)
    auto blocks2 = queue.drainBlocks();
    assert(blocks2.size() == 0 && "Second drain should be empty");

    // Add another
    sink.onBlock(inv, data);

    // Third drain
    auto blocks3 = queue.drainBlocks();
    assert(blocks3.size() == 1 && "Third drain should get 1 block");

    std::cout << "  [✓] Multiple drains work correctly!" << std::endl;
}

//=============================================================================
// Test 6: End-to-End with DownloadCoordinator
//=============================================================================

void test_e2e_download_to_validation() {
    std::cout << "\n[Test 6] End-to-end: Download → Validation queue" << std::endl;

    InFlightManager inflight;
    ValidationQueue queue;
    ValidationSink sink(queue);
    DownloadCoordinator coordinator(inflight, sink);

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(123);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Request block
    inflight.add(inv, "peer1");

    // Download completes
    std::vector<uint8_t> block_data = {0xAA, 0xBB, 0xCC};
    coordinator.handleBlock("peer1", hash, block_data);

    // Should reach validation queue
    assert(queue.blockCount() == 1 && "Block should reach validation queue");

    // Drain and verify
    auto blocks = queue.drainBlocks();
    assert(blocks.size() == 1 && "Should drain 1 block");
    assert(blocks[0].data == block_data && "Data should match");

    // Should be cleared from in-flight
    assert(!inflight.exists(inv) && "Should be cleared from in-flight");

    std::cout << "  [✓] End-to-end wiring works!" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.3.1: Validation Sink Wiring Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nPure wiring tests (data in → data queued)" << std::endl;
    std::cout << "NO validation | NO disk | NO mempool" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        // Test 1: Block reaches queue
        test_block_reaches_queue();

        // Test 2: TX reaches queue
        test_tx_reaches_queue();

        // Test 3: Separate queues
        test_separate_queues();

        // Test 4: Queue ordering
        test_queue_ordering();

        // Test 5: Multiple drains
        test_multiple_drains();

        // Test 6: E2E
        test_e2e_download_to_validation();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Validation Sink Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;

        if (duration.count() < 100) {
            std::cout << "[✓] Fast: < 100ms requirement met" << std::endl;
        } else {
            std::cout << "[!] Warning: Exceeded 100ms target" << std::endl;
        }

        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] Block payload reaches queue" << std::endl;
        std::cout << "  [✓] TX payload reaches queue" << std::endl;
        std::cout << "  [✓] Block vs TX queued separately" << std::endl;
        std::cout << "  [✓] Queue ordering preserved (FIFO)" << std::endl;
        std::cout << "  [✓] Multiple drains work" << std::endl;
        std::cout << "  [✓] End-to-end: Download → Validation" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
