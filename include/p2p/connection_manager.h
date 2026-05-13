// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "p2p/peer_scoring.h"
#include "network/types.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>

namespace dinero {

/**
 * Connection limits configuration
 * Based on Bitcoin Core defaults with conservative values for Phase N
 */
struct ConnectionLimits {
    uint32_t max_inbound{115};     // Maximum inbound peer connections
    uint32_t max_outbound{10};     // Maximum outbound peer connections (excluding blocks-only and anchors)
    uint32_t max_blocks_only{8};   // Maximum blocks-only outbound connections
    uint32_t max_anchor{3};        // Reserved anchor peer slots (separate budget, never reduces outbound)
    uint32_t max_total{128};       // Hard cap on total connections (125 + 3 anchor)
};

/**
 * Peer connection type classification
 * Used for eviction protection and connection accounting
 */
enum class PeerConnectionType {
    INBOUND,            // Incoming connection from remote peer
    OUTBOUND_FULL,      // Outgoing full-relay connection (blocks + transactions)
    OUTBOUND_BLOCKS,    // Outgoing blocks-only connection (no tx relay)
    OUTBOUND_ANCHOR     // Reserved anchor peer connection (never evicted, auto-reconnect)
};

/**
 * Peer connection info for eviction decisions
 * Lightweight snapshot of peer state for eviction algorithm
 */
struct PeerConnectionInfo {
    peer_id_t peer_id;
    PeerConnectionType connection_type;
    int64_t connection_time;        // Unix timestamp when connection established
    int64_t last_recv_time;         // Last message received timestamp
    int64_t last_send_time;         // Last message sent timestamp
    int32_t current_score;          // Current misbehavior score
    std::string addr;               // IP address (for subnet grouping)
    bool served_recent_block{false}; // Sent us a block in last hour
    bool served_recent_tx{false};    // Sent us a transaction in last hour
};

/**
 * Result of eviction attempt
 */
struct EvictionResult {
    bool evicted{false};
    peer_id_t evicted_peer_id{0};
    std::string reason;
};

/**
 * ConnectionManager — Enforces connection limits and manages peer eviction
 *
 * AUTHORITY: Connection lifecycle decisions (when to accept, when to evict)
 * DOES NOT: Decide what to download, validate messages, track misbehavior scores
 *
 * Responsibilities:
 * 1. Track inbound/outbound peer counts
 * 2. Enforce MAX_INBOUND, MAX_OUTBOUND, MAX_TOTAL limits
 * 3. Trigger eviction when inbound connections exceed capacity
 * 4. Implement eviction protection for high-value peers
 * 5. Prevent eclipse attacks via diversity enforcement
 *
 * Phase N: Eviction Policy Status
 * ✅ IMPLEMENTED: ConnectionManager eviction policy is properly wired
 * ✅ Active P2P peer manager calls shouldAcceptInbound() and evicts peers
 * ✅ Eclipse attack prevention active on the P2PService/P2PManager path
 *
 * Bitcoin Core Eviction Algorithm (implemented here):
 * 1. Protect outbound peers (never evict)
 * 2. Protect peers that sent recent blocks/txs
 * 3. Protect peers with lowest misbehavior scores
 * 4. Protect recent connections (last 2 minutes)
 * 5. Protect peers from diverse /16 subnets
 * 6. Evict lowest score among remaining, oldest connection as tie-breaker
 */
class ConnectionManager {
public:
    /**
     * Constructor
     * @param limits Connection limit configuration
     * @param scoring_manager Peer scoring system (for eviction decisions)
     */
    explicit ConnectionManager(
        const ConnectionLimits& limits,
        std::shared_ptr<p2p::PeerScoringManager> scoring_manager
    );

    /**
     * Result of inbound connection attempt
     */
    struct InboundAcceptResult {
        bool accept{false};              // Whether to accept the new connection
        bool requires_eviction{false};   // Whether eviction is needed
        peer_id_t evicted_peer_id;       // Peer to evict (if requires_eviction=true)
        std::string reason;              // Reason for eviction or rejection
    };

    /**
     * Check if a new inbound connection should be accepted
     * If at capacity, attempts to evict an existing peer
     *
     * Eclipse attack prevention: Enforces per-IP and per-/16-subnet limits
     * before checking global capacity. This prevents a single attacker from
     * filling all inbound slots from one network range.
     *
     * @param source_addr IP address of the incoming connection
     * @return InboundAcceptResult with eviction details
     */
    InboundAcceptResult shouldAcceptInbound(const std::string& source_addr);

    /**
     * Check if a new outbound connection should be attempted
     * @param blocks_only True if this is a blocks-only connection
     * @return true if connection should be attempted
     */
    bool shouldAcceptOutbound(bool blocks_only = false, bool is_anchor = false);

    /**
     * Register a new peer connection
     * Must be called when peer completes handshake
     *
     * @param peer_id Unique peer identifier
     * @param type Connection type (inbound/outbound/blocks-only)
     * @param addr IP address string (for subnet tracking)
     */
    void registerPeer(peer_id_t peer_id, PeerConnectionType type, const std::string& addr);

    /**
     * Unregister a peer connection
     * Must be called when peer disconnects
     *
     * @param peer_id Unique peer identifier
     */
    void unregisterPeer(peer_id_t peer_id);

    /**
     * Update peer activity timestamp
     * Called when peer sends/receives messages
     *
     * @param peer_id Peer identifier
     * @param is_recv True if received message, false if sent
     */
    void updateActivity(peer_id_t peer_id, bool is_recv);

    /**
     * Mark peer as having served recent valuable data
     * Protects peer from eviction
     *
     * @param peer_id Peer identifier
     * @param is_block True if served block, false if transaction
     */
    void markRecentService(peer_id_t peer_id, bool is_block);

    /**
     * Select a peer for eviction using Bitcoin Core algorithm
     *
     * Algorithm:
     * 1. Never evict outbound peers
     * 2. Protect peers that sent blocks in last hour
     * 3. Protect peers with score < 50
     * 4. Protect peers connected in last 2 minutes
     * 5. Protect subnet diversity (keep peers from different /16s)
     * 6. Evict lowest score, oldest connection as tie-breaker
     *
     * @return EvictionResult with evicted peer_id if successful
     */
    EvictionResult selectEvictionCandidate();

    /**
     * Get current connection counts
     */
    uint32_t getInboundCount() const { return m_inbound_count; }
    uint32_t getOutboundCount() const { return m_outbound_count; }
    uint32_t getBlocksOnlyCount() const { return m_blocks_only_count; }
    uint32_t getAnchorCount() const { return m_anchor_count; }
    uint32_t getTotalCount() const { return m_inbound_count + m_outbound_count + m_blocks_only_count + m_anchor_count; }

    /**
     * Check if an IP is a registered anchor peer
     */
    bool isAnchorPeer(const std::string& addr) const;

    /**
     * Register anchor peer IPs (called during Init from seed_nodes.h)
     */
    void addAnchorAddress(const std::string& ip);

    /**
     * Get list of anchor IPs that are not currently connected
     * Used by P2PService to auto-reconnect disconnected anchors
     */
    std::vector<std::string> getDisconnectedAnchors() const;

    /**
     * Check if outbound connection to this subnet is allowed (diversity enforcement)
     * Max 2 outbound connections per /16 subnet
     */
    bool isOutboundSubnetAllowed(const std::string& addr) const;

    /**
     * Get connection limits configuration
     */
    const ConnectionLimits& getLimits() const { return m_limits; }

private:
    // Configuration
    ConnectionLimits m_limits;
    std::shared_ptr<p2p::PeerScoringManager> m_scoring_manager;

    // Connection tracking
    uint32_t m_inbound_count{0};
    uint32_t m_outbound_count{0};
    uint32_t m_blocks_only_count{0};
    uint32_t m_anchor_count{0};

    // Anchor peer IPs — never evicted, auto-reconnected
    std::vector<std::string> m_anchor_addresses;

    // Eclipse prevention: max outbound connections per /16 subnet
    static constexpr uint32_t MAX_OUTBOUND_PER_SUBNET16 = 2;

    // Per-peer connection info
    std::unordered_map<peer_id_t, PeerConnectionInfo> m_peers;

    // Eviction protection thresholds
    static constexpr int64_t RECENT_CONNECTION_WINDOW_SECS = 120;  // 2 minutes
    static constexpr int64_t RECENT_SERVICE_WINDOW_SECS = 3600;    // 1 hour
    static constexpr int32_t LOW_SCORE_THRESHOLD = 50;             // Protect peers with score < 50

    // Eclipse attack prevention: per-IP and per-/16-subnet inbound limits
    static constexpr uint32_t MAX_INBOUND_PER_IP = 2;         // Max inbound connections from same IP
    static constexpr uint32_t MAX_INBOUND_PER_SUBNET16 = 4;   // Max inbound from same /16 subnet

    /**
     * Helper: Get all inbound peers as lightweight snapshots
     */
    std::vector<PeerConnectionInfo> getInboundPeers() const;

    /**
     * Helper: Check if peer should be protected from eviction
     */
    bool isProtectedFromEviction(const PeerConnectionInfo& peer) const;

    /**
     * Helper: Group peers by /16 subnet
     * Returns map of subnet -> peer_ids
     */
    std::unordered_map<std::string, std::vector<peer_id_t>> groupBySubnet(
        const std::vector<PeerConnectionInfo>& peers
    ) const;

    /**
     * Helper: Extract /16 subnet from IP address
     * Example: "192.168.1.100" -> "192.168"
     */
    std::string extractSubnet16(const std::string& addr) const;

    /**
     * Helper: Count inbound connections from a specific IP address
     */
    uint32_t countInboundFromIP(const std::string& ip) const;

    /**
     * Helper: Count inbound connections from a /16 subnet
     */
    uint32_t countInboundFromSubnet16(const std::string& subnet) const;

    /**
     * Helper: Get current Unix timestamp
     */
    int64_t getCurrentTime() const;
};

} // namespace dinero
