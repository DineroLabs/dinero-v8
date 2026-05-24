// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QString>
#include <QVector>
#include <chrono>
#include <cstdint>

namespace dinero::qt::dashboard {

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
    quint32 difficulty_compact{0};     // nBits
    int     mempool_tx_count{0};
    qint64  mempool_bytes{0};
    qint64  median_fee_una_per_vbyte{0};
    double  next_bits_delta_pct{0.0};  // optional; 0 if not available
};

// One row in the peers table.
struct PeerRow {
    QString addr;                  // "1.2.3.4:20999" or "relay:<id>:<circ>"
    bool    via_relay{false};
    QString relay_via_addr;        // "1.2.3.4:20999" of the relay we go through
    bool    is_inbound{false};
    QString fleet_name;            // "LA"/"VA"/"MO"/"CN" if a known fleet IP, else empty
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

}  // namespace dinero::qt::dashboard
