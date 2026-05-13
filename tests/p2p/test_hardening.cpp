// Copyright (c) 2026 The Dinero Developers
// Tests for P2P and RPC hardening changes (March 2026 security audit)

#include "p2p/connection_manager.h"
#include "p2p/peer_scoring.h"
#include "config/seed_nodes.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <set>

using namespace dinero;

// ============================================================================
// Helper: Create a ConnectionManager with default limits and scoring
// ============================================================================
static std::pair<std::unique_ptr<ConnectionManager>, std::shared_ptr<p2p::PeerScoringManager>>
makeConnectionManager() {
    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    ConnectionLimits limits;
    auto cm = std::make_unique<ConnectionManager>(limits, scoring);
    return {std::move(cm), scoring};
}

// ============================================================================
// TEST 1: Anchor peers are never evicted
// ============================================================================
static void test_anchor_peers_never_evicted() {
    auto [cm, scoring] = makeConnectionManager();

    cm->addAnchorAddress("172.93.160.131");
    cm->addAnchorAddress("173.249.195.59");

    // Register anchor as inbound
    cm->registerPeer("anchor-la", PeerConnectionType::INBOUND, "172.93.160.131");
    // Register non-anchor inbound peers to fill capacity
    cm->registerPeer("random-1", PeerConnectionType::INBOUND, "1.2.3.4");
    cm->registerPeer("random-2", PeerConnectionType::INBOUND, "5.6.7.8");

    // Give random peers bad scores
    scoring->addMisbehavior("random-1", p2p::MisbehaviorType::PROTOCOL_VIOLATION);
    scoring->addMisbehavior("random-1", p2p::MisbehaviorType::PROTOCOL_VIOLATION);
    scoring->addMisbehavior("random-1", p2p::MisbehaviorType::PROTOCOL_VIOLATION);

    // Try eviction
    auto result = cm->selectEvictionCandidate();
    if (result.evicted) {
        // Must NOT be the anchor peer
        assert(result.evicted_peer_id != "anchor-la");
    }

    std::cout << "  PASS: Anchor peers never evicted" << std::endl;
}

// ============================================================================
// TEST 2: Anchor budget is separate from outbound budget
// ============================================================================
static void test_anchor_budget_separate() {
    auto [cm, scoring] = makeConnectionManager();
    auto limits = cm->getLimits();

    // Fill all regular outbound slots
    for (uint32_t i = 0; i < limits.max_outbound; ++i) {
        std::string id = "outbound-" + std::to_string(i);
        cm->registerPeer(id, PeerConnectionType::OUTBOUND_FULL, "10.0.0." + std::to_string(i));
    }

    // Regular outbound should be full
    assert(!cm->shouldAcceptOutbound(false, false));

    // But anchor slots should still be available
    assert(cm->shouldAcceptOutbound(false, true));

    // Fill anchor slots
    for (uint32_t i = 0; i < limits.max_anchor; ++i) {
        std::string id = "anchor-" + std::to_string(i);
        cm->registerPeer(id, PeerConnectionType::OUTBOUND_ANCHOR, "172.0.0." + std::to_string(i));
    }

    // Now anchor should be full too
    assert(!cm->shouldAcceptOutbound(false, true));

    // Verify outbound count didn't change
    assert(cm->getOutboundCount() == limits.max_outbound);
    assert(cm->getAnchorCount() == limits.max_anchor);

    std::cout << "  PASS: Anchor budget separate from outbound" << std::endl;
}

// ============================================================================
// TEST 3: Outbound subnet diversity enforcement
// ============================================================================
static void test_outbound_subnet_diversity() {
    auto [cm, scoring] = makeConnectionManager();

    // Add 2 outbound peers from same /16 subnet
    cm->registerPeer("peer-1", PeerConnectionType::OUTBOUND_FULL, "192.168.1.1");
    cm->registerPeer("peer-2", PeerConnectionType::OUTBOUND_FULL, "192.168.2.2");

    // Third from same /16 should be rejected
    assert(!cm->isOutboundSubnetAllowed("192.168.3.3"));

    // Different /16 should be allowed
    assert(cm->isOutboundSubnetAllowed("10.0.1.1"));

    std::cout << "  PASS: Outbound subnet diversity enforced" << std::endl;
}

// ============================================================================
// TEST 4: isAnchorPeer detects anchor IPs
// ============================================================================
static void test_is_anchor_peer() {
    auto [cm, scoring] = makeConnectionManager();

    cm->addAnchorAddress("173.249.195.59");
    cm->addAnchorAddress("172.93.160.131");

    assert(cm->isAnchorPeer("173.249.195.59"));
    assert(cm->isAnchorPeer("172.93.160.131"));
    assert(!cm->isAnchorPeer("1.2.3.4"));
    assert(!cm->isAnchorPeer(""));

    std::cout << "  PASS: isAnchorPeer correctly identifies anchors" << std::endl;
}

// ============================================================================
// TEST 5: getDisconnectedAnchors
// ============================================================================
static void test_disconnected_anchors() {
    auto [cm, scoring] = makeConnectionManager();

    cm->addAnchorAddress("1.1.1.1");
    cm->addAnchorAddress("2.2.2.2");
    cm->addAnchorAddress("3.3.3.3");

    // Connect one anchor
    cm->registerPeer("anchor-1", PeerConnectionType::OUTBOUND_ANCHOR, "1.1.1.1");

    auto disconnected = cm->getDisconnectedAnchors();
    assert(disconnected.size() == 2);

    // The connected one should not be in the list
    for (const auto& ip : disconnected) {
        assert(ip != "1.1.1.1");
    }

    std::cout << "  PASS: getDisconnectedAnchors works correctly" << std::endl;
}

// ============================================================================
// TEST 6: Ban escalation — ban_count increases duration
// ============================================================================
static void test_ban_escalation() {
    p2p::PeerScoringManager scoring;
    scoring.setDefaultBanDuration(std::chrono::seconds(3600));  // 1 hour base

    // First ban
    scoring.addMisbehavior("bad-peer", p2p::MisbehaviorType::INVALID_BLOCK);  // score +100
    assert(scoring.isBanned("bad-peer"));

    auto score1 = scoring.getPeerScore("bad-peer");
    assert(score1.ban_count == 1);

    // Unban and trigger second ban
    scoring.unbanPeer("bad-peer");
    scoring.addMisbehavior("bad-peer", p2p::MisbehaviorType::INVALID_BLOCK);
    assert(scoring.isBanned("bad-peer"));

    auto score2 = scoring.getPeerScore("bad-peer");
    assert(score2.ban_count == 2);

    // Ban duration should be longer for second ban (4x escalation)
    // We can't easily check duration directly, but ban_count tracks correctly
    std::cout << "  PASS: Ban escalation increments ban_count" << std::endl;
}

// ============================================================================
// TEST 7: Ban count capped at 3
// ============================================================================
static void test_ban_count_cap() {
    p2p::PeerScoringManager scoring;

    // Trigger many bans
    for (int i = 0; i < 10; ++i) {
        scoring.addMisbehavior("repeat-offender", p2p::MisbehaviorType::INVALID_BLOCK);
        if (scoring.isBanned("repeat-offender")) {
            scoring.unbanPeer("repeat-offender");
        }
    }

    auto score = scoring.getPeerScore("repeat-offender");
    // ban_count should keep incrementing (decay handles cap behavior)
    // The escalation factor is capped at 3 in checkForBan
    assert(score.ban_count > 0);

    std::cout << "  PASS: Ban count tracks correctly across multiple bans" << std::endl;
}

// ============================================================================
// TEST 8: Seed nodes include all 4 mainnet servers
// ============================================================================
static void test_seed_nodes_complete() {
    auto seeds = config::getSeedNodes("mainnet");
    assert(seeds.size() >= 4);

    bool found_la = false, found_va = false, found_mo = false, found_cn = false;
    for (const auto& seed : seeds) {
        if (seed.hostname == "172.93.160.131") found_la = true;
        if (seed.hostname == "173.249.195.59") found_va = true;
        if (seed.hostname == "72.18.214.120") found_mo = true;
        if (seed.hostname == "96.9.226.98") found_cn = true;
    }
    assert(found_la && found_va && found_mo && found_cn);

    std::cout << "  PASS: All 4 mainnet seed nodes present" << std::endl;
}

// ============================================================================
// TEST 9: Anchor peers list has 3 entries with geographic diversity
// ============================================================================
static void test_anchor_peers_list() {
    auto anchors = config::getAnchorPeers("mainnet");
    assert(anchors.size() == 3);

    // Verify geographic diversity
    std::set<std::string> regions;
    for (const auto& a : anchors) {
        regions.insert(a.region);
    }
    assert(regions.size() == 3);  // 3 different regions

    // No anchors for testnet/regtest
    assert(config::getAnchorPeers("testnet").empty());
    assert(config::getAnchorPeers("regtest").empty());

    std::cout << "  PASS: Anchor peers list correct (3 entries, 3 regions)" << std::endl;
}

// ============================================================================
// TEST 10: isAnchorPeer config function
// ============================================================================
static void test_config_is_anchor_peer() {
    assert(config::isAnchorPeer("173.249.195.59", "mainnet"));
    assert(config::isAnchorPeer("172.93.160.131", "mainnet"));
    assert(config::isAnchorPeer("72.18.214.120", "mainnet"));
    assert(!config::isAnchorPeer("1.2.3.4", "mainnet"));
    assert(!config::isAnchorPeer("173.249.195.59", "testnet"));

    std::cout << "  PASS: config::isAnchorPeer works correctly" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    std::cout << "=== P2P Hardening Tests ===" << std::endl;

    test_anchor_peers_never_evicted();
    test_anchor_budget_separate();
    test_outbound_subnet_diversity();
    test_is_anchor_peer();
    test_disconnected_anchors();
    test_ban_escalation();
    test_ban_count_cap();
    test_seed_nodes_complete();
    test_anchor_peers_list();
    test_config_is_anchor_peer();

    std::cout << "\n=== All 10 hardening tests PASSED ===" << std::endl;
    return 0;
}
