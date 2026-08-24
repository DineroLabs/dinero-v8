// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <chrono>
#include <cstdint>
#include <functional>

namespace dinero::qt::dashboard {

// Snapshot of the qt-app-side mining state. Populated by MainWindow's
// accessors, surfaced to IdentitySection via the LocalMiningProvider
// callback wired through CmdKPanel → MyNodeDashboard.
struct LocalMiningState {
    bool    active{false};
    QString miner_type;    // "internal" / "stratum_worker" / "external" / "gpu" / "daemon" / "none"
    double  hashrate{0.0}; // hashes/second
    // App-side uptime (seconds since MainWindow construction). Carried here
    // because the daemon's getuptime RPC doesn't exist on this build and the
    // dashboard wants something to show; "0h 00m" was wrong by default.
    std::chrono::seconds app_uptime{0};
};

using LocalMiningProvider = std::function<LocalMiningState()>;

// Snapshot of the daemon's Dynamic P2P state. Populated from the
// dynamic_p2p.observe RPC (added in dinero-v8 PR #140). When DPP is off
// or unavailable, `enabled` is false and counts are zero.
struct DynamicP2POverview {
    bool    enabled{false};
    QString mode;                  // "active_slow_churn" / "observe" / "off" / "error"
    int     hot_peers{0};
    int     warm_candidates{0};
    int     relay_registration_candidates{0};
    int     demote_candidates{0};
    int     connected_outbound{0};
};

// Read-only Tor onion-service state from getnetworkinfo.onion_service.
// Authentication details are deliberately not carried into the GUI model.
struct OnionServiceStatus {
    bool    available{false};
    bool    requested{false};
    bool    active{false};
    QString address;
    QString message;
    QString mode{"off"};
    bool embedded{false};
};

// Per-tick contribution snapshot. Sparkline buffers (rolling 5-min) live
// in NodePoller — this struct carries only the spot values that are
// rendered as plain labels.
struct ContributionStats {
    qint64  blocks_served_24h{0};      // Phase 2b: getnetworkinfo.relay.blocks_served_24h
    qint64  bytes_relayed_24h{0};      // Phase 2b: getnetworkinfo.relay.bytes_relayed_24h
    int     registrants_active{0};     // Phase 2b: getnetworkinfo.relay.registrants_count (was "circuits")
    int     hints_sent{0};             // RELAY_HINTS we've sent since launch
    int     peers_via_gossip{0};       // peers known to us only via gossip (proxy: received_relay)
    qint64  bytes_in_rate{0};          // current 1-sample rate (bytes/sec), for the label
    qint64  bytes_out_rate{0};
    qint64  relay_bytes_rate{0};
};

// Decentralization score (0.0–10.0) computed from observable signals.
// `breakdown` carries each weighted component so the tooltip can show the
// formula attribution. `label` is the plain-English bucket from the spec.
struct DecentralizationScore {
    double  total{0.0};                 // clamp(0, 10) of the weighted sum
    QString label;                      // "just observing" / "consuming responsibly" / etc.
    struct {
        double reachable{0.0};          // 0 or 1.0
        double relay_active{0.0};       // 0 or 2.0
        double uptime{0.0};             // 0..1.5
        double peer_diversity{0.0};     // 0..1.5
        double traffic{0.0};            // 0..1.0
        double mining{0.0};             // 0..1.5
        double gossip_reach{0.0};       // 0..1.5
    } breakdown;
};

// What we know about THIS node. Populated from getnetworkinfo +
// getrelayinfo + getmininginfo + getuptime.
struct NodeIdentity {
    QString node_id_hex;            // 40 chars (20 bytes hex), lower-case
    QString subversion;             // e.g. "/dinerod:b06ec828/"
    int     version{0};             // numeric protocol version
    quint64 services{0};            // service flags bitmap
    QString local_addr;             // best-known local advertised address
    quint16 local_port{0};
    enum Reachability { UNKNOWN, DIRECT, BEHIND_RELAY, UNREACHABLE };
    Reachability reachability{UNKNOWN};
    int     outbound_connections{0};
    bool    relay_fallback_eligible{false};
    bool    is_relay_active{false};
    int     registrants_count{0};
    int     grace_count{0};
    bool    is_mining{false};
    QString mining_destination;     // e.g. "EpycOne address" or pool URL
    double  shares_per_min{0.0};
    std::chrono::seconds uptime{0};
};

// What we know about THE NETWORK (as observed from this node).
// Populated from getblockchaininfo + getpeerinfo (height distribution)
// + getmempoolinfo.
struct ChainInfo {
    qint64  our_height{0};
    qint64  net_consensus_height{0};   // mode of peers' reported heights
    qint64  max_peer_height{0};
    QVector<qint64> peer_heights;      // raw peer heights for histogram
    double  difficulty{0.0};           // human-readable network difficulty
    int     mempool_tx_count{0};
    qint64  mempool_bytes{0};
    double  median_fee_una_per_vbyte{0.0};
    bool    has_median_fee{false};
    double  next_bits_delta_pct{0.0};  // optional; 0 if not available
    double  network_hashrate_hps{0.0}; // Phase 2a — from getmininginfo.networkhashps; used by Decentralization Score formula
};

// Phase 2b — one row of the DiscoverySection: a relay-hint cache entry
// for a specific target node id. (Source-discrimination is parked
// behind a RELAY_HINTS wire change; for Phase 2b we have endpoint-level
// data only.)
struct HintRow {
    QString target_node_id_hex;   // 40 hex chars
    QString endpoint;             // "addr:port", or "(no addr)" for malformed
    QString net;                  // "ipv4" / "ipv6"
    qint64  age_seconds{0};       // since learned_at
    int     dial_failures{0};
    bool    near_eviction{false};
};

// One row in the peers table.
struct PeerRow {
    QString addr;                  // "1.2.3.4:20999" or "relay:<id>:<circ>"
    bool    via_relay{false};
    QString relay_via_addr;        // "1.2.3.4:20999" of the relay we go through
    bool    is_inbound{false};
    QString fleet_name;            // "LA"/"SJ"/"NA"/"EU1" if a known fleet IP, else empty
    qint64  height{-1};
    qint64  ping_ms{-1};            // -1 = unmeasured
    int     quality_score{-1};      // 0..100; -1 = no DPP score yet
    bool    handshake_complete{true};
    bool    stalling{false};
    // Expanded-row data
    quint64 services{0};
    QString subversion;
    qint64  bytes_sent{0};
    qint64  bytes_recv{0};
    std::chrono::seconds connected_for{0};
    std::chrono::seconds last_message_ago{0};
};

// Phase 3 — local topology view model. This is intentionally a Qt-side
// approximation built from getpeerinfo + relay_hints.list + dashboard state;
// it is not a global network graph.
struct TopologyNode {
    QString id;
    QString label;
    QString endpoint;
    QString kind;       // self/direct/relay_virtual/hint/fleet
    QString bucket;     // hot/warm/demote/relay_candidate/empty
    int     quality_score{-1};
    bool    connected{false};
};

struct TopologyEdge {
    QString from_id;
    QString to_id;
    QString kind;       // direct/relay_virtual/hint
    QString via_relay;
};

struct TopologySnapshot {
    QVector<TopologyNode> nodes;
    QVector<TopologyEdge> edges;
};

}  // namespace dinero::qt::dashboard

Q_DECLARE_METATYPE(dinero::qt::dashboard::NodeIdentity)
Q_DECLARE_METATYPE(dinero::qt::dashboard::ChainInfo)
Q_DECLARE_METATYPE(dinero::qt::dashboard::PeerRow)
Q_DECLARE_METATYPE(QVector<dinero::qt::dashboard::PeerRow>)
Q_DECLARE_METATYPE(dinero::qt::dashboard::HintRow)
Q_DECLARE_METATYPE(QVector<dinero::qt::dashboard::HintRow>)
Q_DECLARE_METATYPE(dinero::qt::dashboard::DynamicP2POverview)
Q_DECLARE_METATYPE(dinero::qt::dashboard::OnionServiceStatus)
Q_DECLARE_METATYPE(dinero::qt::dashboard::ContributionStats)
Q_DECLARE_METATYPE(dinero::qt::dashboard::DecentralizationScore)
Q_DECLARE_METATYPE(dinero::qt::dashboard::TopologyNode)
Q_DECLARE_METATYPE(dinero::qt::dashboard::TopologyEdge)
Q_DECLARE_METATYPE(dinero::qt::dashboard::TopologySnapshot)
