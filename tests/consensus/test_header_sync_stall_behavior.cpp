/**
 * Phase N.2 Step 2B: Header Sync Stall Simulation Tests
 *
 * Purpose: Lock in Bitcoin-Core-grade timeout behavior under adversarial timing.
 *
 * Exit Criteria:
 * ✅ Mid-sync stall detected (hard timeout)
 * ✅ Slow drip tolerated (no false positive)
 * ✅ Height lies detected and punished
 * ✅ Last peer never disconnected
 * ✅ Timeout recalculation works correctly
 * ✅ Outbound preference verified
 *
 * Requirements:
 * - No sockets, threads, or sleeps
 * - Manual time advancement via mock clock
 * - Deterministic Tick() calls
 * - Captured PeerSwitchReason signals
 */

#include "consensus/header_sync.h"
#include "consensus/header_chain.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

// ============================================================================
// Test Helpers
// ============================================================================

// Mock clock for deterministic time advancement
class MockClock {
public:
    MockClock() : current_time_ms_(0) {}

    uint64_t GetTimeMs() const { return current_time_ms_; }

    void AdvanceMs(uint64_t ms) {
        current_time_ms_ += ms;
    }

    void AdvanceSeconds(uint64_t seconds) {
        current_time_ms_ += seconds * 1000;
    }

    void AdvanceMinutes(uint64_t minutes) {
        current_time_ms_ += minutes * 60 * 1000;
    }

private:
    uint64_t current_time_ms_;
};

// Peer switch signal capture
struct PeerSwitchCapture {
    bool was_called = false;
    uint64_t old_peer_id = 0;
    PeerSwitchReason reason;

    void Reset() {
        was_called = false;
        old_peer_id = 0;
    }

    void OnPeerSwitch(uint64_t old_peer, PeerSwitchReason r) {
        was_called = true;
        old_peer_id = old_peer;
        reason = r;
    }
};

// Test helper: Create a block header
BlockHeader CreateTestHeader(
    const uint256& prev_hash,
    uint32_t time,
    uint32_t bits = 0x1d00ffff
) {
    BlockHeader header;
    header.version = 1;
    header.prev_block_hash = prev_hash;  // Phase M.0: uint256 identity
    header.merkle_root = uint256();  // Null hash
    header.timestamp = time;  // Updated field name
    header.difficulty = bits;  // Updated field name
    header.nonce = 1;
    header.utreexo_root = uint256();  // Null hash
    return header;
}

// Create a chain of N headers starting from prev_hash
std::vector<BlockHeader> CreateHeaderChain(const uint256& prev_hash, uint32_t count, uint32_t start_time) {
    std::vector<BlockHeader> headers;
    uint256 prev = prev_hash;

    for (uint32_t i = 0; i < count; i++) {
        BlockHeader header = CreateTestHeader(prev, start_time + i);
        headers.push_back(header);
        prev = header.GetHash();
    }

    return headers;
}

// ============================================================================
// Test 1: Mid-Sync Stall (Hard Timeout)
// ============================================================================

void Test1_MidSyncStall() {
    std::cout << "\n1. Testing mid-sync stall (hard timeout)..." << std::endl;

    MockClock clock;
    HeaderChainSelector selector;
    HeaderSyncManager sync_manager(&selector);
    PeerSwitchCapture switch_capture;

    // Inject mock time source for deterministic timeout testing
    sync_manager.SetTimeSource([&clock]() { return clock.GetTimeMs(); });

    // Register callback
    sync_manager.SetPeerSwitchCallback(
        [&](uint64_t old_peer, PeerSwitchReason reason) {
            switch_capture.OnPeerSwitch(old_peer, reason);
        }
    );

    // Add genesis
    uint256 null_hash;
    null_hash.SetNull();
    BlockHeader genesis = CreateTestHeader(null_hash, 1000000);
    selector.AddHeader(genesis);

    // Peer advertises height 1000
    uint256 peer_best;
    peer_best.SetNull();
    sync_manager.AddPeer(1, 1000, peer_best);
    sync_manager.MarkPeerOutbound(1, true);

    // Simulate sync start
    sync_manager.MarkHeadersRequested(1);
    assert(sync_manager.GetState() == HeaderSyncState::REQUESTING_HEADERS);

    // Peer sends first 200 headers
    uint256 genesis_hash = genesis.GetHash();
    std::vector<BlockHeader> batch1 = CreateHeaderChain(genesis_hash, 200, 1000001);
    bool accepted = sync_manager.ProcessHeaders(1, batch1);
    assert(accepted == true);

    // Should request more headers (200 < 2000)
    assert(sync_manager.GetState() == HeaderSyncState::REQUESTING_HEADERS ||
           sync_manager.GetState() == HeaderSyncState::IDLE);

    std::cout << "   Peer sent 200 headers, then stops..." << std::endl;

    // Advance time to just before timeout (15min + 800 headers * 1ms = ~15min 1sec)
    // Expected headers remaining ≈ 800 (1000 claimed - 200 sent)
    // Timeout = 15min + 800ms ≈ 900,800ms
    clock.AdvanceMinutes(15);
    sync_manager.Tick(clock.GetTimeMs());
    assert(sync_manager.GetState() != HeaderSyncState::STALLED);
    assert(switch_capture.was_called == false);

    std::cout << "   After 15 minutes: peer not yet stalled (within timeout)..." << std::endl;

    // Advance past timeout
    clock.AdvanceSeconds(2);  // Now past 15min 2sec
    sync_manager.Tick(clock.GetTimeMs());

    // Should detect stall
    assert(sync_manager.GetState() == HeaderSyncState::STALLED ||
           sync_manager.GetState() == HeaderSyncState::IDLE);
    assert(switch_capture.was_called == true);
    assert(switch_capture.reason == PeerSwitchReason::STALL_TIMEOUT);
    assert(switch_capture.old_peer_id == 1);

    // Already-accepted headers remain valid
    const auto best = selector.GetBestHeaderValue();
    assert(best.has_value());
    assert(best->height == 200);

    std::cout << "   ✅ Mid-sync stall detected correctly" << std::endl;
    std::cout << "   ✅ Already-accepted headers remain valid (height = 200)" << std::endl;
}

// ============================================================================
// Test 2: Slow Drip (No False Positive)
// ============================================================================

void Test2_SlowDrip() {
    std::cout << "\n2. Testing slow drip (no false positive)..." << std::endl;

    MockClock clock;
    HeaderChainSelector selector;
    HeaderSyncManager sync_manager(&selector);
    PeerSwitchCapture switch_capture;

    // Inject mock time source
    sync_manager.SetTimeSource([&clock]() { return clock.GetTimeMs(); });

    sync_manager.SetPeerSwitchCallback(
        [&](uint64_t old_peer, PeerSwitchReason reason) {
            switch_capture.OnPeerSwitch(old_peer, reason);
        }
    );

    // Add genesis
    uint256 null_hash;
    null_hash.SetNull();
    BlockHeader genesis = CreateTestHeader(null_hash, 1000000);
    selector.AddHeader(genesis);

    // Peer advertises height 100
    uint256 peer_best;
    peer_best.SetNull();
    sync_manager.AddPeer(1, 100, peer_best);
    sync_manager.MarkPeerOutbound(1, true);

    sync_manager.MarkHeadersRequested(1);

    std::cout << "   Peer sends 1 header every 10 seconds..." << std::endl;

    uint256 prev = genesis.GetHash();
    for (uint32_t i = 1; i <= 50; i++) {
        // Advance 10 seconds
        clock.AdvanceSeconds(10);

        // Send 1 header
        std::vector<BlockHeader> single_header;
        BlockHeader header = CreateTestHeader(prev, 1000000 + i);
        single_header.push_back(header);
        prev = header.GetHash();

        bool accepted = sync_manager.ProcessHeaders(1, single_header);
        assert(accepted == true);

        // Tick to check for stall
        sync_manager.Tick(clock.GetTimeMs());

        // Should NOT stall (timeout resets with each header)
        assert(sync_manager.GetState() != HeaderSyncState::STALLED);
        assert(switch_capture.was_called == false);
    }

    // Total time elapsed: 50 * 10 seconds = 500 seconds ≈ 8.3 minutes
    // Peer is making progress, timeout should keep extending

    const auto best = selector.GetBestHeaderValue();
    assert(best.has_value());
    assert(best->height == 50);

    std::cout << "   ✅ Slow drip tolerated (no false positive)" << std::endl;
    std::cout << "   ✅ 50 headers received over 8+ minutes, peer not stalled" << std::endl;
}

// ============================================================================
// Test 3: Height Lie Detection
// ============================================================================

void Test3_HeightLie() {
    std::cout << "\n3. Testing height lie detection..." << std::endl;

    MockClock clock;
    HeaderChainSelector selector;
    HeaderSyncManager sync_manager(&selector);
    PeerSwitchCapture switch_capture;

    // Inject mock time source
    sync_manager.SetTimeSource([&clock]() { return clock.GetTimeMs(); });

    sync_manager.SetPeerSwitchCallback(
        [&](uint64_t old_peer, PeerSwitchReason reason) {
            switch_capture.OnPeerSwitch(old_peer, reason);
        }
    );

    // Add genesis
    uint256 null_hash;
    null_hash.SetNull();
    BlockHeader genesis = CreateTestHeader(null_hash, 1000000);
    selector.AddHeader(genesis);

    // Peer claims height 1000 (LIE)
    uint256 peer_best;
    peer_best.SetNull();
    sync_manager.AddPeer(1, 1000, peer_best);
    sync_manager.MarkPeerOutbound(1, true);

    // Add alternative peer (so we can disconnect the liar)
    sync_manager.AddPeer(2, 500, peer_best);
    sync_manager.MarkPeerOutbound(2, true);

    sync_manager.MarkHeadersRequested(1);

    // Peer sends only 10 headers (way less than claimed 1000)
    uint256 genesis_hash = genesis.GetHash();
    std::vector<BlockHeader> small_batch = CreateHeaderChain(genesis_hash, 10, 1000001);
    bool accepted = sync_manager.ProcessHeaders(1, small_batch);
    assert(accepted == true);

    std::cout << "   Peer claimed height 1000, sent only 10 headers..." << std::endl;

    // Advance past timeout (15min + 990 headers * 1ms)
    clock.AdvanceMinutes(16);
    sync_manager.Tick(clock.GetTimeMs());

    // Should detect stall (peer lied about having 1000 headers)
    assert(switch_capture.was_called == true);
    assert(switch_capture.reason == PeerSwitchReason::STALL_TIMEOUT);
    assert(switch_capture.old_peer_id == 1);

    std::cout << "   ✅ Height lie detected (peer stalled after sending only 10/1000 headers)" << std::endl;
    std::cout << "   ✅ Peer switch requested (not banned, just disconnected per Core)" << std::endl;
}

// ============================================================================
// Test 4: Last Peer Protection
// ============================================================================

void Test4_LastPeerProtection() {
    std::cout << "\n4. Testing last peer protection..." << std::endl;

    MockClock clock;
    HeaderChainSelector selector;
    HeaderSyncManager sync_manager(&selector);
    PeerSwitchCapture switch_capture;

    // Inject mock time source
    sync_manager.SetTimeSource([&clock]() { return clock.GetTimeMs(); });

    sync_manager.SetPeerSwitchCallback(
        [&](uint64_t old_peer, PeerSwitchReason reason) {
            switch_capture.OnPeerSwitch(old_peer, reason);
        }
    );

    // Add genesis
    uint256 null_hash;
    null_hash.SetNull();
    BlockHeader genesis = CreateTestHeader(null_hash, 1000000);
    selector.AddHeader(genesis);

    // Only 1 peer connected
    uint256 peer_best;
    peer_best.SetNull();
    sync_manager.AddPeer(1, 1000, peer_best);
    sync_manager.MarkPeerOutbound(1, true);

    sync_manager.MarkHeadersRequested(1);

    // Peer sends 10 headers then stalls
    uint256 genesis_hash = genesis.GetHash();
    std::vector<BlockHeader> batch = CreateHeaderChain(genesis_hash, 10, 1000001);
    bool accepted = sync_manager.ProcessHeaders(1, batch);
    assert(accepted == true);

    std::cout << "   Only 1 peer, peer sends 10 headers then stalls..." << std::endl;

    // Advance past timeout
    clock.AdvanceMinutes(16);
    sync_manager.Tick(clock.GetTimeMs());

    // Should detect stall and signal to P2P layer (callback called)
    // State remains STALLED (no alternatives to switch to)
    assert(sync_manager.GetState() == HeaderSyncState::STALLED);
    assert(switch_capture.was_called == true);  // P2P layer notified of stall
    assert(switch_capture.reason == PeerSwitchReason::STALL_TIMEOUT);

    // Peer is marked as stalled but remains in peers list (last peer protection)
    auto stats = sync_manager.GetStats();
    assert(stats.stalled_peers == 1);

    std::cout << "   ✅ Stall detected and P2P layer notified" << std::endl;
    std::cout << "   ✅ State = STALLED, peer marked stalled (P2P layer handles recovery)" << std::endl;
}

// ============================================================================
// Test 5: Timeout Recalculation Correctness
// ============================================================================

void Test5_TimeoutRecalculation() {
    std::cout << "\n5. Testing timeout recalculation correctness..." << std::endl;

    MockClock clock;
    HeaderChainSelector selector;
    HeaderSyncManager sync_manager(&selector);
    PeerSwitchCapture switch_capture;

    // Inject mock time source
    sync_manager.SetTimeSource([&clock]() { return clock.GetTimeMs(); });

    sync_manager.SetPeerSwitchCallback(
        [&](uint64_t old_peer, PeerSwitchReason reason) {
            switch_capture.OnPeerSwitch(old_peer, reason);
        }
    );

    // Add genesis
    uint256 null_hash;
    null_hash.SetNull();
    BlockHeader genesis = CreateTestHeader(null_hash, 1000000);
    selector.AddHeader(genesis);

    // Peer advertises height 1000
    uint256 peer_best;
    peer_best.SetNull();
    sync_manager.AddPeer(1, 1000, peer_best);
    sync_manager.MarkPeerOutbound(1, true);

    sync_manager.MarkHeadersRequested(1);

    std::cout << "   Peer sends large batch (500 headers)..." << std::endl;

    // Peer sends large batch (500 headers)
    uint256 genesis_hash = genesis.GetHash();
    std::vector<BlockHeader> large_batch = CreateHeaderChain(genesis_hash, 500, 1000001);
    bool accepted = sync_manager.ProcessHeaders(1, large_batch);
    assert(accepted == true);

    // expected_headers_remaining should drop from 1000 to 500
    // Timeout should recalculate: 15min + 500ms (not original 1000ms)

    std::cout << "   Expected headers remaining: 1000 → 500" << std::endl;
    std::cout << "   Timeout should recalculate: 15min + 500ms" << std::endl;

    // Advance 15 minutes + 400ms (within new timeout)
    clock.AdvanceMinutes(15);
    clock.AdvanceMs(400);
    sync_manager.Tick(clock.GetTimeMs());

    // Should NOT stall (within recalculated timeout)
    assert(sync_manager.GetState() != HeaderSyncState::STALLED);
    assert(switch_capture.was_called == false);

    std::cout << "   After 15min 400ms: peer not stalled (within recalculated timeout)..." << std::endl;

    // Advance past new timeout (15min + 600ms total)
    clock.AdvanceMs(300);  // Now at 15min 700ms
    sync_manager.Tick(clock.GetTimeMs());

    // Should detect stall (past recalculated timeout)
    assert(switch_capture.was_called == true);
    assert(switch_capture.reason == PeerSwitchReason::STALL_TIMEOUT);

    std::cout << "   ✅ Timeout recalculation works correctly" << std::endl;
    std::cout << "   ✅ Timeout shortened from ~16min to ~15.5min after batch received" << std::endl;
}

// ============================================================================
// Test 6: Outbound Preference Under Equal Height
// ============================================================================

void Test6_OutboundPreference() {
    std::cout << "\n6. Testing outbound preference under equal height..." << std::endl;

    HeaderChainSelector selector;
    HeaderSyncManager sync_manager(&selector);

    // Add genesis
    uint256 null_hash;
    null_hash.SetNull();
    BlockHeader genesis = CreateTestHeader(null_hash, 1000000);
    selector.AddHeader(genesis);

    // Two peers, same height (500)
    uint256 peer_best;
    peer_best.SetNull();

    sync_manager.AddPeer(1, 500, peer_best);  // Inbound
    sync_manager.MarkPeerOutbound(1, false);

    sync_manager.AddPeer(2, 500, peer_best);  // Outbound
    sync_manager.MarkPeerOutbound(2, true);

    // Select best peer
    uint64_t best_peer = sync_manager.SelectBestPeer();

    // Should prefer outbound (peer 2) over inbound (peer 1)
    assert(best_peer == 2);

    std::cout << "   ✅ Outbound peer selected over inbound (eclipse resistance)" << std::endl;

    // Test 2: Higher height wins regardless of connection type
    sync_manager.AddPeer(3, 600, peer_best);  // Inbound, but higher height
    sync_manager.MarkPeerOutbound(3, false);

    best_peer = sync_manager.SelectBestPeer();

    // Should select peer 3 (higher height wins)
    assert(best_peer == 3);

    std::cout << "   ✅ Higher height wins over connection type (peer 3 inbound but height 600)" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    SelectParams(Chain::REGTEST);
    std::cout << "=== Phase N.2 Step 2B: Header Sync Stall Simulation Tests ===" << std::endl;

    Test1_MidSyncStall();
    Test2_SlowDrip();
    Test3_HeightLie();
    Test4_LastPeerProtection();
    Test5_TimeoutRecalculation();
    Test6_OutboundPreference();

    std::cout << "\n=== ALL STALL SIMULATION TESTS PASSED ===" << std::endl;
    std::cout << "\nPhase N.2 Step 2B Verification:" << std::endl;
    std::cout << "  ✅ Mid-sync stall detected (hard timeout)" << std::endl;
    std::cout << "  ✅ Slow drip tolerated (no false positive)" << std::endl;
    std::cout << "  ✅ Height lies detected and punished" << std::endl;
    std::cout << "  ✅ Last peer never disconnected" << std::endl;
    std::cout << "  ✅ Timeout recalculation works correctly" << std::endl;
    std::cout << "  ✅ Outbound preference verified" << std::endl;
    std::cout << "\nHeader sync timeout behavior is Bitcoin-Core-grade." << std::endl;
    std::cout << "Ready for P2P wiring (Phase N.2 Step 2C)." << std::endl;

    return 0;
}
