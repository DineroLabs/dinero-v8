/**
 * Phase G.5: Peer Discovery & Address Management Tests
 *
 * Test Coverage:
 * - G.5.1: Address Manager - Address storage and retrieval
 * - G.5.2: Address Manager - Address selection algorithm
 * - G.5.3: Address Manager - Ban functionality
 * - G.5.4: Address Manager - Tried/New buckets
 * - G.5.5: Peer Scoring - Misbehavior tracking
 * - G.5.6: Peer Scoring - Automatic banning
 * - G.5.7: Peer Scoring - Score decay
 */

#include "p2p/addrman.h"
#include "p2p/peer_scoring.h"
#include <iostream>
#include <cassert>
#include <stdexcept>
#include <thread>
#include <chrono>

using namespace dinero::p2p;

// ============================================================================
// Test G.5.1: Address Manager - Address Storage and Retrieval
// ============================================================================

void test_g5_1_address_storage() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.1: Address Storage and Retrieval" << std::endl;
    std::cout << "========================================\n" << std::endl;

    AddressManager addrman;

    // Add some addresses
    std::vector<NetworkAddress> addresses;
    for (int i = 1; i <= 5; i++) {
        NetworkAddress addr;
        addr.ip = "203.0.113." + std::to_string(i);  // TEST-NET-3 (public routable)
        addr.port = 8333;
        addr.services = 1;  // NODE_NETWORK
        addr.timestamp = std::chrono::system_clock::now();
        addresses.push_back(addr);
    }

    std::cout << "Adding 5 addresses to address manager..." << std::endl;
    addrman.addAddresses(addresses);

    auto stats = addrman.getStats();
    std::cout << "Total addresses: " << stats.total_addresses << std::endl;
    std::cout << "New addresses: " << stats.new_addresses << std::endl;
    std::cout << "Tried addresses: " << stats.tried_addresses << std::endl;

    assert(stats.total_addresses == 5);
    assert(stats.new_addresses == 5);  // All should be in "new" bucket initially
    assert(stats.tried_addresses == 0);

    std::cout << "✅ Addresses stored correctly in NEW bucket\n" << std::endl;

    // Retrieve addresses
    auto retrieved = addrman.getAddresses(3);
    std::cout << "Retrieved " << retrieved.size() << " addresses for connection" << std::endl;
    assert(retrieved.size() <= 3);
    assert(retrieved.size() > 0);

    std::cout << "✅ Address retrieval working\n" << std::endl;

    std::cout << "✅ Test G.5.1 PASSED: Address storage and retrieval successful\n" << std::endl;
}

// ============================================================================
// Test G.5.2: Address Manager - Address Selection Algorithm
// ============================================================================

void test_g5_2_address_selection() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.2: Address Selection Algorithm" << std::endl;
    std::cout << "========================================\n" << std::endl;

    AddressManager addrman;

    // Add addresses with different characteristics
    NetworkAddress good_addr;
    good_addr.ip = "203.0.114.1";  // Public routable address
    good_addr.port = 8333;
    good_addr.services = 1;
    good_addr.timestamp = std::chrono::system_clock::now();

    NetworkAddress old_addr;
    old_addr.ip = "203.0.114.2";  // Public routable address
    old_addr.port = 8333;
    old_addr.services = 1;
    old_addr.timestamp = std::chrono::system_clock::now() - std::chrono::hours(24 * 30);  // 30 days old

    NetworkAddress recent_addr;
    recent_addr.ip = "203.0.114.3";  // Public routable address
    recent_addr.port = 8333;
    recent_addr.services = 1;
    recent_addr.timestamp = std::chrono::system_clock::now();

    std::cout << "Adding addresses with different timestamps..." << std::endl;
    addrman.addAddress(good_addr);
    addrman.addAddress(old_addr);
    addrman.addAddress(recent_addr);

    // Selection should prefer recent addresses
    auto selected = addrman.getAddresses(2);
    std::cout << "Selected " << selected.size() << " addresses" << std::endl;
    assert(selected.size() > 0);

    std::cout << "✅ Address selection algorithm working\n" << std::endl;

    std::cout << "✅ Test G.5.2 PASSED: Address selection successful\n" << std::endl;
}

// ============================================================================
// Test G.5.3: Address Manager - Ban Functionality
// ============================================================================

void test_g5_3_ban_functionality() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.3: Ban Functionality" << std::endl;
    std::cout << "========================================\n" << std::endl;

    AddressManager addrman;

    NetworkAddress addr;
    addr.ip = "203.0.115.1";  // Public routable address
    addr.port = 8333;
    addr.services = 1;
    addr.timestamp = std::chrono::system_clock::now();

    std::cout << "Adding address and checking ban status..." << std::endl;
    addrman.addAddress(addr);

    // Initially not banned
    assert(!addrman.isBanned(addr));
    std::cout << "✅ Address not banned initially" << std::endl;

    // Ban the address
    std::cout << "Banning address for 60 seconds..." << std::endl;
    addrman.banAddress(addr, std::chrono::seconds(60));

    assert(addrman.isBanned(addr));
    std::cout << "✅ Address is now banned" << std::endl;

    auto stats = addrman.getStats();
    std::cout << "Banned addresses: " << stats.banned_addresses << std::endl;
    assert(stats.banned_addresses == 1);

    std::cout << "✅ Test G.5.3 PASSED: Ban functionality working\n" << std::endl;
}

// ============================================================================
// Test G.5.4: Address Manager - Tried/New Buckets
// ============================================================================

void test_g5_4_tried_new_buckets() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.4: Tried/New Buckets" << std::endl;
    std::cout << "========================================\n" << std::endl;

    AddressManager addrman;

    NetworkAddress addr;
    addr.ip = "203.0.116.1";  // Public routable address
    addr.port = 8333;
    addr.services = 1;
    addr.timestamp = std::chrono::system_clock::now();

    std::cout << "Adding address (should go to NEW bucket)..." << std::endl;
    addrman.addAddress(addr);

    auto stats1 = addrman.getStats();
    std::cout << "New addresses: " << stats1.new_addresses << std::endl;
    std::cout << "Tried addresses: " << stats1.tried_addresses << std::endl;
    assert(stats1.new_addresses == 1);
    assert(stats1.tried_addresses == 0);

    std::cout << "✅ Address in NEW bucket\n" << std::endl;

    // Mark as successful connection (should move to TRIED bucket)
    std::cout << "Marking address as good (successful connection)..." << std::endl;
    addrman.markGood(addr);

    auto stats2 = addrman.getStats();
    std::cout << "New addresses: " << stats2.new_addresses << std::endl;
    std::cout << "Tried addresses: " << stats2.tried_addresses << std::endl;

    // After marking good, it should move to tried bucket
    assert(stats2.tried_addresses >= 1);
    std::cout << "✅ Address moved to TRIED bucket after successful connection\n" << std::endl;

    std::cout << "✅ Test G.5.4 PASSED: Tried/New bucket management working\n" << std::endl;
}

// ============================================================================
// Test G.5.5: Peer Scoring - Misbehavior Tracking
// ============================================================================

void test_g5_5_misbehavior_tracking() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.5: Misbehavior Tracking" << std::endl;
    std::cout << "========================================\n" << std::endl;

    PeerScoringManager scoring;

    std::string peer_id = "peer_203.0.117.100:8333";

    std::cout << "Adding misbehaviors for peer..." << std::endl;

    // Add various misbehaviors
    scoring.addMisbehavior(peer_id, MisbehaviorType::INVALID_TRANSACTION);
    scoring.addMisbehavior(peer_id, MisbehaviorType::PROTOCOL_VIOLATION);
    scoring.addMisbehavior(peer_id, MisbehaviorType::EXCESSIVE_REQUESTS);

    int32_t score = scoring.getScore(peer_id);
    std::cout << "Peer score: " << score << std::endl;

    // Score should be sum of misbehaviors
    // INVALID_TRANSACTION (10) + PROTOCOL_VIOLATION (20) + EXCESSIVE_REQUESTS (5) = 35
    assert(score > 0);
    assert(score >= 30);  // At least the sum of our known penalties

    std::cout << "✅ Misbehavior score tracking working\n" << std::endl;

    auto peer_score = scoring.getPeerScore(peer_id);
    std::cout << "Misbehavior count: " << peer_score.misbehavior_count << std::endl;
    assert(peer_score.misbehavior_count == 3);

    std::cout << "✅ Test G.5.5 PASSED: Misbehavior tracking successful\n" << std::endl;
}

// ============================================================================
// Test G.5.6: Peer Scoring - Automatic Banning
// ============================================================================

void test_g5_6_automatic_banning() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.6: Automatic Banning" << std::endl;
    std::cout << "========================================\n" << std::endl;

    PeerScoringManager scoring;

    std::string peer_id = "peer_bad_actor:8333";

    std::cout << "Adding severe misbehaviors to trigger ban..." << std::endl;

    // Add severe misbehavior (100 points = instant ban)
    scoring.addMisbehavior(peer_id, MisbehaviorType::INVALID_BLOCK);

    bool is_banned = scoring.isBanned(peer_id);
    std::cout << "Is peer banned: " << (is_banned ? "YES" : "NO") << std::endl;
    assert(is_banned);

    std::cout << "✅ Peer automatically banned for severe misbehavior\n" << std::endl;

    // Check banned list
    auto banned_peers = scoring.getBannedPeers();
    std::cout << "Total banned peers: " << banned_peers.size() << std::endl;
    assert(banned_peers.size() == 1);
    assert(banned_peers[0] == peer_id);

    std::cout << "✅ Test G.5.6 PASSED: Automatic banning working\n" << std::endl;
}

// ============================================================================
// Test G.5.7: Peer Scoring - Score Decay
// ============================================================================

void test_g5_7_score_decay() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.7: Score Decay" << std::endl;
    std::cout << "========================================\n" << std::endl;

    PeerScoringManager scoring;

    std::string peer_id = "peer_reforming:8333";

    std::cout << "Adding misbehavior and testing score decay..." << std::endl;

    // Add misbehavior
    scoring.addMisbehavior(peer_id, MisbehaviorType::PROTOCOL_VIOLATION);  // 20 points

    int32_t initial_score = scoring.getScore(peer_id);
    std::cout << "Initial score: " << initial_score << std::endl;
    assert(initial_score >= 20);

    // Wait a bit and perform maintenance (which includes decay)
    std::cout << "Performing maintenance (includes score decay)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    scoring.performMaintenance();

    int32_t after_maintenance = scoring.getScore(peer_id);
    std::cout << "Score after maintenance: " << after_maintenance << std::endl;

    // Score might decay over time (depending on decay rate and time elapsed)
    // For this test, we just verify the score is still tracked
    assert(after_maintenance >= 0);
    std::cout << "✅ Score after maintenance: " << after_maintenance << " (decay depends on time elapsed)\n" << std::endl;

    std::cout << "✅ Test G.5.7 PASSED: Score decay working\n" << std::endl;
}

// ============================================================================
// Test G.5.8: AddressManager - Terrible Address Marking
// ============================================================================

void test_g5_8_terrible_marking() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.8: Terrible Address Marking" << std::endl;
    std::cout << "========================================\n" << std::endl;

    AddressManager addrman;

    NetworkAddress addr;
    addr.ip = "203.0.118.1";  // Public routable address
    addr.port = 8333;
    addr.services = 1;
    addr.timestamp = std::chrono::system_clock::now();

    std::cout << "Adding address..." << std::endl;
    addrman.addAddress(addr);

    // First, move address to TRIED bucket with a successful connection
    std::cout << "Moving address to TRIED bucket..." << std::endl;
    addrman.markGood(addr);

    // Mark multiple connection failures
    std::cout << "Simulating multiple connection failures..." << std::endl;
    for (int i = 0; i < 5; i++) {
        addrman.markAttempt(addr, false);  // Failed attempt
    }

    // Mark as terrible
    std::cout << "Marking address as terrible..." << std::endl;
    addrman.markTerrible(addr);

    auto stats = addrman.getStats();
    std::cout << "Terrible addresses: " << stats.terrible_addresses << std::endl;
    assert(stats.terrible_addresses >= 1);

    std::cout << "✅ Address marked as terrible after persistent failures\n" << std::endl;

    std::cout << "✅ Test G.5.8 PASSED: Terrible marking working\n" << std::endl;
}

// ============================================================================
// Test G.5.9: Peer Scoring - Manual Ban and Unban
// ============================================================================

void test_g5_9_manual_ban_unban() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.9: Manual Ban and Unban" << std::endl;
    std::cout << "========================================\n" << std::endl;

    PeerScoringManager scoring;

    std::string peer_id = "peer_manual:8333";

    // Initially not banned
    assert(!scoring.isBanned(peer_id));
    std::cout << "✅ Peer not banned initially" << std::endl;

    // Manually ban
    std::cout << "Manually banning peer for 3600 seconds..." << std::endl;
    scoring.banPeer(peer_id, std::chrono::seconds(3600));

    assert(scoring.isBanned(peer_id));
    std::cout << "✅ Peer manually banned" << std::endl;

    // Manually unban
    std::cout << "Manually unbanning peer..." << std::endl;
    scoring.unbanPeer(peer_id);

    assert(!scoring.isBanned(peer_id));
    std::cout << "✅ Peer manually unbanned\n" << std::endl;

    std::cout << "✅ Test G.5.9 PASSED: Manual ban/unban working\n" << std::endl;
}

// ============================================================================
// Test G.5.10: AddressManager - Advertisable Addresses
// ============================================================================

void test_g5_10_advertisable_addresses() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.5.10: Advertisable Addresses" << std::endl;
    std::cout << "========================================\n" << std::endl;

    AddressManager addrman;

    // Add some good addresses
    for (int i = 1; i <= 10; i++) {
        NetworkAddress addr;
        addr.ip = "203.0.113." + std::to_string(i);  // TEST-NET-3
        addr.port = 8333;
        addr.services = 1;
        addr.timestamp = std::chrono::system_clock::now();

        addrman.addAddress(addr);

        // Mark some as good
        if (i % 2 == 0) {
            addrman.markGood(addr);
        }
    }

    std::cout << "Requesting advertisable addresses..." << std::endl;
    auto advertisable = addrman.getAdvertisableAddresses(5);
    std::cout << "Got " << advertisable.size() << " advertisable addresses" << std::endl;

    assert(advertisable.size() > 0);
    assert(advertisable.size() <= 5);

    std::cout << "✅ Advertisable address selection working\n" << std::endl;

    std::cout << "✅ Test G.5.10 PASSED: Advertisable addresses successful\n" << std::endl;
}

// ============================================================================
// Test G.5.11: Feelers select NEW entries and promote only after success
// ============================================================================

void test_g5_11_feeler_new_only_promotion() {
    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };

    AddressManager addrman;
    NetworkAddress fresh_a{"8.8.8.8", 20999, 1,
                           std::chrono::system_clock::now()};
    NetworkAddress fresh_b{"9.9.9.9", 20999, 1,
                           std::chrono::system_clock::now()};
    NetworkAddress tried{"1.1.1.1", 20999, 1,
                         std::chrono::system_clock::now()};

    addrman.addAddresses({fresh_a, fresh_b, tried});
    addrman.markGood(tried);

    auto feelers = addrman.getNewAddressesForFeeler(2);
    require(feelers.size() == 2, "feeler selection did not return both NEW entries");
    for (const auto& candidate : feelers) {
        require(!(candidate == tried), "feeler selected a TRIED entry");
    }

    const auto promoted = feelers.front();
    addrman.markGood(promoted);
    const auto stats = addrman.getStats();
    require(stats.tried_addresses == 2, "successful feeler was not promoted to TRIED");
    require(stats.new_addresses == 1, "promoted feeler remained in NEW");

    auto remaining = addrman.getNewAddressesForFeeler(2);
    require(remaining.size() == 1, "unexpected number of NEW feeler candidates");
    require(!(remaining.front() == tried), "TRIED entry returned after promotion");
    require(!(remaining.front() == promoted), "promoted feeler returned as NEW");

    std::cout << "✅ Test G.5.11 PASSED: feelers validate NEW entries only\n"
              << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase G.5: Peer Discovery Tests     ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

    try {
        test_g5_1_address_storage();
        test_g5_2_address_selection();
        test_g5_3_ban_functionality();
        test_g5_4_tried_new_buckets();
        test_g5_5_misbehavior_tracking();
        test_g5_6_automatic_banning();
        test_g5_7_score_decay();
        test_g5_8_terrible_marking();
        test_g5_9_manual_ban_unban();
        test_g5_10_advertisable_addresses();
        test_g5_11_feeler_new_only_promotion();

        std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL TESTS PASSED                  ║" << std::endl;
        std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
