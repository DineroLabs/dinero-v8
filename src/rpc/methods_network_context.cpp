/**
 * Network RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates network RPC methods from legacy globals to DaemonContext.
 * Compare with network_rpc_handlers.cpp to see the difference.
 *
 * OLD PATTERN (legacy):
 *   extern P2PManager* g_peer_manager;
 *   auto peers = dinero::legacy::g_peer_manager()->get_connected_peers();
 *
 * NEW PATTERN (context-aware):
 *   auto p2p = ctx.daemon->p2p;
 *   auto peers = p2p->getConnectedPeers();
 *
 * Benefits:
 * - No dependency on global variables
 * - Testable with mock P2P services
 * - Clear dependency tracking
 * - Thread-safe service access
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "rpc/peer_json_utils.h"
#include "daemon/daemon_context.h"
#include "daemon/services/p2p_service.h"
#include "consensus/chainparams.h"
#include "common/logger.h"
#include <memory>
#include <atomic>

namespace {

bool ParseNodeEndpoint(const std::string& node, std::string* host, uint16_t* port) {
    if (!host || !port) {
        return false;
    }
    if (node.empty()) {
        return false;
    }

    const size_t colon_pos = node.rfind(':');
    if (colon_pos == std::string::npos) {
        *host = node;
        *port = 20999;
        return true;
    }

    *host = node.substr(0, colon_pos);
    if (host->empty()) {
        return false;
    }

    try {
        const auto parsed = std::stoul(node.substr(colon_pos + 1));
        if (parsed == 0 || parsed > 65535) {
            return false;
        }
        *port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::shared_ptr<dinero::P2PService> GetP2PService(const ExecutionContext& ctx) {
    if (!ctx.daemon || !ctx.daemon->p2p) {
        return {};
    }
    return std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE NETWORK RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * getpeerinfo - Returns information about connected peers
 *
 * OLD: extern P2PManager* g_peer_manager;
 *      dinero::legacy::g_peer_manager()->get_connected_peers()
 *
 * NEW: ctx.daemon->p2p->getConnectedPeers()
 */
din::Json rpc_context_getpeerinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result(Json::arrayValue);

    if (!ctx.daemon || !ctx.daemon->p2p) {
        // Return empty array if P2P not available
        return result;
    }

    auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
    if (!p2p) {
        return result;
    }

    // Get thread-safe snapshot of connected peers
    auto peers = p2p->GetConnectedPeers();

    for (const auto& peerInfo : peers) {
        result.append(dinero::rpc::BuildPeerInfoJson(peerInfo));
    }

    return result;
}

/**
 * getconnectioncount - Returns the number of connections to other nodes
 *
 * NEW: ctx.daemon->p2p->getPeerCount()
 */
din::Json rpc_context_getconnectioncount(const ExecutionContext& ctx, const din::Json& params) {
    auto p2p = GetP2PService(ctx);
    if (!p2p) {
        return 0;
    }
    return static_cast<int>(p2p->GetPeerCount());
}

/**
 * getnetworkinfo - Returns P2P network status and reachability diagnostics
 *
 * This is the daemon truth source for operator CLI and Qt. Keep UI wording
 * out of this layer; expose concrete facts about listening, peer direction,
 * advertised addresses, and UPnP/NAT-PMP state.
 */
din::Json rpc_context_getnetworkinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    auto p2p = GetP2PService(ctx);
    const auto network_name = dinero::Params().name;

    result["version"] = 80000;
    result["subversion"] = dinero::P2PService::GetUserAgent();
    result["protocolversion"] = static_cast<int>(dinero::P2PService::GetProtocolVersion());
    result["network"] = network_name;
    result["localrelay"] = false;
    result["timeoffset"] = 0;

    din::Json networks(Json::arrayValue);
    din::Json ipv4;
    ipv4["name"] = "ipv4";
    ipv4["limited"] = false;
    ipv4["reachable"] = true;
    ipv4["proxy"] = "";
    networks.append(ipv4);
    din::Json onion;
    onion["name"] = "onion";
    onion["limited"] = true;
    onion["reachable"] = false;
    onion["proxy"] = "";
    networks.append(onion);
    din::Json i2p;
    i2p["name"] = "i2p";
    i2p["limited"] = true;
    i2p["reachable"] = false;
    i2p["proxy"] = "";
    networks.append(i2p);
    result["networks"] = networks;

    if (!p2p) {
        result["networkactive"] = false;
        result["connections"] = 0;
        result["connections_in"] = 0;
        result["connections_out"] = 0;
        result["listen"] = false;
        result["listen_port"] = 0;
        result["localaddresses"] = din::Json(Json::arrayValue);
        result["warnings"] = "P2P service not available";

        din::Json portmap;
        portmap["requested"] = false;
        portmap["active"] = false;
        portmap["mode"] = "disabled";
        portmap["protocol"] = "";
        portmap["external_address"] = "";
        portmap["external_port"] = 0;
        portmap["message"] = "P2P service not available";
        result["port_mapping"] = portmap;
        din::Json onion_transport;
        onion_transport["configured"] = false;
        onion_transport["enabled"] = false;
        onion_transport["reachable"] = false;
        onion_transport["auto_detected"] = false;
        onion_transport["proxy"] = "";
        onion_transport["note"] = "P2P service not available";
        result["onion_transport"] = onion_transport;
        return result;
    }

    const auto status = p2p->GetNetworkStatus();
    networks.clear();
    ipv4["reachable"] = status.network_active;
    networks.append(ipv4);
    onion["limited"] = !status.onion_transport_reachable;
    onion["reachable"] = status.onion_transport_reachable;
    onion["proxy"] = status.onion_proxy;
    networks.append(onion);
    i2p["limited"] = true;
    i2p["reachable"] = false;
    i2p["proxy"] = "";
    networks.append(i2p);
    result["networks"] = networks;

    result["networkactive"] = status.network_active;
    result["localrelay"] = status.local_relay;
    result["relay_mode"] = status.relay_mode;
    result["connections"] = static_cast<Json::UInt64>(status.connections);
    result["connections_in"] = static_cast<Json::UInt64>(status.inbound);
    result["connections_out"] = static_cast<Json::UInt64>(status.outbound);
    result["listen"] = status.listening;
    result["listen_port"] = static_cast<int>(status.listen_port);
    const bool direct_inbound_observed = status.inbound > 0;
    const bool direct_advertised = !status.advertised_addresses.empty();
    const bool direct_reachable =
        direct_inbound_observed || direct_advertised || status.port_mapping_active;
    const bool relay_fallback_eligible =
        status.network_active && status.listening && !direct_reachable;
    result["reachable"] = direct_reachable;
    result["inbound_observed"] = direct_inbound_observed;
    result["direct_inbound_observed"] = direct_inbound_observed;
    result["direct_reachable"] = direct_reachable;
    result["relay_fallback_eligible"] = relay_fallback_eligible;

    din::Json relay;
    relay["mode"] = status.relay_mode;
    relay["local"] = status.local_relay;
    // Auto-mode toggle: true when the relay role is currently engaged for
    // any reason (today: mining is running; future: spare-bandwidth
    // heuristic, dashboard opt-in, etc.). NOT a mining-only signal — see
    // docs/network-participation.md.
    relay["active"] = status.relay_active;
    relay["fallback_eligible"] = relay_fallback_eligible;
    din::Json relay_hints;
    relay_hints["received_self"] =
        static_cast<Json::UInt64>(status.relay_hints_received_self);
    relay_hints["received_relay"] =
        static_cast<Json::UInt64>(status.relay_hints_received_relay);
    relay_hints["evicted_expired"] =
        static_cast<Json::UInt64>(status.relay_hints_evicted_expired);
    relay_hints["evicted_failure"] =
        static_cast<Json::UInt64>(status.relay_hints_evicted_failure);
    relay["hints"] = relay_hints;
    din::Json relay_directory;
    relay_directory["entries"] =
        static_cast<Json::UInt64>(status.relay_directory_entries);
    relay_directory["grace_pending"] =
        static_cast<Json::UInt64>(status.relay_directory_grace_pending);
    relay["directory"] = relay_directory;
    result["relay"] = relay;

    din::Json local_addresses(Json::arrayValue);
    for (const auto& [address, port] : status.advertised_addresses) {
        din::Json entry;
        entry["address"] = address;
        entry["port"] = static_cast<int>(port);
        entry["score"] = 1;
        local_addresses.append(entry);
    }
    result["localaddresses"] = local_addresses;
    result["advertised_addresses_count"] = static_cast<Json::UInt64>(status.advertised_addresses.size());

    din::Json portmap;
    portmap["requested"] = status.port_mapping_requested;
    portmap["active"] = status.port_mapping_active;
    portmap["mode"] = status.port_mapping_mode;
    portmap["protocol"] = status.port_mapping_protocol;
    portmap["external_address"] = status.port_mapping_external_address;
    portmap["external_port"] = static_cast<int>(status.port_mapping_external_port);
    portmap["message"] = status.port_mapping_message;
    result["port_mapping"] = portmap;

    // NAT traversal Phase C1: STUN-discovered public address. Empty
    // strings while discovery is in flight; populated by P2PService
    // once a STUN BINDING RESPONSE arrives.
    din::Json stun;
    stun["discovered_address"] = status.stun_discovered_address;
    stun["server_used"] = status.stun_server_used;
    stun["message"] = status.stun_message;
    result["stun"] = stun;

    din::Json onion_transport;
    onion_transport["configured"] = status.onion_transport_configured;
    onion_transport["enabled"] = status.onion_transport_enabled;
    onion_transport["reachable"] = status.onion_transport_reachable;
    onion_transport["auto_detected"] = status.onion_transport_auto_detected;
    onion_transport["proxy"] = status.onion_proxy;
    onion_transport["note"] = status.onion_transport_enabled
        ? (status.onion_transport_message.empty()
            ? "onion peers are dialed through SOCKS5; no clearnet fallback"
            : status.onion_transport_message)
        : (status.onion_transport_message.empty() ? "disabled" : status.onion_transport_message);
    result["onion_transport"] = onion_transport;

    std::string warning;
    if (status.network_active && status.listening &&
        status.inbound == 0 && status.advertised_addresses.empty()) {
        warning = "No inbound peer or advertised public address observed yet; outbound P2P still works";
    }
    result["warnings"] = warning;
    result["rpc_schema"] = "din.rpc.v1";

    return result;
}

/**
 * getnettotals - Returns network traffic statistics
 *
 * NEW: ctx.daemon->p2p->getNetworkStats()
 */
din::Json rpc_context_getnettotals(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    uint64_t total_bytes_recv = 0;
    uint64_t total_bytes_sent = 0;

    auto p2p = GetP2PService(ctx);
    if (p2p) {
        const auto totals = p2p->GetNetworkTotals();
        total_bytes_recv = totals.bytes_recv;
        total_bytes_sent = totals.bytes_sent;
    }

    result["totalbytesrecv"] = static_cast<Json::UInt64>(total_bytes_recv);
    result["totalbytessent"] = static_cast<Json::UInt64>(total_bytes_sent);
    result["timemillis"] = static_cast<uint64_t>(timestamp * 1000);

    din::Json uploadtarget;
    uploadtarget["timeframe"] = 86400;  // 24 hours
    uploadtarget["target"] = 0;  // No limit
    uploadtarget["target_reached"] = false;
    uploadtarget["serve_historical_blocks"] = true;
    uploadtarget["bytes_left_in_cycle"] = 0;
    uploadtarget["time_left_in_cycle"] = 0;
    result["uploadtarget"] = uploadtarget;

    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

/**
 * ping - Requests that a ping be sent to all other nodes
 *
 * NEW: ctx.daemon->p2p->sendPingToAll()
 */
din::Json rpc_context_ping(const ExecutionContext& ctx, const din::Json& params) {
    auto p2p = GetP2PService(ctx);
    if (!p2p) {
        din::Json result;
        result["error"]["code"] = -32000;
        result["error"]["message"] = "P2P service not available";
        return result;
    }

    din::Json result;
    result["pinged_peers"] = static_cast<Json::UInt64>(p2p->SendPingToAll());
    return result;
}

/**
 * addnode - Attempts to add or remove a node from the addnode list
 *
 * NEW: ctx.daemon->p2p->addNode() / removeNode()
 */
din::Json rpc_context_addnode(const ExecutionContext& ctx, const din::Json& params) {
    if (!params.isArray() || params.size() < 2 ||
        !params[0].isString() || !params[1].isString()) {
        din::Json result;
        result["error"]["code"] = -8;
        result["error"]["message"] = "node address and command required";
        return result;
    }

    std::string node = params[0].as<std::string>();
    std::string command = params[1].as<std::string>();

    if (command != "add" && command != "remove" && command != "onetry") {
        din::Json result;
        result["error"]["code"] = -8;
        result["error"]["message"] = "command must be 'add', 'remove', or 'onetry'";
        return result;
    }

    auto p2p = GetP2PService(ctx);
    if (!p2p) {
        din::Json result;
        result["error"]["code"] = -32000;
        result["error"]["message"] = "P2P service not available";
        return result;
    }
    std::string host;
    uint16_t port = 20999;
    if (!ParseNodeEndpoint(node, &host, &port)) {
        din::Json result;
        result["error"]["code"] = -8;
        result["error"]["message"] = "Invalid node address";
        return result;
    }

    if (command == "onetry") {
        // Connect to peer immediately
        bool success = p2p->ConnectToPeer(host, port);
        din::Json result;
        result["success"] = success;
        if (!success) {
            result["message"] = "Connection attempt initiated (async)";
        }
        return result;
    }
    if (command == "add") {
        p2p->AddSeedNode(host, port);
        din::Json result;
        result["success"] = true;
        result["message"] = "Node added to seed list for this runtime";
        return result;
    }

    if (command == "remove") {
        const bool removed = p2p->RemoveSeedNode(host, port);
        din::Json result;
        result["success"] = removed;
        result["removed"] = removed;
        if (!removed) {
            result["message"] = "Node was not present in the runtime seed list";
        }
        return result;
    }

    return din::null();
}

/**
 * disconnectnode - Immediately disconnects from the specified peer node
 *
 * NEW: ctx.daemon->p2p->disconnectPeer()
 */
din::Json rpc_context_disconnectnode(const ExecutionContext& ctx, const din::Json& params) {
    if (!params.isArray() || params.size() < 1) {
        din::Json result;
        result["error"]["code"] = -8;
        result["error"]["message"] = "node address or nodeid required";
        return result;
    }

    auto p2p = GetP2PService(ctx);
    if (!p2p) {
        din::Json result;
        result["error"]["code"] = -32000;
        result["error"]["message"] = "P2P service not available";
        return result;
    }

    if (params[0].isString()) {
        p2p->DisconnectPeer(params[0].as<std::string>());
        return din::null();
    }

    din::Json result;
    result["error"]["code"] = -32601;
    result["error"]["message"] = "disconnectnode by numeric node ID is not supported";
    return result;
}

/**
 * getaddednodeinfo - Returns information about the given added node
 *
 * NEW: ctx.daemon->p2p->getAddedNodes()
 */
din::Json rpc_context_getaddednodeinfo(const ExecutionContext& ctx, const din::Json& params) {
    auto p2p = GetP2PService(ctx);
    if (!p2p) {
        din::Json result;
        result["error"]["code"] = -32000;
        result["error"]["message"] = "P2P service not available";
        return result;
    }

    std::string requested;
    if (params.isArray() && !params.empty() && params[0].isString()) {
        requested = params[0].as<std::string>();
    }

    const auto added_nodes = p2p->GetAddedNodes();
    const auto connected_peers = p2p->GetConnectedPeers();
    din::Json result(Json::arrayValue);

    for (const auto& [host, port] : added_nodes) {
        const std::string endpoint = host + ":" + std::to_string(port);
        if (!requested.empty() && endpoint != requested) {
            continue;
        }

        din::Json entry;
        entry["addednode"] = endpoint;

        din::Json addresses(Json::arrayValue);
        bool connected = false;
        for (const auto& peer : connected_peers) {
            if (peer.address == host && peer.port == port && peer.is_connected) {
                connected = true;
                din::Json addr_entry;
                addr_entry["address"] = endpoint;
                addr_entry["connected"] = "outbound";
                addresses.append(addr_entry);
            }
        }

        entry["connected"] = connected;
        entry["addresses"] = addresses;
        result.append(entry);
    }

    return result;
}

/**
 * setnetworkactive - Disable/enable all p2p network activity
 *
 * NEW: ctx.daemon->p2p->setNetworkActive()
 */
din::Json rpc_context_setnetworkactive(const ExecutionContext& ctx, const din::Json& params) {
    if (!params.isArray() || params.size() < 1 || !params[0].isBool()) {
        din::Json result;
        result["error"]["code"] = -8;
        result["error"]["message"] = "state (boolean) required";
        return result;
    }

    bool state = params[0].asBool();

    auto p2p = GetP2PService(ctx);
    if (p2p) {
        din::Json result;
        result["requested_state"] = state;
        result["applied"] = p2p->SetNetworkActive(state);
        result["networkactive"] = p2p->IsNetworkActive();
        return result;
    }

    din::Json result;
    result["error"]["code"] = -32000;
    result["error"]["message"] = "P2P service not available";
    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

/**
 * Register context-aware network methods (Week 2)
 *
 * These methods replace the legacy versions and use DaemonContext
 * instead of global variables for service access.
 */
void registerNetworkMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    // Note: Using RegisterMode::Overwrite to replace legacy handlers

    g_rpcRegistry.registerHandler("getpeerinfo",
                                 rpc_context_getpeerinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("getconnectioncount",
                                 rpc_context_getconnectioncount,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("getnetworkinfo",
                                 rpc_context_getnetworkinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("network.info",
                                 rpc_context_getnetworkinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("getnettotals",
                                 rpc_context_getnettotals,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("ping",
                                 rpc_context_ping,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("addnode",
                                 rpc_context_addnode,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("disconnectnode",
                                 rpc_context_disconnectnode,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("getaddednodeinfo",
                                 rpc_context_getaddednodeinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("setnetworkactive",
                                 rpc_context_setnetworkactive,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 10 network context-aware methods");
}
