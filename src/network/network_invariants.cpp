// SPDX-License-Identifier: MIT
// Dinero - Network Invariants Implementation

#include "dinero/network/network_invariants.h"
#include "daemon/p2p_manager.h"
#include "p2p/connection_manager.h"
#include "common/logger.h"
#include <cassert>
#include <algorithm>
#include <unordered_map>

namespace dinero {
namespace network {

NetworkInvariants::NetworkInvariants(const ::P2PManager* p2p_mgr)
    : p2p_mgr_(p2p_mgr), conn_mgr_(nullptr) {
    assert(p2p_mgr_ != nullptr && "P2PManager cannot be null");
}

std::vector<InvariantViolation> NetworkInvariants::checkAll() {
    std::vector<InvariantViolation> violations;

    // Check all invariants and collect violations
    auto count_violations = checkConnectionCountConsistency();
    violations.insert(violations.end(), count_violations.begin(), count_violations.end());

    auto limit_violations = checkConnectionLimits();
    violations.insert(violations.end(), limit_violations.begin(), limit_violations.end());

    auto eviction_violations = checkEvictionProtection();
    violations.insert(violations.end(), eviction_violations.begin(), eviction_violations.end());

    auto subnet_violations = checkSubnetDiversity();
    violations.insert(violations.end(), subnet_violations.begin(), subnet_violations.end());

    auto duplicate_violations = checkNoDuplicatePeers();
    violations.insert(violations.end(), duplicate_violations.begin(), duplicate_violations.end());

    return violations;
}

std::vector<InvariantViolation> NetworkInvariants::checkConnectionCountConsistency() {
    std::vector<InvariantViolation> violations;
    const auto peer_list = getPeerSnapshots();
    const uint32_t managed_count = getManagedPeerCount();
    const uint32_t connected_count = static_cast<uint32_t>(
        std::count_if(peer_list.begin(), peer_list.end(),
                      [](const PeerSnapshot& peer) { return peer.connected; }));

    if (managed_count != connected_count) {
        violations.push_back({
            "connection_count_consistency",
            "Peer manager count (" + std::to_string(managed_count) +
            ") != connected snapshot count (" + std::to_string(connected_count) + ")",
            "CRITICAL"
        });
    }

    if (conn_mgr_) {
        const uint32_t conn_mgr_count = conn_mgr_->getTotalCount();
        if (managed_count != conn_mgr_count) {
            violations.push_back({
                "connection_manager_mismatch",
                "Peer manager count (" + std::to_string(managed_count) +
                ") != ConnectionManager count (" + std::to_string(conn_mgr_count) + ")",
                "CRITICAL"
            });
        }
    }

    return violations;
}

std::vector<InvariantViolation> NetworkInvariants::checkConnectionLimits() {
    std::vector<InvariantViolation> violations;

    if (!conn_mgr_) {
        return violations;
    }

    // INVARIANT: Total connections never exceed MAX_TOTAL
    const auto& limits = conn_mgr_->getLimits();
    uint32_t total = conn_mgr_->getTotalCount();
    uint32_t inbound = conn_mgr_->getInboundCount();
    uint32_t outbound = conn_mgr_->getOutboundCount();
    uint32_t blocks_only = conn_mgr_->getBlocksOnlyCount();

    if (total > limits.max_total) {
        violations.push_back({
            "max_total_exceeded",
            "Total connections (" + std::to_string(total) +
            ") exceeds MAX_TOTAL (" + std::to_string(limits.max_total) + ")",
            "CRITICAL"
        });
    }

    if (inbound > limits.max_inbound) {
        violations.push_back({
            "max_inbound_exceeded",
            "Inbound connections (" + std::to_string(inbound) +
            ") exceeds MAX_INBOUND (" + std::to_string(limits.max_inbound) + ")",
            "CRITICAL"
        });
    }

    if (outbound > limits.max_outbound) {
        violations.push_back({
            "max_outbound_exceeded",
            "Outbound connections (" + std::to_string(outbound) +
            ") exceeds MAX_OUTBOUND (" + std::to_string(limits.max_outbound) + ")",
            "CRITICAL"
        });
    }

    if (blocks_only > limits.max_blocks_only) {
        violations.push_back({
            "max_blocks_only_exceeded",
            "Blocks-only connections (" + std::to_string(blocks_only) +
            ") exceeds MAX_BLOCKS_ONLY (" + std::to_string(limits.max_blocks_only) + ")",
            "CRITICAL"
        });
    }

    // INVARIANT: Total == inbound + outbound + blocks_only
    if (total != inbound + outbound + blocks_only) {
        violations.push_back({
            "connection_count_mismatch",
            "Total (" + std::to_string(total) + ") != inbound (" +
            std::to_string(inbound) + ") + outbound (" + std::to_string(outbound) +
            ") + blocks_only (" + std::to_string(blocks_only) + ")",
            "CRITICAL"
        });
    }

    return violations;
}

std::vector<InvariantViolation> NetworkInvariants::checkEvictionProtection() {
    std::vector<InvariantViolation> violations;

    uint32_t total_outbound = 0;
    if (conn_mgr_) {
        // Legacy path: include blocks-only outbound peers.
        total_outbound = conn_mgr_->getOutboundCount() + conn_mgr_->getBlocksOnlyCount();
    } else {
        const auto peer_list = getPeerSnapshots();
        total_outbound = static_cast<uint32_t>(
            std::count_if(peer_list.begin(), peer_list.end(),
                          [](const PeerSnapshot& peer) {
                              return peer.connected && peer.outbound;
                          }));
    }

    if (total_outbound < MIN_OUTBOUND_PROTECTED) {
        violations.push_back({
            "insufficient_outbound_peers",
            "Total outbound peers (" + std::to_string(total_outbound) +
            ") < MIN_OUTBOUND_PROTECTED (" + std::to_string(MIN_OUTBOUND_PROTECTED) + ")",
            "WARNING"
        });
    }

    return violations;
}

std::vector<InvariantViolation> NetworkInvariants::checkSubnetDiversity() {
    std::vector<InvariantViolation> violations;

    // INVARIANT: No more than MAX_PEERS_PER_SUBNET peers from same /16 subnet
    // This prevents eclipse attacks where attacker controls many IPs in same subnet

    // Get all connected peers
    const auto peer_list = getPeerSnapshots();

    // Group peers by /16 subnet
    std::unordered_map<std::string, std::vector<std::string>> subnet_to_peers;

    for (const auto& peer : peer_list) {
        if (!peer.connected) continue;

        // Extract /16 subnet from IP address
        std::string subnet = extractSubnet16(peer.address);

        // Create peer identifier
        std::string peer_id = peer.address + ":" + std::to_string(peer.port);

        subnet_to_peers[subnet].push_back(peer_id);
    }

    // Check each subnet for excessive peer count
    for (const auto& [subnet, peer_ids] : subnet_to_peers) {
        if (peer_ids.size() > MAX_PEERS_PER_SUBNET) {
            std::string peer_list_str;
            for (size_t i = 0; i < std::min(peer_ids.size(), size_t(5)); ++i) {
                if (i > 0) peer_list_str += ", ";
                peer_list_str += peer_ids[i];
            }
            if (peer_ids.size() > 5) {
                peer_list_str += "... (+" + std::to_string(peer_ids.size() - 5) + " more)";
            }

            violations.push_back({
                "subnet_diversity_violation",
                "Subnet " + subnet + " has " + std::to_string(peer_ids.size()) +
                " peers (max: " + std::to_string(MAX_PEERS_PER_SUBNET) +
                "). Peers: " + peer_list_str + ". Possible eclipse attack!",
                "WARNING"
            });
        }
    }

    return violations;
}

std::string NetworkInvariants::extractSubnet16(const std::string& addr) const {
    // Extract /16 subnet from IP address
    // Example: "192.168.1.100" -> "192.168"

    size_t first_dot = addr.find('.');
    if (first_dot == std::string::npos) {
        return addr;  // Invalid IP, return as-is
    }

    size_t second_dot = addr.find('.', first_dot + 1);
    if (second_dot == std::string::npos) {
        return addr;  // Invalid IP, return as-is
    }

    return addr.substr(0, second_dot);
}

std::vector<InvariantViolation> NetworkInvariants::checkNoDuplicatePeers() {
    std::vector<InvariantViolation> violations;

    // INVARIANT: Each peer_id (address:port) appears exactly once in peer registry
    // No duplicate registrations allowed

    // Get all connected peers
    const auto peer_list = getPeerSnapshots();

    // Track peer IDs and check for duplicates
    std::unordered_map<std::string, int> peer_id_count;

    for (const auto& peer : peer_list) {
        // Create peer identifier from address:port
        std::string peer_id = peer.address + ":" + std::to_string(peer.port);
        peer_id_count[peer_id]++;
    }

    // Check for any duplicates
    for (const auto& [peer_id, count] : peer_id_count) {
        if (count > 1) {
            violations.push_back({
                "duplicate_peer_registration",
                "Peer " + peer_id + " is registered " + std::to_string(count) +
                " times. Expected exactly 1 registration per peer.",
                "CRITICAL"
            });
        }
    }

    // Additional check: Verify peer manager accounting matches unique peer IDs
    uint32_t network_mgr_count = getManagedPeerCount();
    uint32_t unique_peer_count = static_cast<uint32_t>(peer_id_count.size());

    if (network_mgr_count != unique_peer_count) {
        violations.push_back({
            "peer_count_unique_mismatch",
            "Peer manager reports " + std::to_string(network_mgr_count) +
            " peers but found " + std::to_string(unique_peer_count) +
            " unique peer IDs. Indicates duplicate or missing registrations.",
            "CRITICAL"
        });
    }

    return violations;
}

std::vector<NetworkInvariants::PeerSnapshot> NetworkInvariants::getPeerSnapshots() const {
    std::vector<PeerSnapshot> snapshots;
    if (p2p_mgr_) {
        const auto peers = p2p_mgr_->get_connected_peers();
        snapshots.reserve(peers.size());
        for (const auto& peer : peers) {
            snapshots.push_back(PeerSnapshot{
                peer.address,
                peer.port,
                peer.is_connected,
                peer.is_outbound
            });
        }
        return snapshots;
    }
    return snapshots;
}

uint32_t NetworkInvariants::getManagedPeerCount() const {
    if (p2p_mgr_) {
        return static_cast<uint32_t>(p2p_mgr_->get_peer_count());
    }
    return 0;
}

void NetworkInvariants::assertAllInvariants() {
    auto violations = checkAll();

    if (!violations.empty()) {
        g_logger.error("=== NETWORK INVARIANT VIOLATIONS DETECTED ===");

        for (const auto& v : violations) {
            g_logger.error("[" + v.severity + "] " + v.invariant_name + ": " + v.description);
        }

        // In debug builds, abort on critical violations
        #ifdef DEBUG
        for (const auto& v : violations) {
            if (v.severity == "CRITICAL") {
                assert(false && "Critical network invariant violation detected");
            }
        }
        #endif
    }
}

} // namespace network
} // namespace dinero
