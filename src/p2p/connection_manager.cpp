// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "p2p/connection_manager.h"
#include "common/logger.h"
#include <chrono>
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace dinero {

ConnectionManager::ConnectionManager(
    const ConnectionLimits& limits,
    std::shared_ptr<p2p::PeerScoringManager> scoring_manager)
    : m_limits(limits), m_scoring_manager(scoring_manager) {

    g_logger.info("ConnectionManager initialized with limits: "
                 "inbound=" + std::to_string(m_limits.max_inbound) +
                 ", outbound=" + std::to_string(m_limits.max_outbound) +
                 ", blocks_only=" + std::to_string(m_limits.max_blocks_only) +
                 ", anchor=" + std::to_string(m_limits.max_anchor) +
                 ", total=" + std::to_string(m_limits.max_total));
}

ConnectionManager::InboundAcceptResult ConnectionManager::shouldAcceptInbound(const std::string& source_addr) {
    InboundAcceptResult result;

    // Eclipse prevention: per-IP inbound limit (max 2 from same IP)
    uint32_t ip_count = countInboundFromIP(source_addr);
    if (ip_count >= MAX_INBOUND_PER_IP) {
        g_logger.debug("Per-IP inbound limit reached for " + source_addr +
                       " (" + std::to_string(ip_count) + "/" + std::to_string(MAX_INBOUND_PER_IP) + ")");
        result.accept = false;
        result.reason = "Per-IP inbound limit reached (" + std::to_string(MAX_INBOUND_PER_IP) + ")";
        return result;
    }

    // Eclipse prevention: per-/16-subnet inbound limit (max 4 from same /16)
    std::string subnet = extractSubnet16(source_addr);
    uint32_t subnet_count = countInboundFromSubnet16(subnet);
    if (subnet_count >= MAX_INBOUND_PER_SUBNET16) {
        g_logger.debug("Per-subnet inbound limit reached for " + subnet +
                       " (" + std::to_string(subnet_count) + "/" + std::to_string(MAX_INBOUND_PER_SUBNET16) + ")");
        result.accept = false;
        result.reason = "Per-subnet inbound limit reached (" + std::to_string(MAX_INBOUND_PER_SUBNET16) + ")";
        return result;
    }

    // Check total connection limit
    if (getTotalCount() >= m_limits.max_total) {
        g_logger.debug("Total connections at max (" + std::to_string(m_limits.max_total) + "), rejecting inbound");
        result.accept = false;
        result.reason = "Total connection limit reached";
        return result;
    }

    // If under inbound limit, accept immediately
    if (m_inbound_count < m_limits.max_inbound) {
        result.accept = true;
        result.requires_eviction = false;
        result.reason = "Under inbound limit";
        return result;
    }

    // At inbound capacity — attempt eviction
    g_logger.info("Inbound connections at max (" + std::to_string(m_limits.max_inbound) + "), attempting eviction");

    EvictionResult eviction = selectEvictionCandidate();
    if (eviction.evicted) {
        g_logger.info("Selected peer " + eviction.evicted_peer_id +
                     " for eviction to make room for new inbound connection. Reason: " + eviction.reason);

        result.accept = true;
        result.requires_eviction = true;
        result.evicted_peer_id = eviction.evicted_peer_id;
        result.reason = eviction.reason;
        return result;
    }

    g_logger.warning("Failed to find eviction candidate, rejecting inbound connection");
    result.accept = false;
    result.reason = "No eviction candidate available (all peers protected)";
    return result;
}

bool ConnectionManager::shouldAcceptOutbound(bool blocks_only, bool is_anchor) {
    // Check total connection limit
    if (getTotalCount() >= m_limits.max_total) {
        return false;
    }

    // Anchor peers use a dedicated budget — never compete with exploratory outbound
    if (is_anchor) {
        return m_anchor_count < m_limits.max_anchor;
    }

    if (blocks_only) {
        return m_blocks_only_count < m_limits.max_blocks_only;
    } else {
        return m_outbound_count < m_limits.max_outbound;
    }
}

void ConnectionManager::registerPeer(peer_id_t peer_id, PeerConnectionType type, const std::string& addr) {
    // Check if peer already registered (shouldn't happen, but be defensive)
    if (m_peers.find(peer_id) != m_peers.end()) {
        g_logger.warning("Peer " + peer_id + " already registered, ignoring duplicate registration");
        return;
    }

    // Create connection info
    PeerConnectionInfo info;
    info.peer_id = peer_id;
    info.connection_type = type;
    info.connection_time = getCurrentTime();
    info.last_recv_time = info.connection_time;
    info.last_send_time = info.connection_time;
    info.addr = addr;
    info.current_score = 0;  // Will be updated by scoring manager
    info.served_recent_block = false;
    info.served_recent_tx = false;

    // Update counts
    switch (type) {
        case PeerConnectionType::INBOUND:
            m_inbound_count++;
            break;
        case PeerConnectionType::OUTBOUND_FULL:
            m_outbound_count++;
            break;
        case PeerConnectionType::OUTBOUND_BLOCKS:
            m_blocks_only_count++;
            break;
        case PeerConnectionType::OUTBOUND_ANCHOR:
            m_anchor_count++;
            break;
    }

    m_peers[peer_id] = info;

    g_logger.debug("Registered peer " + peer_id +
                  " (type=" + std::to_string(static_cast<int>(type)) +
                  ", addr=" + addr +
                  "). Counts: inbound=" + std::to_string(m_inbound_count) +
                  ", outbound=" + std::to_string(m_outbound_count) +
                  ", blocks_only=" + std::to_string(m_blocks_only_count));
}

void ConnectionManager::unregisterPeer(peer_id_t peer_id) {
    auto it = m_peers.find(peer_id);
    if (it == m_peers.end()) {
        g_logger.warning("Attempted to unregister unknown peer " + peer_id);
        return;
    }

    // Update counts
    const PeerConnectionInfo& info = it->second;
    switch (info.connection_type) {
        case PeerConnectionType::INBOUND:
            if (m_inbound_count > 0) m_inbound_count--;
            break;
        case PeerConnectionType::OUTBOUND_FULL:
            if (m_outbound_count > 0) m_outbound_count--;
            break;
        case PeerConnectionType::OUTBOUND_BLOCKS:
            if (m_blocks_only_count > 0) m_blocks_only_count--;
            break;
        case PeerConnectionType::OUTBOUND_ANCHOR:
            if (m_anchor_count > 0) m_anchor_count--;
            break;
    }

    m_peers.erase(it);

    g_logger.debug("Unregistered peer " + peer_id +
                  ". Counts: inbound=" + std::to_string(m_inbound_count) +
                  ", outbound=" + std::to_string(m_outbound_count) +
                  ", blocks_only=" + std::to_string(m_blocks_only_count));
}

void ConnectionManager::updateActivity(peer_id_t peer_id, bool is_recv) {
    auto it = m_peers.find(peer_id);
    if (it == m_peers.end()) {
        return;  // Peer not registered yet (handshake incomplete)
    }

    int64_t now = getCurrentTime();
    if (is_recv) {
        it->second.last_recv_time = now;
    } else {
        it->second.last_send_time = now;
    }
}

void ConnectionManager::markRecentService(peer_id_t peer_id, bool is_block) {
    auto it = m_peers.find(peer_id);
    if (it == m_peers.end()) {
        return;
    }

    if (is_block) {
        it->second.served_recent_block = true;
        g_logger.debug("Peer " + peer_id + " marked as serving recent block (eviction protection)");
    } else {
        it->second.served_recent_tx = true;
        g_logger.debug("Peer " + peer_id + " marked as serving recent tx (eviction protection)");
    }
}

EvictionResult ConnectionManager::selectEvictionCandidate() {
    EvictionResult result;
    result.evicted = false;

    // Get all inbound peers (outbound peers are NEVER evicted)
    std::vector<PeerConnectionInfo> candidates = getInboundPeers();

    if (candidates.empty()) {
        result.reason = "No inbound peers to evict";
        return result;
    }

    g_logger.debug("Eviction candidate selection: " + std::to_string(candidates.size()) + " inbound peers");

    // Step 0: Remove anchor peers (ABSOLUTE PROTECTION — never evicted)
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [this](const PeerConnectionInfo& peer) {
                if (isAnchorPeer(peer.addr)) {
                    g_logger.debug("  - Protecting peer " + peer.peer_id + " (anchor peer: " + peer.addr + ")");
                    return true;
                }
                return false;
            }),
        candidates.end()
    );

    if (candidates.empty()) {
        result.reason = "All inbound peers are anchor peers (protected)";
        return result;
    }

    // Step 1: Remove peers that served recent blocks (HIGHEST PROTECTION)
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [](const PeerConnectionInfo& peer) {
                if (peer.served_recent_block) {
                    g_logger.debug("  - Protecting peer " + peer.peer_id + " (served recent block)");
                    return true;
                }
                return false;
            }),
        candidates.end()
    );

    if (candidates.empty()) {
        result.reason = "All inbound peers served recent blocks (protected)";
        return result;
    }

    // Step 2: Remove peers with low misbehavior scores (< 50)
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [this](const PeerConnectionInfo& peer) {
                // Sync current score from PeerScoringManager
                int32_t current_score = m_scoring_manager->getScore(peer.peer_id);
                if (current_score < LOW_SCORE_THRESHOLD) {
                    g_logger.debug("  - Protecting peer " + peer.peer_id +
                                  " (low score=" + std::to_string(current_score) + ")");
                    return true;
                }
                return false;
            }),
        candidates.end()
    );

    if (candidates.empty()) {
        result.reason = "All remaining peers have low misbehavior scores (protected)";
        return result;
    }

    // Step 3: Remove recently connected peers (last 2 minutes)
    int64_t now = getCurrentTime();
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [now](const PeerConnectionInfo& peer) {
                int64_t connection_age = now - peer.connection_time;
                if (connection_age < RECENT_CONNECTION_WINDOW_SECS) {
                    g_logger.debug("  - Protecting peer " + peer.peer_id +
                                  " (recent connection, age=" + std::to_string(connection_age) + "s)");
                    return true;
                }
                return false;
            }),
        candidates.end()
    );

    if (candidates.empty()) {
        result.reason = "All remaining peers are recent connections (protected)";
        return result;
    }

    // Step 4: Protect subnet diversity
    auto subnet_groups = groupBySubnet(candidates);

    if (subnet_groups.size() > 1) {
        std::vector<PeerConnectionInfo> diverse_protected;

        for (const auto& [subnet, peer_ids] : subnet_groups) {
            if (peer_ids.size() == 1) {
                auto it = std::find_if(candidates.begin(), candidates.end(),
                    [&peer_ids](const PeerConnectionInfo& p) {
                        return p.peer_id == peer_ids[0];
                    });

                if (it != candidates.end()) {
                    g_logger.debug("  - Protecting peer " + it->peer_id +
                                  " (subnet diversity: " + subnet + ")");
                    diverse_protected.push_back(*it);
                }
            }
        }

        for (const auto& protected_peer : diverse_protected) {
            candidates.erase(
                std::remove_if(candidates.begin(), candidates.end(),
                    [&protected_peer](const PeerConnectionInfo& p) {
                        return p.peer_id == protected_peer.peer_id;
                    }),
                candidates.end()
            );
        }
    }

    if (candidates.empty()) {
        result.reason = "All remaining peers protected by subnet diversity";
        return result;
    }

    // Step 5: Evict peer with HIGHEST misbehavior score
    auto eviction_candidate = std::max_element(
        candidates.begin(), candidates.end(),
        [this](const PeerConnectionInfo& a, const PeerConnectionInfo& b) {
            int32_t score_a = m_scoring_manager->getScore(a.peer_id);
            int32_t score_b = m_scoring_manager->getScore(b.peer_id);

            if (score_a != score_b) {
                return score_a < score_b;
            }

            return a.connection_time > b.connection_time;
        }
    );

    if (eviction_candidate != candidates.end()) {
        result.evicted = true;
        result.evicted_peer_id = eviction_candidate->peer_id;

        int32_t final_score = m_scoring_manager->getScore(eviction_candidate->peer_id);
        result.reason = "Highest misbehavior score among " + std::to_string(candidates.size()) +
                       " candidates (score=" + std::to_string(final_score) +
                       ", connection_age=" + std::to_string(now - eviction_candidate->connection_time) + "s)";

        g_logger.info("Selected peer " + result.evicted_peer_id + " for eviction: " + result.reason);
    } else {
        result.reason = "No eviction candidate found (algorithm error)";
    }

    return result;
}

std::vector<PeerConnectionInfo> ConnectionManager::getInboundPeers() const {
    std::vector<PeerConnectionInfo> inbound_peers;
    inbound_peers.reserve(m_inbound_count);

    for (const auto& [peer_id, info] : m_peers) {
        if (info.connection_type == PeerConnectionType::INBOUND) {
            inbound_peers.push_back(info);
        }
    }

    return inbound_peers;
}

bool ConnectionManager::isProtectedFromEviction(const PeerConnectionInfo& peer) const {
    // Outbound and anchor peers are never evicted
    if (peer.connection_type != PeerConnectionType::INBOUND) {
        return true;
    }
    // Inbound connections from anchor IPs are also protected
    if (isAnchorPeer(peer.addr)) {
        return true;
    }

    if (peer.served_recent_block) {
        return true;
    }

    int32_t score = m_scoring_manager->getScore(peer.peer_id);
    if (score < LOW_SCORE_THRESHOLD) {
        return true;
    }

    int64_t connection_age = getCurrentTime() - peer.connection_time;
    if (connection_age < RECENT_CONNECTION_WINDOW_SECS) {
        return true;
    }

    return false;
}

std::unordered_map<std::string, std::vector<peer_id_t>>
ConnectionManager::groupBySubnet(const std::vector<PeerConnectionInfo>& peers) const {
    std::unordered_map<std::string, std::vector<peer_id_t>> subnet_map;

    for (const auto& peer : peers) {
        std::string subnet = extractSubnet16(peer.addr);
        subnet_map[subnet].push_back(peer.peer_id);
    }

    return subnet_map;
}

std::string ConnectionManager::extractSubnet16(const std::string& addr) const {
    size_t first_dot = addr.find('.');
    if (first_dot == std::string::npos) {
        return addr;
    }

    size_t second_dot = addr.find('.', first_dot + 1);
    if (second_dot == std::string::npos) {
        return addr;
    }

    return addr.substr(0, second_dot);
}

int64_t ConnectionManager::getCurrentTime() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

uint32_t ConnectionManager::countInboundFromIP(const std::string& ip) const {
    uint32_t count = 0;
    for (const auto& [peer_id, info] : m_peers) {
        if (info.connection_type == PeerConnectionType::INBOUND && info.addr == ip) {
            count++;
        }
    }
    return count;
}

uint32_t ConnectionManager::countInboundFromSubnet16(const std::string& subnet) const {
    uint32_t count = 0;
    for (const auto& [peer_id, info] : m_peers) {
        if (info.connection_type == PeerConnectionType::INBOUND &&
            extractSubnet16(info.addr) == subnet) {
            count++;
        }
    }
    return count;
}

bool ConnectionManager::isAnchorPeer(const std::string& addr) const {
    for (const auto& anchor_ip : m_anchor_addresses) {
        if (addr == anchor_ip || addr.find(anchor_ip) == 0) {
            return true;
        }
    }
    return false;
}

void ConnectionManager::addAnchorAddress(const std::string& ip) {
    for (const auto& existing : m_anchor_addresses) {
        if (existing == ip) return;
    }
    m_anchor_addresses.push_back(ip);
    g_logger.info("ConnectionManager: registered anchor peer " + ip);
}

std::vector<std::string> ConnectionManager::getDisconnectedAnchors() const {
    std::unordered_set<std::string> connected_ips;
    for (const auto& [peer_id, info] : m_peers) {
        connected_ips.insert(info.addr);
    }

    std::vector<std::string> disconnected;
    for (const auto& anchor_ip : m_anchor_addresses) {
        if (connected_ips.find(anchor_ip) == connected_ips.end()) {
            disconnected.push_back(anchor_ip);
        }
    }
    return disconnected;
}

bool ConnectionManager::isOutboundSubnetAllowed(const std::string& addr) const {
    std::string target_subnet = extractSubnet16(addr);
    uint32_t subnet_count = 0;

    for (const auto& [peer_id, info] : m_peers) {
        if (info.connection_type == PeerConnectionType::OUTBOUND_FULL ||
            info.connection_type == PeerConnectionType::OUTBOUND_BLOCKS) {
            if (extractSubnet16(info.addr) == target_subnet) {
                subnet_count++;
            }
        }
    }
    return subnet_count < MAX_OUTBOUND_PER_SUBNET16;
}

} // namespace dinero
