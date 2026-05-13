// Ring 3 Phase 2: Property Test Framework Smoke Tests
// Tests the property-based testing framework for P2P protocol verification

#include "property_test_framework.h"
#include <gtest/gtest.h>

using namespace dinero::p2p::test;

// ============================================================================
// ConnectionSequenceGenerator Tests
// ============================================================================

TEST(ConnectionSequenceGenerator, GenerateValidHandshake) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto events = gen.generate_valid_handshake();

    // Valid handshake: CONNECT → VERSION → VERSION → VERACK → VERACK
    ASSERT_GE(events.size(), 5);
    EXPECT_EQ(events[0].type, ConnectionEventType::CONNECT);
    EXPECT_EQ(events[1].type, ConnectionEventType::SEND_VERSION);
    EXPECT_EQ(events[2].type, ConnectionEventType::RECV_VERSION);
    EXPECT_EQ(events[3].type, ConnectionEventType::SEND_VERACK);
    EXPECT_EQ(events[4].type, ConnectionEventType::RECV_VERACK);

    // Timestamps should be monotonically increasing
    for (size_t i = 1; i < events.size(); i++) {
        EXPECT_GT(events[i].timestamp_ms, events[i-1].timestamp_ms);
    }
}

TEST(ConnectionSequenceGenerator, GenerateInvalidHandshake) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto events = gen.generate_invalid_handshake();

    // Invalid: sends message before VERSION
    ASSERT_GE(events.size(), 3);
    EXPECT_EQ(events[0].type, ConnectionEventType::CONNECT);
    EXPECT_EQ(events[1].type, ConnectionEventType::SEND_MESSAGE);  // Wrong!
    EXPECT_EQ(events[2].type, ConnectionEventType::SEND_VERSION);
}

TEST(ConnectionSequenceGenerator, GenerateRandomSequence) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator::Config config;
    config.min_events = 10;
    config.max_events = 20;
    // Why disable disconnect/timeout: generate_random_sequence() breaks out of
    // its event loop early when rng_.boolean(disconnect_probability) fires,
    // so with the defaults the produced size can be < min_events depending
    // on RNG draws. Zeroing both probabilities makes this assertion stable
    // against any portable RNG ordering.
    config.disconnect_probability = 0.0;
    config.timeout_probability = 0.0;

    ConnectionSequenceGenerator gen(rng, config);

    auto events = gen.generate_random_sequence();

    // Should have at least min_events
    EXPECT_GE(events.size(), config.min_events);

    // First event must be CONNECT
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events[0].type, ConnectionEventType::CONNECT);
}

TEST(ConnectionSequenceGenerator, GeneratePingPongSequence) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    size_t num_pings = 5;
    auto events = gen.generate_ping_pong_sequence(num_pings);

    // Count PING and PONG events
    size_t ping_count = 0;
    size_t pong_count = 0;

    for (const auto& event : events) {
        if (event.type == ConnectionEventType::SEND_PING) {
            ping_count++;
            EXPECT_TRUE(event.nonce.has_value());  // PING must have nonce
        }
        if (event.type == ConnectionEventType::RECV_PONG) {
            pong_count++;
            EXPECT_TRUE(event.nonce.has_value());  // PONG must have nonce
        }
    }

    EXPECT_EQ(ping_count, num_pings);
    EXPECT_EQ(pong_count, num_pings);
}

TEST(ConnectionSequenceGenerator, GenerateStalePeerSequence) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto events = gen.generate_stale_peer_sequence();

    // Should contain PING but no PONG
    bool found_ping = false;
    bool found_pong = false;
    bool found_timeout = false;

    for (const auto& event : events) {
        if (event.type == ConnectionEventType::SEND_PING) found_ping = true;
        if (event.type == ConnectionEventType::RECV_PONG) found_pong = true;
        if (event.type == ConnectionEventType::TIMEOUT) found_timeout = true;
    }

    EXPECT_TRUE(found_ping);
    EXPECT_FALSE(found_pong);    // Stale peer never responds
    EXPECT_TRUE(found_timeout);  // Timeout occurs
}

TEST(ConnectionSequenceGenerator, DeterministicGeneration) {
    PropertyTestRNG rng1(42);
    PropertyTestRNG rng2(42);

    ConnectionSequenceGenerator gen1(rng1);
    ConnectionSequenceGenerator gen2(rng2);

    auto events1 = gen1.generate_random_sequence();
    auto events2 = gen2.generate_random_sequence();

    // Same seed → same sequence
    ASSERT_EQ(events1.size(), events2.size());
    for (size_t i = 0; i < events1.size(); i++) {
        EXPECT_EQ(events1[i].type, events2[i].type);
        EXPECT_EQ(events1[i].timestamp_ms, events2[i].timestamp_ms);
    }
}

// ============================================================================
// ConnectionStateMachine Tests
// ============================================================================

TEST(ConnectionStateMachine, InitialStateIsDisconnected) {
    ConnectionStateMachine sm;
    EXPECT_EQ(sm.state(), ConnectionState::DISCONNECTED);
}

TEST(ConnectionStateMachine, ValidHandshakeProgression) {
    ConnectionStateMachine sm;

    // DISCONNECTED → CONNECTING
    EXPECT_TRUE(sm.process_event({ConnectionEventType::CONNECT, 0}));
    EXPECT_EQ(sm.state(), ConnectionState::CONNECTING);

    // CONNECTING → HANDSHAKING
    EXPECT_TRUE(sm.process_event({ConnectionEventType::SEND_VERSION, 10}));
    EXPECT_EQ(sm.state(), ConnectionState::HANDSHAKING);

    // Still HANDSHAKING (waiting for VERACKs)
    EXPECT_TRUE(sm.process_event({ConnectionEventType::RECV_VERSION, 20}));
    EXPECT_EQ(sm.state(), ConnectionState::HANDSHAKING);

    // Send VERACK (still HANDSHAKING, waiting for RECV_VERACK)
    EXPECT_TRUE(sm.process_event({ConnectionEventType::SEND_VERACK, 30}));
    EXPECT_EQ(sm.state(), ConnectionState::HANDSHAKING);

    // HANDSHAKING → ESTABLISHED (after both VERACKs)
    EXPECT_TRUE(sm.process_event({ConnectionEventType::RECV_VERACK, 40}));
    EXPECT_EQ(sm.state(), ConnectionState::ESTABLISHED);
}

TEST(ConnectionStateMachine, InvalidEventRejected) {
    ConnectionStateMachine sm;

    // Cannot send VERSION before CONNECT
    EXPECT_FALSE(sm.process_event({ConnectionEventType::SEND_VERSION, 0}));
    EXPECT_EQ(sm.state(), ConnectionState::DISCONNECTED);
}

TEST(ConnectionStateMachine, MessagesOnlyInEstablished) {
    ConnectionStateMachine sm;

    // Cannot send messages before ESTABLISHED
    EXPECT_FALSE(sm.process_event({ConnectionEventType::SEND_MESSAGE, 0}));

    // Set up ESTABLISHED state
    sm.process_event({ConnectionEventType::CONNECT, 0});
    sm.process_event({ConnectionEventType::SEND_VERSION, 10});
    sm.process_event({ConnectionEventType::RECV_VERSION, 20});
    sm.process_event({ConnectionEventType::SEND_VERACK, 30});
    sm.process_event({ConnectionEventType::RECV_VERACK, 40});

    // Now messages allowed
    EXPECT_TRUE(sm.process_event({ConnectionEventType::SEND_MESSAGE, 50}));
}

TEST(ConnectionStateMachine, TimeoutAlwaysValid) {
    ConnectionStateMachine sm;

    // Timeout valid in any state
    EXPECT_TRUE(sm.process_event({ConnectionEventType::TIMEOUT, 0}));

    sm.process_event({ConnectionEventType::CONNECT, 10});
    EXPECT_TRUE(sm.process_event({ConnectionEventType::TIMEOUT, 20}));
}

// ============================================================================
// InvariantChecker Tests
// ============================================================================

TEST(InvariantChecker, ValidSequenceNoViolations) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto events = gen.generate_valid_handshake();
    InvariantChecker checker;

    auto violations = checker.check_sequence(events);
    EXPECT_TRUE(violations.empty());
}

TEST(InvariantChecker, InvalidSequenceDetected) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto events = gen.generate_invalid_handshake();
    InvariantChecker checker;

    auto violations = checker.check_sequence(events);
    EXPECT_FALSE(violations.empty());  // Should detect protocol violation
}

TEST(InvariantChecker, MonotonicityVerification) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto valid_events = gen.generate_valid_handshake();
    InvariantChecker checker1;
    EXPECT_TRUE(checker1.verify_monotonicity(valid_events));

    // Invalid sequence (message before handshake)
    std::vector<ConnectionEvent> invalid_events = {
        {ConnectionEventType::CONNECT, 0},
        {ConnectionEventType::SEND_MESSAGE, 10},  // Wrong!
    };
    InvariantChecker checker2;
    // May not detect monotonicity violation (depends on state machine impl)
    // But should detect as invalid event
    auto violations = checker2.check_sequence(invalid_events);
    EXPECT_FALSE(violations.empty());
}

TEST(InvariantChecker, StateTransitionValidity) {
    ConnectionStateMachine sm;

    // Valid transitions
    EXPECT_TRUE(sm.is_valid_transition(ConnectionState::DISCONNECTED, ConnectionState::CONNECTING));
    EXPECT_TRUE(sm.is_valid_transition(ConnectionState::CONNECTING, ConnectionState::HANDSHAKING));
    EXPECT_TRUE(sm.is_valid_transition(ConnectionState::HANDSHAKING, ConnectionState::ESTABLISHED));

    // Invalid transitions
    EXPECT_FALSE(sm.is_valid_transition(ConnectionState::ESTABLISHED, ConnectionState::CONNECTING));
    EXPECT_FALSE(sm.is_valid_transition(ConnectionState::DISCONNECTED, ConnectionState::ESTABLISHED));
}

// ============================================================================
// PropertyTest DSL Tests
// ============================================================================

TEST(PropertyTest, ForAllBasicUsage) {
    PropertyTestRNG rng(42);

    PropertyTest test("all numbers are positive");
    test.iterations(100);

    auto result = test.forAll(
        [&rng]() { return rng.uint32(1, 1000); },
        [](uint32_t x) { return x > 0; }
    );

    EXPECT_TRUE(result.passed);
    EXPECT_EQ(result.num_iterations, 100);
    EXPECT_EQ(result.num_failures, 0);
}

TEST(PropertyTest, DetectsFailures) {
    PropertyTestRNG rng(42);

    PropertyTest test("all numbers are even");
    test.iterations(100);

    auto result = test.forAll(
        [&rng]() { return rng.uint32(0, 100); },
        [](uint32_t x) { return x % 2 == 0; }
    );

    EXPECT_FALSE(result.passed);  // Should fail (some odd numbers)
    EXPECT_GT(result.num_failures, 0);
}

TEST(PropertyTest, ExceptionHandling) {
    PropertyTestRNG rng(42);

    PropertyTest test("throws exception");
    test.iterations(10);

    auto result = test.forAll(
        [&rng]() { return rng.uint32(0, 10); },
        [](uint32_t x) -> bool {
            if (x == 5) throw std::runtime_error("test exception");
            return true;
        }
    );

    // Should catch exception and record as failure
    EXPECT_FALSE(result.passed);
    EXPECT_GT(result.num_failures, 0);
}

TEST(PropertyTest, SummaryFormat) {
    PropertyTestResult result;
    result.num_iterations = 100;
    result.num_failures = 5;
    result.passed = false;

    std::string summary = result.summary();
    EXPECT_NE(summary.find("100"), std::string::npos);  // Contains iteration count
    EXPECT_NE(summary.find("5"), std::string::npos);    // Contains failure count
    EXPECT_NE(summary.find("FAIL"), std::string::npos); // Contains status
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(PropertyFrameworkIntegration, ValidHandshakesAlwaysPass) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto result = execute_sequence_test(
        "valid handshakes",
        [&gen]() { return gen.generate_valid_handshake(); },
        100
    );

    EXPECT_TRUE(result.passed);
    EXPECT_EQ(result.num_failures, 0);
}

TEST(PropertyFrameworkIntegration, InvalidHandshakesAlwaysFail) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    auto result = execute_sequence_test(
        "invalid handshakes",
        [&gen]() { return gen.generate_invalid_handshake(); },
        100
    );

    EXPECT_FALSE(result.passed);  // Should detect violations
    EXPECT_EQ(result.num_failures, 100);  // All invalid
}

TEST(PropertyFrameworkIntegration, RandomSequencesProduceVariety) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    std::set<size_t> sequence_lengths;

    for (int i = 0; i < 50; i++) {
        auto events = gen.generate_random_sequence();
        sequence_lengths.insert(events.size());
    }

    // Should generate sequences of varying lengths
    EXPECT_GT(sequence_lengths.size(), 5);
}

TEST(PropertyFrameworkIntegration, PingPongSequencesWellFormed) {
    PropertyTestRNG rng(42);
    ConnectionSequenceGenerator gen(rng);

    for (size_t num_pings : {1, 5, 10}) {
        auto events = gen.generate_ping_pong_sequence(num_pings);

        // Verify all PINGs have matching PONGs
        std::map<uint64_t, int> nonce_counts;  // +1 for PING, -1 for PONG

        for (const auto& event : events) {
            if (event.type == ConnectionEventType::SEND_PING && event.nonce.has_value()) {
                nonce_counts[event.nonce.value()]++;
            }
            if (event.type == ConnectionEventType::RECV_PONG && event.nonce.has_value()) {
                nonce_counts[event.nonce.value()]--;
            }
        }

        // All nonces should balance (PING + PONG = 0)
        for (const auto& [nonce, count] : nonce_counts) {
            EXPECT_EQ(count, 0) << "Nonce " << nonce << " unbalanced";
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
