// Ring 3 Phase 1: Peer Simulator Smoke Tests
// Tests basic functionality of deterministic P2P testing infrastructure

#include "peer_simulator.h"
#include <gtest/gtest.h>
#include <cassert>

using namespace dinero::p2p::test;

// ============================================================================
// PropertyTestRNG Tests
// ============================================================================

TEST(PropertyTestRNG, DeterministicGeneration) {
    PropertyTestRNG rng1(42);
    PropertyTestRNG rng2(42);

    // Same seed → same sequence
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(rng1.uint32(0, 1000), rng2.uint32(0, 1000));
    }
}

TEST(PropertyTestRNG, DifferentSeedsDifferentSequence) {
    PropertyTestRNG rng1(42);
    PropertyTestRNG rng2(43);

    // Different seeds → likely different sequence
    bool found_difference = false;
    for (int i = 0; i < 100; i++) {
        if (rng1.uint32(0, 1000) != rng2.uint32(0, 1000)) {
            found_difference = true;
            break;
        }
    }
    EXPECT_TRUE(found_difference);
}

TEST(PropertyTestRNG, ReseedProducesSameSequence) {
    PropertyTestRNG rng(42);

    std::vector<uint32_t> sequence1;
    for (int i = 0; i < 10; i++) {
        sequence1.push_back(rng.uint32(0, 1000));
    }

    rng.reseed(42);

    std::vector<uint32_t> sequence2;
    for (int i = 0; i < 10; i++) {
        sequence2.push_back(rng.uint32(0, 1000));
    }

    EXPECT_EQ(sequence1, sequence2);
}

TEST(PropertyTestRNG, BytesGeneration) {
    PropertyTestRNG rng(42);

    auto bytes = rng.bytes(100);
    EXPECT_EQ(bytes.size(), 100);

    // Verify not all zeros (extremely unlikely)
    bool has_nonzero = false;
    for (auto b : bytes) {
        if (b != 0) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);
}

// ============================================================================
// MockSocket Tests
// ============================================================================

TEST(MockSocket, BasicReadWrite) {
    PropertyTestRNG rng(42);
    MockSocket socket(rng);

    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};

    // Write to send buffer
    EXPECT_TRUE(socket.write(msg));
    EXPECT_EQ(socket.send_buffer_size(), 1);

    // Drain send buffer (simulate network delivery)
    auto delivered = socket.drain_send_buffer();
    ASSERT_EQ(delivered.size(), 1);
    EXPECT_EQ(delivered[0], msg);
}

TEST(MockSocket, LatencySimulation) {
    PropertyTestRNG rng(42);
    MockSocket::Config config;
    config.latency_ms = 100;  // 100ms latency
    MockSocket socket(rng, config);

    std::vector<uint8_t> msg = {1, 2, 3};
    socket.write(msg);

    // Message not delivered yet (latency not elapsed)
    auto delivered1 = socket.drain_send_buffer();
    EXPECT_EQ(delivered1.size(), 0);

    // Advance time by 50ms (still not enough)
    socket.advance_time(50);
    auto delivered2 = socket.drain_send_buffer();
    EXPECT_EQ(delivered2.size(), 0);

    // Advance time by another 50ms (total 100ms - latency met)
    socket.advance_time(50);
    auto delivered3 = socket.drain_send_buffer();
    ASSERT_EQ(delivered3.size(), 1);
    EXPECT_EQ(delivered3[0], msg);
}

TEST(MockSocket, PacketLoss) {
    PropertyTestRNG rng(42);
    MockSocket::Config config;
    config.packet_loss_rate = 1.0;  // 100% packet loss
    MockSocket socket(rng, config);

    // Write 10 messages
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> msg = {static_cast<uint8_t>(i)};
        EXPECT_TRUE(socket.write(msg));  // write() succeeds (sent to network)
    }

    // But nothing delivered (all lost)
    auto delivered = socket.drain_send_buffer();
    EXPECT_EQ(delivered.size(), 0);
}

TEST(MockSocket, InjectMessage) {
    PropertyTestRNG rng(42);
    MockSocket socket(rng);

    std::vector<uint8_t> msg = {9, 8, 7};
    socket.inject_message(msg);

    // Read from recv buffer
    auto received = socket.read();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received.value(), msg);
}

TEST(MockSocket, Disconnect) {
    PropertyTestRNG rng(42);
    MockSocket socket(rng);

    EXPECT_TRUE(socket.is_connected());

    socket.disconnect();
    EXPECT_FALSE(socket.is_connected());

    // Cannot write after disconnect
    std::vector<uint8_t> msg = {1, 2, 3};
    EXPECT_FALSE(socket.write(msg));

    // Cannot read after disconnect
    socket.inject_message(msg);
    auto received = socket.read();
    EXPECT_FALSE(received.has_value());
}

TEST(MockSocket, BackpressureWhenBufferFull) {
    PropertyTestRNG rng(42);
    MockSocket::Config config;
    config.max_send_buffer = 10;  // Very small buffer
    MockSocket socket(rng, config);

    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};

    // First write succeeds
    EXPECT_TRUE(socket.write(msg));

    // Second write succeeds
    EXPECT_TRUE(socket.write(msg));

    // Third write fails (buffer full: 10 bytes used, msg is 5 bytes)
    EXPECT_FALSE(socket.write(msg));
}

// ============================================================================
// PeerSimulator Tests
// ============================================================================

TEST(PeerSimulator, HonestHandshake) {
    PropertyTestRNG rng(42);
    PeerSimulator peer(rng, PeerBehavior::HONEST);

    // Send VERSION to peer
    auto version_msg = make_version_message();
    peer.on_message(version_msg);

    // Peer should respond with VERSION + VERACK
    auto response1 = peer.socket().read();
    ASSERT_TRUE(response1.has_value());
    EXPECT_EQ(response1.value()[0], 'V');
    EXPECT_EQ(response1.value()[1], 'E');
    EXPECT_EQ(response1.value()[2], 'R');
    EXPECT_EQ(response1.value()[3], 'S');  // VERSION

    auto response2 = peer.socket().read();
    ASSERT_TRUE(response2.has_value());
    EXPECT_EQ(response2.value()[0], 'V');
    EXPECT_EQ(response2.value()[1], 'E');
    EXPECT_EQ(response2.value()[2], 'R');
    EXPECT_EQ(response2.value()[3], 'A');  // VERACK
}

TEST(PeerSimulator, HonestPingPong) {
    PropertyTestRNG rng(42);
    PeerSimulator peer(rng, PeerBehavior::HONEST);

    // Send PING with nonce 12345
    auto ping_msg = make_ping_message(12345);
    peer.on_message(ping_msg);

    // Peer should respond with PONG (nonce echoed)
    auto pong = peer.socket().read();
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong.value()[0], 'P');
    EXPECT_EQ(pong.value()[1], 'O');  // PONG

    // Verify nonce matches (simplified check - just verify message echoed)
    EXPECT_EQ(pong.value().size(), ping_msg.size());
}

TEST(PeerSimulator, StaleNeverRespondsToPing) {
    PropertyTestRNG rng(42);
    PeerSimulator peer(rng, PeerBehavior::STALE);

    // Send VERSION → peer responds (completes handshake)
    auto version_msg = make_version_message();
    peer.on_message(version_msg);

    auto response1 = peer.socket().read();
    EXPECT_TRUE(response1.has_value());  // VERSION reply

    auto response2 = peer.socket().read();
    EXPECT_TRUE(response2.has_value());  // VERACK

    // Send PING → peer does NOT respond
    auto ping_msg = make_ping_message(99999);
    peer.on_message(ping_msg);

    auto pong = peer.socket().read();
    EXPECT_FALSE(pong.has_value());  // No PONG
}

TEST(PeerSimulator, ProtocolViolatorSendsMessagesOutOfOrder) {
    PropertyTestRNG rng(42);
    PeerSimulator peer(rng, PeerBehavior::PROTOCOL_VIOLATOR);

    // Send VERSION
    auto version_msg = make_version_message();
    peer.on_message(version_msg);

    // Peer sends VERACK first (wrong order)
    auto response1 = peer.socket().read();
    ASSERT_TRUE(response1.has_value());
    EXPECT_EQ(response1.value()[0], 'V');
    EXPECT_EQ(response1.value()[3], 'A');  // VERACK (should be VERSION)

    // Then sends VERSION (too late)
    auto response2 = peer.socket().read();
    ASSERT_TRUE(response2.has_value());
    EXPECT_EQ(response2.value()[3], 'S');  // VERSION
}

TEST(PeerSimulator, MaliciousSendsJunk) {
    PropertyTestRNG rng(42);
    PeerSimulator peer(rng, PeerBehavior::MALICIOUS);

    // Send VERSION
    auto version_msg = make_version_message();
    peer.on_message(version_msg);

    // Peer sends junk data
    auto response = peer.socket().read();
    ASSERT_TRUE(response.has_value());

    // Junk should not be a valid VERSION or VERACK
    auto msg = response.value();
    bool is_valid = (msg.size() >= 4 && msg[0] == 'V' && msg[1] == 'E');
    EXPECT_FALSE(is_valid);
}

TEST(PeerSimulator, BehaviorCanChange) {
    PropertyTestRNG rng(42);
    PeerSimulator peer(rng, PeerBehavior::HONEST);

    // Initially honest - responds to PING
    auto ping1 = make_ping_message(111);
    peer.on_message(ping1);
    EXPECT_TRUE(peer.socket().read().has_value());  // PONG received

    // Change to STALE - stops responding to PING
    peer.set_behavior(PeerBehavior::STALE);

    auto ping2 = make_ping_message(222);
    peer.on_message(ping2);
    EXPECT_FALSE(peer.socket().read().has_value());  // No PONG
}

// ============================================================================
// DeterministicScheduler Tests
// ============================================================================

TEST(DeterministicScheduler, EventsExecuteInTimeOrder) {
    DeterministicScheduler scheduler(42);

    std::vector<int> execution_order;

    scheduler.schedule_at(300, [&]() { execution_order.push_back(3); });
    scheduler.schedule_at(100, [&]() { execution_order.push_back(1); });
    scheduler.schedule_at(200, [&]() { execution_order.push_back(2); });

    scheduler.run_until_idle();

    ASSERT_EQ(execution_order.size(), 3);
    EXPECT_EQ(execution_order[0], 1);  // t=100
    EXPECT_EQ(execution_order[1], 2);  // t=200
    EXPECT_EQ(execution_order[2], 3);  // t=300
}

TEST(DeterministicScheduler, RunUntilStopsAtSpecifiedTime) {
    DeterministicScheduler scheduler(42);

    int executed = 0;

    scheduler.schedule_at(100, [&]() { executed++; });
    scheduler.schedule_at(200, [&]() { executed++; });
    scheduler.schedule_at(300, [&]() { executed++; });

    scheduler.run_until(150);

    EXPECT_EQ(executed, 1);  // Only first event executed
    EXPECT_EQ(scheduler.current_time(), 150);
}

TEST(DeterministicScheduler, ScheduleAfterUsesCurrentTime) {
    DeterministicScheduler scheduler(42);

    std::vector<uint64_t> times;

    scheduler.schedule_at(100, [&]() {
        times.push_back(scheduler.current_time());
        scheduler.schedule_after(50, [&]() {
            times.push_back(scheduler.current_time());
        });
    });

    scheduler.run_until_idle();

    ASSERT_EQ(times.size(), 2);
    EXPECT_EQ(times[0], 100);  // First event
    EXPECT_EQ(times[1], 150);  // Second event (100 + 50)
}

TEST(DeterministicScheduler, ScheduleNowExecutesImmediately) {
    DeterministicScheduler scheduler(42);

    std::vector<int> order;

    scheduler.schedule_at(100, [&]() {
        order.push_back(1);
        scheduler.schedule_now([&]() { order.push_back(2); });
    });

    scheduler.run_until_idle();

    ASSERT_EQ(order.size(), 2);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

TEST(DeterministicScheduler, HasPendingEventsDetectsEmptyQueue) {
    DeterministicScheduler scheduler(42);

    EXPECT_FALSE(scheduler.has_pending_events());

    scheduler.schedule_at(100, []() {});
    EXPECT_TRUE(scheduler.has_pending_events());

    scheduler.run_until_idle();
    EXPECT_FALSE(scheduler.has_pending_events());
}

// ============================================================================
// Integration Test: Simulated Handshake
// ============================================================================

TEST(PeerSimulatorIntegration, SimulatedHandshakeWithLatency) {
    PropertyTestRNG rng(42);
    DeterministicScheduler scheduler(42);

    // Create peer with 50ms network latency
    PeerSimulator peer(rng, PeerBehavior::HONEST);
    peer.socket().inject_latency(50);

    bool handshake_complete = false;

    // t=0: Send VERSION to peer
    scheduler.schedule_now([&]() {
        auto version = make_version_message();
        peer.on_message(version);
    });

    // t=50: Check for peer response (VERSION + VERACK should be in recv buffer)
    scheduler.schedule_at(50, [&]() {
        peer.socket().advance_time(50);

        auto version_reply = peer.socket().read();
        EXPECT_TRUE(version_reply.has_value());

        auto verack = peer.socket().read();
        EXPECT_TRUE(verack.has_value());

        handshake_complete = true;
    });

    scheduler.run_until_idle();

    EXPECT_TRUE(handshake_complete);
}

TEST(PeerSimulatorIntegration, SimulatedPingPongWithTimeout) {
    PropertyTestRNG rng(42);
    DeterministicScheduler scheduler(42);

    // Test both HONEST and STALE peers
    PeerSimulator honest_peer(rng, PeerBehavior::HONEST);
    PeerSimulator stale_peer(rng, PeerBehavior::STALE);

    bool honest_responded = false;
    bool stale_responded = false;

    // t=0: Send PING to both peers
    scheduler.schedule_now([&]() {
        auto ping = make_ping_message(42);
        honest_peer.on_message(ping);
        stale_peer.on_message(ping);
    });

    // t=100: Check responses
    scheduler.schedule_at(100, [&]() {
        // Honest peer should have PONG
        auto pong1 = honest_peer.socket().read();
        honest_responded = pong1.has_value();

        // Stale peer should NOT have PONG
        auto pong2 = stale_peer.socket().read();
        stale_responded = pong2.has_value();
    });

    scheduler.run_until_idle();

    EXPECT_TRUE(honest_responded);
    EXPECT_FALSE(stale_responded);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
