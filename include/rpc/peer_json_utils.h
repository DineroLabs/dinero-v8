#pragma once

#include "din_json.h"
#include "daemon/p2p_manager.h"
#include "p2p/peer_quality_derivation.h"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace dinero::rpc {

inline uint64_t PeerTimepointToUnixSeconds(const std::chrono::system_clock::time_point& tp) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        tp.time_since_epoch()).count());
}

inline std::string FormatPeerServiceFlags(uint64_t service_flags) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << service_flags;
    return oss.str();
}

inline din::Json BuildPeerInfoJson(const ::PeerInfo& peer_info) {
    din::Json peer;
    const uint32_t advertised_best_height = std::max(peer_info.best_known_height, peer_info.start_height);
    const uint32_t observed_sync_height = std::max(peer_info.synced_headers, peer_info.synced_blocks);

    peer["addr"] = peer_info.address + ":" + std::to_string(peer_info.port);
    peer["inbound"] = !peer_info.is_outbound;
    peer["version"] = static_cast<int>(peer_info.protocol_version);
    peer["subver"] = peer_info.user_agent;
    peer["user_agent"] = peer_info.user_agent;
    peer["services"] = FormatPeerServiceFlags(peer_info.service_flags);
    peer["service_flags"] = static_cast<Json::UInt64>(peer_info.service_flags);

    peer["startingheight"] = static_cast<int>(peer_info.start_height);
    peer["start_height"] = static_cast<int>(peer_info.start_height);
    peer["bestknownheight"] = static_cast<int>(advertised_best_height);
    peer["best_known_height"] = static_cast<int>(advertised_best_height);
    peer["advertised_best_height"] = static_cast<int>(advertised_best_height);
    peer["synced_headers"] = static_cast<int>(peer_info.synced_headers);
    peer["synced_blocks"] = static_cast<int>(peer_info.synced_blocks);
    peer["observed_sync_height"] = static_cast<int>(observed_sync_height);

    peer["bytessent"] = static_cast<Json::UInt64>(peer_info.bytes_sent);
    peer["bytesrecv"] = static_cast<Json::UInt64>(peer_info.bytes_recv);

    const auto last_message_at = PeerTimepointToUnixSeconds(peer_info.last_message_at);
    const auto connected_since = PeerTimepointToUnixSeconds(peer_info.connected_since);
    peer["lastrecv"] = last_message_at;
    peer["last_message_at"] = last_message_at;
    peer["conntime"] = connected_since;
    peer["connected_since"] = connected_since;

    peer["connected"] = peer_info.is_connected;
    peer["compactblocks_enabled"] = peer_info.compact_blocks_enabled;
    peer["compactblocks_announce"] = peer_info.compact_blocks_announce;
    peer["compactblocks_version"] = static_cast<Json::UInt64>(peer_info.compact_blocks_version);

    // Phase 1.5 dashboard surface: ping (from EMA latency in PeerInfo)
    // + quality_score (derived per-call from PeerInfo state). Both fields
    // always present so consumers can rely on key existence.
    peer["ping_ms"] = static_cast<int>(peer_info.avg_latency_ms);
    peer["quality_score"] =
        dinero::p2p::BuildDynamicP2PQualitySnapshot(peer_info).score;

    return peer;
}

}  // namespace dinero::rpc
