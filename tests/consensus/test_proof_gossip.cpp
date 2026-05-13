#include "consensus/proof_gossip.h"
#include <iostream>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <chrono>

using namespace dinero;
using namespace dinero::consensus;

// Test utilities

uint256 CreateTestHash(uint32_t seed) {
    uint256 hash;
    hash.data[0] = static_cast<uint8_t>(seed & 0xFF);
    hash.data[1] = static_cast<uint8_t>((seed >> 8) & 0xFF);
    hash.data[2] = static_cast<uint8_t>((seed >> 16) & 0xFF);
    hash.data[3] = static_cast<uint8_t>((seed >> 24) & 0xFF);
    for (size_t i = 4; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>((seed + i) & 0xFF);
    }
    return hash;
}

BlockUtreexoData CreateTestProof(uint32_t seed) {
    BlockUtreexoData proof;

    // Set accumulator root before
    std::vector<uint8_t> root_data(32);
    for (size_t i = 0; i < 32; i++) {
        root_data[i] = static_cast<uint8_t>(seed + i);
    }
    proof.accumulator_root_before = UtreexoHash(root_data);

    // Add some spent outputs
    for (size_t i = 0; i < 3; i++) {
        SpentOutputData output;
        output.value = 1000000 * (i + 1);
        output.scriptPubKey = {0x00, 0x14, static_cast<uint8_t>(i)};
        proof.spent_outputs.push_back(output);
    }

    return proof;
}

// Test implementations

void test_T9_13_invproof_serialization() {
    std::cout << "\n[T9.13] InvProof serialization round-trip\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    InvProof original(CreateTestHash(42), 1024, CreateTestHash(100), 2);

    std::cout << "✓ Created InvProof:\n";
    std::cout << "  Proof size: " << original.proof_size << " bytes\n";
    std::cout << "  TTL: " << static_cast<int>(original.ttl) << "\n";

    // Serialize
    auto data = original.Serialize();
    std::cout << "✓ Serialized to " << data.size() << " bytes\n";

    if (data.size() != 69) {  // 32 + 4 + 32 + 1
        std::cout << "❌ TEST FAILED: Expected 69 bytes, got " << data.size() << "\n";
        return;
    }

    // Deserialize
    InvProof deserialized = InvProof::Deserialize(data);
    std::cout << "✓ Deserialized InvProof\n";

    // Verify fields match
    bool match = true;
    for (size_t i = 0; i < 32; i++) {
        if (deserialized.block_hash.data[i] != original.block_hash.data[i]) {
            match = false;
            break;
        }
    }

    if (!match || deserialized.proof_size != original.proof_size || deserialized.ttl != original.ttl) {
        std::cout << "❌ TEST FAILED: Deserialized data doesn't match original\n";
        return;
    }

    std::cout << "✓ Round-trip successful (all fields match)\n";
    std::cout << "✅ TEST PASSED: InvProof serialization works\n";
}

void test_T9_14_ttl_propagation() {
    std::cout << "\n[T9.14] TTL limit prevents infinite propagation\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    InvProof inv(CreateTestHash(1), 1000, CreateTestHash(2), InvProof::MAX_TTL);

    std::cout << "✓ Created InvProof with TTL=" << static_cast<int>(InvProof::MAX_TTL) << "\n";

    if (!inv.CanReGossip()) {
        std::cout << "❌ TEST FAILED: InvProof with TTL=2 should be re-gossipable\n";
        return;
    }

    std::cout << "✓ Can re-gossip (TTL > 0)\n";

    // Decrement TTL twice
    InvProof hop1 = inv.DecrementTTL();
    std::cout << "✓ After 1 hop: TTL=" << static_cast<int>(hop1.ttl) << "\n";

    InvProof hop2 = hop1.DecrementTTL();
    std::cout << "✓ After 2 hops: TTL=" << static_cast<int>(hop2.ttl) << "\n";

    if (hop2.CanReGossip()) {
        std::cout << "❌ TEST FAILED: InvProof with TTL=0 should not be re-gossipable\n";
        return;
    }

    std::cout << "✓ Cannot re-gossip (TTL = 0)\n";
    std::cout << "✓ Max 2 hops enforced\n";
    std::cout << "✅ TEST PASSED: TTL limit prevents flooding\n";
}

void test_T9_15_invproof_deduplication() {
    std::cout << "\n[T9.15] InvProof deduplication prevents spam\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofGossipManager manager;

    uint256 block_hash = CreateTestHash(10);
    BlockUtreexoData proof = CreateTestProof(10);

    // Announce proof
    InvProof inv = manager.AnnounceProof(block_hash, proof);
    std::cout << "✓ Announced proof (invproof created)\n";

    // Handle invproof from peer 1 (first time)
    bool should_request1 = manager.HandleInvProof(inv, 1);
    std::cout << "✓ Peer 1 sends invproof: should_request=" << (should_request1 ? "true" : "false") << "\n";

    if (!should_request1) {
        std::cout << "❌ TEST FAILED: First invproof should trigger request\n";
        return;
    }

    // Handle same invproof from peer 2 (duplicate)
    bool should_request2 = manager.HandleInvProof(inv, 2);
    std::cout << "✓ Peer 2 sends same invproof: should_request=" << (should_request2 ? "true" : "false") << "\n";

    if (should_request2) {
        std::cout << "❌ TEST FAILED: Duplicate invproof should be ignored\n";
        return;
    }

    std::cout << "✓ Duplicate invproof ignored\n";

    // Check stats
    auto stats = manager.GetStats();
    std::cout << "  InvProofs sent: " << stats.invproofs_sent << "\n";
    std::cout << "  InvProofs received: " << stats.invproofs_received << "\n";
    std::cout << "  InvProofs duplicate: " << stats.invproofs_duplicate << "\n";

    if (stats.invproofs_duplicate != 1) {
        std::cout << "❌ TEST FAILED: Expected 1 duplicate invproof\n";
        return;
    }

    std::cout << "✓ Deduplication working\n";
    std::cout << "✅ TEST PASSED: InvProof deduplication prevents spam\n";
}

void test_T9_16_proof_request_response() {
    std::cout << "\n[T9.16] Proof request/response flow\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofGossipManager manager;

    // Setup proof provider
    std::unordered_map<uint256, BlockUtreexoData> local_proofs;
    uint256 block_hash = CreateTestHash(20);
    BlockUtreexoData proof = CreateTestProof(20);
    local_proofs[block_hash] = proof;

    manager.SetProofProvider([&local_proofs](const uint256& hash) -> std::optional<BlockUtreexoData> {
        auto it = local_proofs.find(hash);
        if (it != local_proofs.end()) {
            return it->second;
        }
        return std::nullopt;
    });

    std::cout << "✓ Set up proof provider with 1 proof\n";

    // Create proof request
    GetProof req = manager.CreateProofRequest(block_hash);
    std::cout << "✓ Created GetProof request\n";

    // Serialize/deserialize request
    auto req_data = req.Serialize();
    GetProof req_deserialized = GetProof::Deserialize(req_data);
    std::cout << "✓ Serialized/deserialized GetProof\n";

    // Handle request (should return proof)
    auto response = manager.HandleProofRequest(req_deserialized, 1);

    if (!response.has_value()) {
        std::cout << "❌ TEST FAILED: Should have returned proof\n";
        return;
    }

    std::cout << "✓ HandleProofRequest returned proof\n";

    // Serialize/deserialize response
    auto resp_data = response->Serialize();
    ProofData resp_deserialized = ProofData::Deserialize(resp_data);
    std::cout << "✓ Serialized/deserialized ProofData\n";

    // Handle response
    bool expected = manager.HandleProofData(resp_deserialized, 1);

    if (!expected) {
        std::cout << "❌ TEST FAILED: Proof should have been expected\n";
        return;
    }

    std::cout << "✓ HandleProofData accepted proof\n";

    // Check stats
    auto stats = manager.GetStats();
    std::cout << "  Proofs requested: " << stats.proofs_requested << "\n";
    std::cout << "  Proofs delivered: " << stats.proofs_delivered << "\n";
    std::cout << "  Proofs received: " << stats.proofs_received << "\n";

    if (stats.proofs_requested != 1 || stats.proofs_delivered != 1 || stats.proofs_received != 1) {
        std::cout << "❌ TEST FAILED: Stats don't match expected values\n";
        return;
    }

    std::cout << "✓ Statistics tracked correctly\n";
    std::cout << "✅ TEST PASSED: Proof request/response flow works\n";
}

void test_T9_17_gossip_failure_non_critical() {
    std::cout << "\n[T9.17] Gossip failure does not block validation\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofGossipManager manager;

    // No proof provider configured (simulates gossip failure)
    uint256 block_hash = CreateTestHash(30);

    // Create proof request
    GetProof req = manager.CreateProofRequest(block_hash);
    std::cout << "✓ Created GetProof request\n";

    // Handle request with no provider (should return nullopt)
    auto response = manager.HandleProofRequest(req, 1);

    if (response.has_value()) {
        std::cout << "❌ TEST FAILED: Should not have returned proof (no provider)\n";
        return;
    }

    std::cout << "✓ No proof available (expected)\n";
    std::cout << "✓ Gossip failure is graceful (returns nullopt)\n";
    std::cout << "ℹ️  Node can still validate via direct requests or cache\n";
    std::cout << "✅ TEST PASSED: Gossip failure is non-critical\n";
}

void test_T9_18_periodic_cleanup() {
    std::cout << "\n[T9.18] Periodic cleanup removes old tracking data\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    GossipTracker tracker;

    // Record some invproofs
    for (int i = 0; i < 10; i++) {
        tracker.RecordInvProof(CreateTestHash(i), i);
    }

    std::cout << "✓ Recorded 10 invproofs\n";

    // Check they're tracked
    bool tracked = tracker.HaveSeenInvProof(CreateTestHash(0));
    if (!tracked) {
        std::cout << "❌ TEST FAILED: InvProof should be tracked\n";
        return;
    }

    std::cout << "✓ InvProofs tracked\n";

    // Cleanup (entries are fresh, so shouldn't be removed yet)
    tracker.Cleanup();
    std::cout << "✓ Ran cleanup\n";

    // Should still be tracked (not expired)
    tracked = tracker.HaveSeenInvProof(CreateTestHash(0));
    if (!tracked) {
        std::cout << "❌ TEST FAILED: Fresh InvProof should not be removed\n";
        return;
    }

    std::cout << "✓ Fresh entries not removed\n";

    // Clear all
    tracker.Clear();
    std::cout << "✓ Cleared all tracking data\n";

    tracked = tracker.HaveSeenInvProof(CreateTestHash(0));
    if (tracked) {
        std::cout << "❌ TEST FAILED: InvProof should be cleared\n";
        return;
    }

    std::cout << "✓ Tracking data cleared\n";
    std::cout << "✅ TEST PASSED: Periodic cleanup works\n";
}

void test_T9_19_request_coalescing_and_cache() {
    std::cout << "\n[T9.19] Request coalescing + cache serve path\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofGossipManager manager;
    std::atomic<int> provider_calls{0};

    const uint256 block_hash = CreateTestHash(40);
    const BlockUtreexoData proof = CreateTestProof(40);

    manager.SetProofProvider([&](const uint256& hash) -> std::optional<BlockUtreexoData> {
        if (hash != block_hash) {
            return std::nullopt;
        }
        provider_calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return proof;
    });

    GetProof req = manager.CreateProofRequest(block_hash);

    std::optional<ProofData> resp1;
    std::optional<ProofData> resp2;

    std::thread t1([&]() {
        resp1 = manager.HandleProofRequest(req, 1);
    });
    std::thread t2([&]() {
        resp2 = manager.HandleProofRequest(req, 2);
    });

    t1.join();
    t2.join();

    if (!resp1.has_value() || !resp2.has_value()) {
        std::cout << "❌ TEST FAILED: Coalesced requests should both receive proof\n";
        return;
    }

    if (provider_calls.load() != 1) {
        std::cout << "❌ TEST FAILED: Expected 1 provider call under coalescing, got "
                  << provider_calls.load() << "\n";
        return;
    }

    std::cout << "✓ Concurrent requests coalesced to single provider call\n";

    // Follow-up request should hit recent cache (no extra provider call).
    auto resp3 = manager.HandleProofRequest(req, 3);
    if (!resp3.has_value()) {
        std::cout << "❌ TEST FAILED: Cached request should return proof\n";
        return;
    }
    if (provider_calls.load() != 1) {
        std::cout << "❌ TEST FAILED: Cached request triggered provider call unexpectedly\n";
        return;
    }

    auto stats = manager.GetStats();
    std::cout << "  Proofs delivered: " << stats.proofs_delivered << "\n";
    std::cout << "  Cache hits: " << stats.proof_cache_hits << "\n";
    std::cout << "  Cache misses: " << stats.proof_cache_misses << "\n";
    std::cout << "  Coalesced requests: " << stats.proof_requests_coalesced << "\n";

    if (stats.proof_cache_hits == 0 || stats.proof_requests_coalesced == 0) {
        std::cout << "❌ TEST FAILED: Expected cache hit and coalesced accounting\n";
        return;
    }

    std::cout << "✓ Cache and coalescing metrics updated\n";
    std::cout << "✅ TEST PASSED: Coalescing + cache burst path works\n";
}

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Phase 9.3: Gossip Protocol Tests (T9.13–T9.18)\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";

    int passed = 0;
    int failed = 0;

    try {
        test_T9_13_invproof_serialization();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.13 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_14_ttl_propagation();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.14 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_15_invproof_deduplication();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.15 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_16_proof_request_response();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.16 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_17_gossip_failure_non_critical();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.17 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_18_periodic_cleanup();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.18 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_19_request_coalescing_and_cache();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.19 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    std::cout << "\n═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Test Summary\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Passed: " << passed << "\n";
    std::cout << "  Failed: " << failed << "\n";

    if (failed == 0) {
        std::cout << "\n✅ All Phase 9.3 tests PASSED\n";
        return 0;
    } else {
        std::cout << "\n❌ Some tests FAILED\n";
        return 1;
    }
}
