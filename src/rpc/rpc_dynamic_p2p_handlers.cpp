// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/rpc_dynamic_p2p_handlers.h"

#include "daemon/services/p2p_service.h"
#include "p2p/peer_quality.h"
#include "p2p/peer_quality_derivation.h"

namespace dinero::rpc {

namespace {

din::Json QualityToJson(const dinero::p2p::PeerQualitySnapshot& q) {
    din::Json j;
    j["score"]                = q.score;
    j["connection_successes"] = static_cast<Json::UInt>(q.connection_successes);
    j["connection_failures"]  = static_cast<Json::UInt>(q.connection_failures);
    j["handshake_successes"]  = static_cast<Json::UInt>(q.handshake_successes);
    j["handshake_failures"]   = static_cast<Json::UInt>(q.handshake_failures);
    j["useful_headers"]       = static_cast<Json::UInt>(q.useful_headers);
    j["useful_blocks"]        = static_cast<Json::UInt>(q.useful_blocks);
    j["stale_height_events"]  = static_cast<Json::UInt>(q.stale_height_events);
    j["relay_successes"]      = static_cast<Json::UInt>(q.relay_successes);
    j["relay_failures"]       = static_cast<Json::UInt>(q.relay_failures);
    j["latency_ms"]           = static_cast<Json::UInt>(q.latency_ms);
    j["hot_peer_candidate"]   = q.hot_peer_candidate;
    j["relay_candidate"]      = q.relay_candidate;
    return j;
}

din::Json StringListToJsonArray(const std::vector<std::string>& src) {
    din::Json arr(Json::arrayValue);
    for (const auto& s : src) arr.append(s);
    return arr;
}

din::Json DisabledShape(const std::string& mode) {
    din::Json out;
    out["rpc_schema"] = "din.rpc.v1";
    out["enabled"]    = false;
    out["mode"]       = mode;
    out["governor"]   = Json::nullValue;
    out["peers"]      = din::Json(Json::arrayValue);
    return out;
}

}  // namespace

din::Json HandleDynamicP2PObserve(dinero::P2PService* p2p_service) {
    if (!p2p_service) {
        return DisabledShape("error");
    }

    const auto status = p2p_service->GetNetworkStatus();
    const bool enabled = status.dynamic_p2p_enabled;
    const std::string& mode = status.dynamic_p2p_mode;

    if (!enabled || mode == "off") {
        return DisabledShape(mode);
    }

    din::Json out;
    out["rpc_schema"] = "din.rpc.v1";
    out["enabled"]    = enabled;
    out["mode"]       = mode;

    // Governor snapshot from NetworkStatus.dynamic_p2p_governor.
    const auto& gov = status.dynamic_p2p_governor;
    din::Json governor;
    governor["available"]       = gov.available;
    governor["mode"]            = gov.mode;
    governor["candidate_source"] = gov.candidate_source;
    governor["connected_outbound"]   = static_cast<Json::UInt64>(gov.connected_outbound);
    governor["configured_seed_hot"]  = static_cast<Json::UInt64>(gov.configured_seed_hot);
    governor["relay_capable_seen"]   = static_cast<Json::UInt64>(gov.relay_capable_seen);
    governor["hot_peers"]                     = StringListToJsonArray(gov.hot_peers);
    governor["warm_candidates"]               = StringListToJsonArray(gov.warm_candidates);
    governor["relay_registration_candidates"] = StringListToJsonArray(gov.relay_registration_candidates);
    governor["demote_candidates"]             = StringListToJsonArray(gov.demote_candidates);
    out["governor"] = governor;

    // Per-peer snapshot: GetConnectedPeers() is a public convenience accessor
    // on P2PService that wraps p2p_mgr_->get_connected_peers().
    din::Json peers(Json::arrayValue);
    for (const auto& peer : p2p_service->GetConnectedPeers()) {
        din::Json row;
        row["addr"]    = peer.address + ":" + std::to_string(peer.port);
        row["quality"] = QualityToJson(
            dinero::p2p::BuildDynamicP2PQualitySnapshot(peer));
        peers.append(row);
    }
    out["peers"] = peers;

    return out;
}

}  // namespace dinero::rpc
