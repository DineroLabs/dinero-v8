// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/rpc_relay_hints_handlers.h"

#include "daemon/p2p_manager.h"
#include "daemon/services/p2p_service.h"
#include "p2p/addr_v2.h"

#include <cstdio>
#include <string>
#include <vector>

namespace dinero::rpc {

namespace {

const char* NetworkTypeToString(dinero::p2p::NetworkType n) {
    switch (n) {
        case dinero::p2p::NetworkType::IPV4:  return "ipv4";
        case dinero::p2p::NetworkType::IPV6:  return "ipv6";
        case dinero::p2p::NetworkType::TORV3: return "torv3";
        case dinero::p2p::NetworkType::I2P:   return "i2p";
        default:                              return "unknown";
    }
}

std::string EncodeAddr(dinero::p2p::NetworkType net,
                       const std::vector<uint8_t>& bytes) {
    if (net == dinero::p2p::NetworkType::IPV4 && bytes.size() == 4) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      static_cast<unsigned>(bytes[0]),
                      static_cast<unsigned>(bytes[1]),
                      static_cast<unsigned>(bytes[2]),
                      static_cast<unsigned>(bytes[3]));
        return buf;
    }
    if (net == dinero::p2p::NetworkType::IPV6 && bytes.size() == 16) {
        // Manual colon-hex: 8 groups of 4 hex separated by colons.
        // Not RFC-4291 compressed; adequate for an observational RPC.
        // Max output: 39 chars + NUL → char[40] is sufficient.
        char buf[40];
        std::snprintf(buf, sizeof(buf),
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
            bytes[0],  bytes[1],  bytes[2],  bytes[3],
            bytes[4],  bytes[5],  bytes[6],  bytes[7],
            bytes[8],  bytes[9],  bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
        return buf;
    }
    return "";  // TORV3/I2P/unknown/malformed — empty string, not crash
}

din::Json DisabledShape() {
    din::Json out;
    out["rpc_schema"]    = "din.rpc.v1";
    out["targets"]       = din::Json(Json::arrayValue);
    out["total_targets"] = 0;
    out["ttl_seconds"]   = 0;
    out["max_failures"]  = 0;
    return out;
}

}  // namespace

din::Json HandleRelayHintsList(dinero::P2PService* p2p_service) {
    if (!p2p_service) return DisabledShape();

    const auto snapshot = p2p_service->get().SnapshotRelayHintsForRpc();

    din::Json out;
    out["rpc_schema"]    = "din.rpc.v1";
    out["ttl_seconds"]   = static_cast<Json::UInt>(snapshot.ttl_seconds);
    out["max_failures"]  = snapshot.max_failures;
    out["total_targets"] = static_cast<Json::UInt>(snapshot.entries.size());

    din::Json targets(Json::arrayValue);
    for (const auto& entry : snapshot.entries) {
        din::Json target;
        target["target_node_id_hex"] = entry.target_hex;
        din::Json endpoints(Json::arrayValue);
        for (const auto& ep : entry.endpoints) {
            din::Json e;
            e["net"]           = NetworkTypeToString(ep.net);
            e["addr"]          = EncodeAddr(ep.net, ep.addr);
            e["port"]          = static_cast<Json::UInt>(ep.port);
            e["age_seconds"]   = static_cast<Json::UInt>(ep.age_seconds);
            e["dial_failures"] = ep.dial_failures;
            e["near_eviction"] = ep.near_eviction;
            endpoints.append(e);
        }
        target["endpoints"] = endpoints;
        targets.append(target);
    }
    out["targets"] = targets;
    return out;
}

}  // namespace dinero::rpc
