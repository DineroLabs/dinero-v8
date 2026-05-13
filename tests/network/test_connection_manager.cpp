// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "p2p/connection_manager.h"
#include "p2p/peer_scoring.h"
#include <cassert>
#include <iostream>
#include <memory>

using namespace dinero;

void test_connection_limits() {
    std::cout << "Testing connection limits..." << std::endl;

    ConnectionLimits limits;
    limits.max_inbound = 10;
    limits.max_outbound = 5;
    limits.max_total = 15;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    ConnectionManager mgr(limits, scoring);

    // Test outbound limit
    for (int i = 0; i < 5; ++i) {
        assert(mgr.shouldAcceptOutbound(false));
        mgr.registerPeer("out_" + std::to_string(i), PeerConnectionType::OUTBOUND_FULL, "192.168.1." + std::to_string(i));
    }

    // Should reject 6th outbound
    assert(!mgr.shouldAcceptOutbound(false));

    // Test inbound limit (each from different /16 subnet to avoid eclipse limits)
    for (int i = 0; i < 10; ++i) {
        std::string ip = std::to_string(10 + i) + ".0.0.1";
        auto result = mgr.shouldAcceptInbound(ip);
        assert(result.accept);
        mgr.registerPeer("in_" + std::to_string(i), PeerConnectionType::INBOUND, ip);
    }

    assert(mgr.getInboundCount() == 10);
    assert(mgr.getOutboundCount() == 5);
    assert(mgr.getTotalCount() == 15);

    std::cout << "✓ Connection limits test passed" << std::endl;
}

void test_eviction_protection() {
    std::cout << "Testing eviction protection..." << std::endl;

    ConnectionLimits limits;
    limits.max_inbound = 5;
    limits.max_outbound = 5;
    limits.max_total = 10;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    ConnectionManager mgr(limits, scoring);

    // Register 5 inbound peers
    mgr.registerPeer("peer1", PeerConnectionType::INBOUND, "192.168.1.1");
    mgr.registerPeer("peer2", PeerConnectionType::INBOUND, "192.168.1.2");
    mgr.registerPeer("peer3", PeerConnectionType::INBOUND, "192.168.1.3");
    mgr.registerPeer("peer4", PeerConnectionType::INBOUND, "192.168.1.4");
    mgr.registerPeer("peer5", PeerConnectionType::INBOUND, "192.168.1.5");

    assert(mgr.getInboundCount() == 5);

    // Set peer3 to high misbehavior score
    for (int i = 0; i < 4; i++) {
        scoring->addMisbehavior("peer3", p2p::MisbehaviorType::PROTOCOL_VIOLATION);
    }

    // Verify misbehavior score was recorded
    assert(scoring->getScore("peer3") == 80);

    // Peer4 served recent block (should be protected)
    mgr.markRecentService("peer4", true);

    // Try to accept 6th inbound - all peers protected by recent connection window
    auto result = mgr.shouldAcceptInbound("10.0.0.6");

    // Due to recent connection protection (< 120s), eviction should fail
    assert(!result.accept);
    assert(!result.requires_eviction);

    std::cout << "✓ Eviction protection test passed" << std::endl;
}

void test_peer_counts() {
    std::cout << "Testing peer counting..." << std::endl;

    ConnectionLimits limits;
    limits.max_inbound = 10;
    limits.max_outbound = 10;
    limits.max_total = 20;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    ConnectionManager mgr(limits, scoring);

    // Register mixed peer types
    mgr.registerPeer("in1", PeerConnectionType::INBOUND, "192.168.1.1");
    mgr.registerPeer("out1", PeerConnectionType::OUTBOUND_FULL, "192.168.1.2");
    mgr.registerPeer("in2", PeerConnectionType::INBOUND, "192.168.1.3");
    mgr.registerPeer("blocks1", PeerConnectionType::OUTBOUND_BLOCKS, "192.168.1.4");

    assert(mgr.getInboundCount() == 2);
    assert(mgr.getOutboundCount() == 1);
    assert(mgr.getTotalCount() == 4);

    // Unregister a peer
    mgr.unregisterPeer("in1");

    assert(mgr.getInboundCount() == 1);
    assert(mgr.getOutboundCount() == 1);
    assert(mgr.getTotalCount() == 3);

    std::cout << "✓ Peer counting test passed" << std::endl;
}

void test_outbound_never_evicted() {
    std::cout << "Testing outbound peers never evicted..." << std::endl;

    ConnectionLimits limits;
    limits.max_inbound = 2;
    limits.max_outbound = 2;
    limits.max_total = 4;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    ConnectionManager mgr(limits, scoring);

    // Register 2 outbound + 2 inbound
    mgr.registerPeer("out1", PeerConnectionType::OUTBOUND_FULL, "192.168.1.1");
    mgr.registerPeer("out2", PeerConnectionType::OUTBOUND_FULL, "192.168.1.2");
    mgr.registerPeer("in1", PeerConnectionType::INBOUND, "192.168.1.3");
    mgr.registerPeer("in2", PeerConnectionType::INBOUND, "192.168.1.4");

    assert(mgr.getTotalCount() == 4);

    // Try to accept 5th peer (would exceed total limit)
    auto result = mgr.shouldAcceptInbound("10.0.0.1");

    // Total limit reached, should reject
    assert(!result.accept);

    std::cout << "✓ Outbound protection test passed" << std::endl;
}

void test_per_ip_inbound_limit() {
    std::cout << "Testing per-IP inbound limit..." << std::endl;

    ConnectionLimits limits;
    limits.max_inbound = 20;
    limits.max_outbound = 5;
    limits.max_total = 25;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    ConnectionManager mgr(limits, scoring);

    // Accept 2 inbound from same IP (MAX_INBOUND_PER_IP = 2)
    auto r1 = mgr.shouldAcceptInbound("10.0.0.1");
    assert(r1.accept);
    mgr.registerPeer("dup1", PeerConnectionType::INBOUND, "10.0.0.1");

    auto r2 = mgr.shouldAcceptInbound("10.0.0.1");
    assert(r2.accept);
    mgr.registerPeer("dup2", PeerConnectionType::INBOUND, "10.0.0.1");

    // 3rd from same IP should be rejected
    auto r3 = mgr.shouldAcceptInbound("10.0.0.1");
    assert(!r3.accept);
    assert(r3.reason.find("Per-IP") != std::string::npos);

    // Different IP should still be accepted
    auto r4 = mgr.shouldAcceptInbound("10.0.0.2");
    assert(r4.accept);

    std::cout << "✓ Per-IP inbound limit test passed" << std::endl;
}

void test_per_subnet_inbound_limit() {
    std::cout << "Testing per-/16-subnet inbound limit..." << std::endl;

    ConnectionLimits limits;
    limits.max_inbound = 20;
    limits.max_outbound = 5;
    limits.max_total = 25;

    auto scoring = std::make_shared<p2p::PeerScoringManager>();
    ConnectionManager mgr(limits, scoring);

    // Accept 4 inbound from same /16 (MAX_INBOUND_PER_SUBNET16 = 4)
    // All from 10.0.x.x (subnet "10.0")
    for (int i = 0; i < 4; ++i) {
        std::string ip = "10.0." + std::to_string(i) + ".1";
        auto r = mgr.shouldAcceptInbound(ip);
        assert(r.accept);
        mgr.registerPeer("sub_" + std::to_string(i), PeerConnectionType::INBOUND, ip);
    }

    // 5th from same /16 should be rejected
    auto r5 = mgr.shouldAcceptInbound("10.0.4.1");
    assert(!r5.accept);
    assert(r5.reason.find("Per-subnet") != std::string::npos);

    // Different /16 should still be accepted
    auto r6 = mgr.shouldAcceptInbound("10.1.0.1");
    assert(r6.accept);

    std::cout << "✓ Per-/16-subnet inbound limit test passed" << std::endl;
}

int main() {
    std::cout << "Running ConnectionManager tests..." << std::endl;

    try {
        test_connection_limits();
        test_eviction_protection();
        test_peer_counts();
        test_outbound_never_evicted();
        test_per_ip_inbound_limit();
        test_per_subnet_inbound_limit();

        std::cout << "\n✅ All ConnectionManager tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
