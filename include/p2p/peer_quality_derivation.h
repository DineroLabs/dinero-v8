// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "p2p/peer_quality.h"
#include "daemon/p2p_manager.h"  // for PeerInfo + ServiceFlags

namespace dinero::p2p {

// Derive a PeerQualitySnapshot from a PeerInfo by applying observable
// events (handshake state, useful headers/blocks, latency, relay
// capability). Pure: holds no state, re-derives on each call.
// Extracted from p2p_service.cpp so both the P2P service and JSON-RPC
// peer-row builder can use the same logic.
inline PeerQualitySnapshot BuildDynamicP2PQualitySnapshot(
        const ::PeerInfo& peer) {
    PeerQuality quality;
    if (peer.is_connected) {
        quality.Apply(PeerQualityEvent::ConnectionSuccess);
    }
    if (peer.protocol_version != 0 || !peer.user_agent.empty()) {
        quality.Apply(PeerQualityEvent::HandshakeSuccess);
    }
    if (peer.synced_headers > 0 || peer.best_known_height > 0 ||
        peer.best_height > 0) {
        quality.Apply(PeerQualityEvent::UsefulHeader);
    }
    if (peer.synced_blocks > 0) {
        quality.Apply(PeerQualityEvent::UsefulBlock);
    }
    if (peer.avg_latency_ms > 0.0) {
        quality.RecordLatency(static_cast<uint32_t>(peer.avg_latency_ms));
    }

    auto snapshot = quality.Snapshot();
    if ((peer.service_flags & dinero::ServiceFlags::NODE_RELAY) != 0 &&
        peer.is_connected) {
        snapshot.relay_successes = 1;
        snapshot.relay_candidate = snapshot.score >= 55;
    }
    return snapshot;
}

}  // namespace dinero::p2p
