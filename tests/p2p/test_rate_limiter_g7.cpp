/**
 * Phase G.7: Rate Limiting & DoS Protection Tests
 *
 * Test Coverage:
 * - G.7.1: Token bucket algorithm (consume and refill)
 * - G.7.2: Message cost enforcement
 * - G.7.3: Rate limit violations and bans
 * - G.7.4: Peer removal and cleanup
 * - G.7.5: Statistics tracking
 * - G.7.6: Enable/disable functionality
 */

#include "p2p/rate_limiter.h"
#include "p2p/peer_scoring.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

using namespace dinero;

// ============================================================================
// Test G.7.1: Token Bucket Algorithm
// ============================================================================

void test_g7_1_token_bucket() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.7.1: Token Bucket Algorithm" << std::endl;
    std::cout << "========================================\n" << std::endl;

    RateLimiterConfig config;
    config.max_tokens = 100;
    config.refill_rate = 10.0;  // 10 tokens/second
    config.ban_threshold = 5;
    config.enabled = true;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    RateLimiter limiter(config, scoring);

    peer_id_t peer = "peer1:8333";

    std::cout << "Sending first message creates bucket with max tokens..." << std::endl;

    // First message creates bucket (costs 1 token)
    assert(limiter.allowMessage(peer, 1));
    double initial_tokens = limiter.getTokens(peer);
    std::cout << "Remaining tokens after first message: " << initial_tokens << std::endl;
    assert(initial_tokens >= config.max_tokens - 2);  // Should have max - 1 (consumed)
    std::cout << "✅ Bucket created with max tokens\n" << std::endl;

    // Consume some tokens
    std::cout << "Consuming 50 tokens..." << std::endl;
    assert(limiter.allowMessage(peer, 50));
    double remaining = limiter.getTokens(peer);
    std::cout << "Remaining tokens: " << remaining << std::endl;
    assert(remaining >= 49 && remaining <= 51);
    std::cout << "✅ Tokens consumed correctly\n" << std::endl;

    // Try to consume more than available
    std::cout << "Attempting to consume 60 tokens (should fail)..." << std::endl;
    assert(!limiter.allowMessage(peer, 60));
    std::cout << "✅ Rate limit enforced\n" << std::endl;

    std::cout << "✅ Test G.7.1 PASSED: Token bucket algorithm working\n" << std::endl;
}

// ============================================================================
// Test G.7.2: Message Cost Enforcement
// ============================================================================

void test_g7_2_message_costs() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.7.2: Message Cost Enforcement" << std::endl;
    std::cout << "========================================\n" << std::endl;

    RateLimiterConfig config;
    config.max_tokens = 100;
    config.refill_rate = 10.0;
    config.ban_threshold = 5;
    config.enabled = true;

    RateLimiter limiter(config, nullptr);

    peer_id_t peer = "peer2:8333";

    std::cout << "Testing different message costs..." << std::endl;

    // Low cost messages (PING = 1 token)
    std::cout << "\nPING messages (1 token each):" << std::endl;
    for (int i = 0; i < 50; i++) {
        assert(limiter.allowMessage(peer, MessageCost::PING));
    }
    double after_pings = limiter.getTokens(peer);
    std::cout << "  After 50 PINGs: " << after_pings << " tokens remaining" << std::endl;
    assert(after_pings >= 49 && after_pings <= 51);
    std::cout << "✅ Low-cost messages work\n" << std::endl;

    // Reset for next test
    limiter.removePeer(peer);

    // High cost messages (BLOCK = 50 tokens)
    std::cout << "BLOCK messages (50 tokens each):" << std::endl;
    assert(limiter.allowMessage(peer, MessageCost::BLOCK));
    assert(limiter.allowMessage(peer, MessageCost::BLOCK));
    // Third block should fail (need 50 tokens, but only ~0 left)
    assert(!limiter.allowMessage(peer, MessageCost::BLOCK));
    std::cout << "✅ High-cost messages enforced\n" << std::endl;

    std::cout << "✅ Test G.7.2 PASSED: Message costs working correctly\n" << std::endl;
}

// ============================================================================
// Test G.7.3: Rate Limit Violations and Bans
// ============================================================================

void test_g7_3_violations_and_bans() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.7.3: Violations and Bans" << std::endl;
    std::cout << "========================================\n" << std::endl;

    RateLimiterConfig config;
    config.max_tokens = 10;  // Small bucket
    config.refill_rate = 1.0;
    config.ban_threshold = 3;  // Ban after 3 violations
    config.enabled = true;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    RateLimiter limiter(config, scoring);

    peer_id_t peer = "peer_bad:8333";

    std::cout << "Exhausting token bucket..." << std::endl;

    // Exhaust tokens (10 tokens / 5 cost = 2 messages)
    assert(limiter.allowMessage(peer, 5));
    assert(limiter.allowMessage(peer, 5));
    std::cout << "✅ Tokens exhausted\n" << std::endl;

    std::cout << "\nCausing violations..." << std::endl;

    // Cause violations (should not allow)
    assert(!limiter.allowMessage(peer, 5));  // Violation 1
    assert(!limiter.allowMessage(peer, 5));  // Violation 2
    assert(!limiter.allowMessage(peer, 5));  // Violation 3 - triggers ban

    uint32_t violations = limiter.getViolations(peer);
    std::cout << "Violations recorded: " << violations << std::endl;

    // After ban threshold, violations reset to 0
    assert(violations == 0);
    std::cout << "✅ Violations reset after ban threshold\n" << std::endl;

    // Check misbehavior score was added
    int32_t score = scoring->getScore(peer);
    std::cout << "Misbehavior score: " << score << std::endl;
    assert(score > 0);
    std::cout << "✅ Misbehavior score added\n" << std::endl;

    std::cout << "✅ Test G.7.3 PASSED: Violations and bans working\n" << std::endl;
}

// ============================================================================
// Test G.7.4: Peer Removal
// ============================================================================

void test_g7_4_peer_removal() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.7.4: Peer Removal" << std::endl;
    std::cout << "========================================\n" << std::endl;

    RateLimiterConfig config;
    config.max_tokens = 100;
    config.refill_rate = 10.0;
    config.ban_threshold = 5;
    config.enabled = true;

    RateLimiter limiter(config, nullptr);

    peer_id_t peer1 = "peer1:8333";
    peer_id_t peer2 = "peer2:8333";

    // Use both peers
    limiter.allowMessage(peer1, 10);
    limiter.allowMessage(peer2, 10);

    auto stats_before = limiter.getStats();
    std::cout << "Peers before removal: " << stats_before.total_peers << std::endl;
    assert(stats_before.total_peers == 2);

    // Remove peer1
    std::cout << "Removing peer1..." << std::endl;
    limiter.removePeer(peer1);

    auto stats_after = limiter.getStats();
    std::cout << "Peers after removal: " << stats_after.total_peers << std::endl;
    assert(stats_after.total_peers == 1);

    // Removed peer should have no bucket (returns 0)
    double tokens = limiter.getTokens(peer1);
    std::cout << "Tokens for removed peer (should be 0): " << tokens << std::endl;
    assert(tokens == 0);

    // If peer sends message again, they start fresh with max tokens
    assert(limiter.allowMessage(peer1, 1));
    double new_tokens = limiter.getTokens(peer1);
    std::cout << "Tokens after re-creating peer: " << new_tokens << std::endl;
    assert(new_tokens >= config.max_tokens - 2);

    std::cout << "✅ Test G.7.4 PASSED: Peer removal working\n" << std::endl;
}

// ============================================================================
// Test G.7.5: Statistics Tracking
// ============================================================================

void test_g7_5_statistics() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.7.5: Statistics Tracking" << std::endl;
    std::cout << "========================================\n" << std::endl;

    RateLimiterConfig config;
    config.max_tokens = 50;
    config.refill_rate = 10.0;
    config.ban_threshold = 5;
    config.enabled = true;

    RateLimiter limiter(config, nullptr);

    peer_id_t peer = "peer_stats:8333";

    // Generate some activity
    std::cout << "Generating traffic..." << std::endl;
    limiter.allowMessage(peer, 10);  // Allowed
    limiter.allowMessage(peer, 10);  // Allowed
    limiter.allowMessage(peer, 10);  // Allowed
    limiter.allowMessage(peer, 10);  // Allowed
    limiter.allowMessage(peer, 10);  // Allowed
    limiter.allowMessage(peer, 20);  // Rejected (50 - 50 = 0, need 20)

    auto stats = limiter.getStats();
    std::cout << "\nStatistics:" << std::endl;
    std::cout << "  Total peers: " << stats.total_peers << std::endl;
    std::cout << "  Messages allowed: " << stats.total_messages_allowed << std::endl;
    std::cout << "  Messages rejected: " << stats.total_messages_rejected << std::endl;
    std::cout << "  Total violations: " << stats.total_violations << std::endl;

    assert(stats.total_peers == 1);
    assert(stats.total_messages_allowed == 5);
    assert(stats.total_messages_rejected == 1);
    assert(stats.total_violations == 1);

    std::cout << "✅ Statistics tracked correctly\n" << std::endl;

    std::cout << "✅ Test G.7.5 PASSED: Statistics tracking working\n" << std::endl;
}

// ============================================================================
// Test G.7.6: Enable/Disable Functionality
// ============================================================================

void test_g7_6_enable_disable() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.7.6: Enable/Disable" << std::endl;
    std::cout << "========================================\n" << std::endl;

    RateLimiterConfig config;
    config.max_tokens = 10;
    config.refill_rate = 1.0;
    config.ban_threshold = 3;
    config.enabled = false;  // DISABLED

    RateLimiter limiter(config, nullptr);

    peer_id_t peer = "peer_unlimited:8333";

    std::cout << "Rate limiting disabled - testing unlimited messages..." << std::endl;

    // Should allow unlimited messages
    for (int i = 0; i < 1000; i++) {
        assert(limiter.allowMessage(peer, MessageCost::BLOCK));
    }

    auto stats = limiter.getStats();
    std::cout << "Messages allowed: " << stats.total_messages_allowed << std::endl;
    std::cout << "Messages rejected: " << stats.total_messages_rejected << std::endl;

    // When disabled, statistics aren't tracked (returns true immediately)
    assert(stats.total_messages_allowed == 0);
    assert(stats.total_messages_rejected == 0);

    std::cout << "✅ Disabled rate limiter allows all messages (stats not tracked)\n" << std::endl;

    std::cout << "✅ Test G.7.6 PASSED: Enable/disable working\n" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase G.7: Rate Limiter Tests       ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

    try {
        test_g7_1_token_bucket();
        test_g7_2_message_costs();
        test_g7_3_violations_and_bans();
        test_g7_4_peer_removal();
        test_g7_5_statistics();
        test_g7_6_enable_disable();

        std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL TESTS PASSED                  ║" << std::endl;
        std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
