// SPDX-License-Identifier: MIT
// Dinero - Network Invariants
//
// Network invariant checks to ensure consistency and prevent eclipse attacks.
// These checks verify critical network state consistency and can be enabled in debug builds.

#pragma once

#include "p2p/connection_manager.h"
#include "p2p/outbound_policy.h"
#include <vector>
#include <string>
#include <cstdint>

class P2PManager;

namespace dinero {

namespace network {

/**
 * Network invariant violation
 */
struct InvariantViolation {
    std::string invariant_name;
    std::string description;
    std::string severity;  // "CRITICAL", "WARNING", "INFO"
};

/**
 * NetworkInvariants - Verify network consistency and prevent attacks
 *
 * INVARIANTS ENFORCED:
 * 1. Connection count consistency
 *    - Total peers in the active P2P manager matches connected peer snapshots
 *    - Inbound/outbound counts remain internally coherent
 *    - Total connections never exceed MAX_TOTAL
 *
 * 2. Eviction protection
 *    - At least MIN_OUTBOUND outbound peers protected
 *    - No duplicate peer registrations
 *    - All registered peers exist in peer map
 *
 * 3. Socket state consistency
 *    - No orphaned sockets (socket exists but peer not tracked)
 *    - No ghost peers (peer tracked but socket closed)
 *
 * 4. Eclipse attack prevention
 *    - Subnet diversity maintained (no more than N peers from same /16)
 *    - Connection slots not all filled by attacker
 *
 * Usage:
 *   NetworkInvariants checker(p2p_manager);
 *   auto violations = checker.checkAll();
 *   for (const auto& v : violations) {
 *       if (v.severity == "CRITICAL") {
 *           g_logger.error("INVARIANT VIOLATION: " + v.description);
 *       }
 *   }
 */
class NetworkInvariants {
public:
    /**
     * Constructor for the active daemon P2P path.
     * Uses P2PManager as the source of truth for live peer state.
     */
    explicit NetworkInvariants(const ::P2PManager* p2p_mgr);

    /**
     * Check all network invariants
     * @return Vector of violations (empty if all invariants hold)
     */
    std::vector<InvariantViolation> checkAll();

    /**
     * Check connection count consistency
     * Verifies that peer counts match between the active peer manager and snapshots
     */
    std::vector<InvariantViolation> checkConnectionCountConsistency();

    /**
     * Check connection limits enforcement
     * Verifies that total connections never exceed configured limits
     */
    std::vector<InvariantViolation> checkConnectionLimits();

    /**
     * Check eviction protection invariants
     * Verifies that outbound peers are protected from eviction
     */
    std::vector<InvariantViolation> checkEvictionProtection();

    /**
     * Check subnet diversity for eclipse attack prevention
     * Verifies that not too many peers come from the same subnet
     *
     * IMPLEMENTED: Groups peers by /16 subnet and checks for violations
     * Warning if any subnet has > MAX_PEERS_PER_SUBNET (32) peers
     */
    std::vector<InvariantViolation> checkSubnetDiversity();

    /**
     * Check for duplicate peer registrations
     * Verifies that each peer is registered exactly once
     *
     * IMPLEMENTED: Checks for duplicate peer_id (address:port) registrations
     * Critical violation if any peer appears multiple times
     */
    std::vector<InvariantViolation> checkNoDuplicatePeers();

    /**
     * Assert all invariants (aborts on violation)
     * Use this in debug builds to catch violations early
     */
    void assertAllInvariants();

private:
    struct PeerSnapshot {
        std::string address;
        uint16_t port{0};
        bool connected{false};
        bool outbound{false};
    };

    const ::P2PManager* p2p_mgr_;
    const ConnectionManager* conn_mgr_;

    // Thresholds for warnings
    static constexpr uint32_t MAX_PEERS_PER_SUBNET = 32;  // Max peers from same /16
    static constexpr uint32_t MIN_OUTBOUND_PROTECTED =
        static_cast<uint32_t>(dinero::p2p::kTargetDurableOutbound);

    /**
     * Helper: Extract /16 subnet from IP address
     * Example: "192.168.1.100" -> "192.168"
     */
    std::string extractSubnet16(const std::string& addr) const;

    std::vector<PeerSnapshot> getPeerSnapshots() const;
    uint32_t getManagedPeerCount() const;
};

} // namespace network
} // namespace dinero
