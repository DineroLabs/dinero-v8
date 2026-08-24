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
#include "daemon/block_relay_manager.h"  // Task 5: BlocksServed24h()
#include "network/quic_transport.h"      // Stage B: surface QUIC relay-transport readiness
#include "consensus/chainparams.h"
#include "common/logger.h"
#include <memory>
#include <atomic>
#include <vector>
#include <string>
#include <cstdint>

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

din::Json StringArrayJson(const std::vector<std::string>& values) {
    din::Json array(Json::arrayValue);
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
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
        din::Json onion_service;
        onion_service["requested"] = false;
        onion_service["active"] = false;
        onion_service["address"] = "";
        onion_service["authentication"] = "";
        onion_service["message"] = "P2P service not available";
        result["onion_service"] = onion_service;
        din::Json dynamic_p2p;
        dynamic_p2p["enabled"] = false;
        dynamic_p2p["mode"] = "unavailable";
        dynamic_p2p["message"] = "P2P service not available";
        result["dynamic_p2p"] = dynamic_p2p;
        return result;
    }

    const auto status = p2p->GetNetworkStatus();
    networks.clear();
    ipv4["reachable"] = status.network_active;
    networks.append(ipv4);
    const bool onion_reachable = status.onion_transport_reachable ||
                                 status.onion_service_active;
    onion["limited"] = !onion_reachable;
    onion["reachable"] = onion_reachable;
    onion["proxy"] = status.onion_proxy;
    networks.append(onion);
    i2p["limited"] = true;
    i2p["reachable"] = false;
    i2p["proxy"] = "";
    networks.append(i2p);
    result["networks"] = networks;

    din::Json onion_service;
    onion_service["requested"] = status.onion_service_requested;
    onion_service["active"] = status.onion_service_active;
    onion_service["address"] = status.onion_service_address;
    onion_service["authentication"] = status.onion_service_authentication;
    onion_service["message"] = status.onion_service_message;
    result["onion_service"] = onion_service;

    result["networkactive"] = status.network_active;
    result["localrelay"] = status.local_relay;
    result["relay_mode"] = status.relay_mode;
    result["connections"] = static_cast<Json::UInt64>(status.connections);
    result["connections_in"] = static_cast<Json::UInt64>(status.inbound);
    result["connections_out"] = static_cast<Json::UInt64>(status.outbound);
    result["listen"] = status.listening;
    result["listen_port"] = static_cast<int>(status.listen_port);
    const bool direct_inbound_observed = status.inbound > 0;
    // Gap 2 fix: only an EXPLICIT/confirmed reachable advertisement (operator
    // externalip or port-mapping) proves direct reachability — a Gap-1-LEARNED
    // address is just the IP peers see us from (the NAT gateway's public IP for
    // a NAT'd node), which isn't actually dialable. Using "any advertised
    // address" here would let a learned, dead address wrongly suppress the relay
    // fallback for genuinely NAT'd nodes.
    const bool direct_advertised = status.has_explicit_advertised;
    // Same predicate the relay auto-register gate uses (IsDirectlyReachable in
    // p2p_manager.h) — keep these two call sites identical so the relay-fallback
    // report can never disagree with the registration behavior it gates.
    const bool direct_reachable = IsDirectlyReachable(
        direct_inbound_observed, direct_advertised, status.port_mapping_active);
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
    // Phase 2b: 24h throughput counters — real inputs for the dashboard's
    // Decentralization Score (replaces the 5min-extrapolation and
    // zero-placeholder from Phase 2a).
    if (ctx.daemon && ctx.daemon->block_relay) {
        relay["blocks_served_24h"] = static_cast<Json::UInt64>(
            ctx.daemon->block_relay->BlocksServed24h());
    } else {
        relay["blocks_served_24h"] = Json::UInt64(0);
    }
    relay["bytes_relayed_24h"] = static_cast<Json::UInt64>(
        p2p->get().BytesRelayed24h());
    result["relay"] = relay;

    // Stage B: surface the QUIC relay-transport readiness so operators can
    // confirm in the field whether encrypted relay DATA can actually flow on
    // this binary (registration rides TCP and works regardless; relay data on
    // mainnet requires the QUIC crypto bridge). Without this there is no
    // runtime signal of crypto_backend / mainnet_relay_ready.
    {
        const auto quic = dinero::network::QuicTransport::CompileInfo();
        din::Json q;
        q["ngtcp2_available"] = quic.ngtcp2_available;
        q["crypto_available"] = quic.crypto_available;
        q["mainnet_relay_ready"] = quic.mainnet_relay_ready;
        q["crypto_backend"] = quic.crypto_backend;
        q["ngtcp2_version"] = quic.ngtcp2_version;
        q["openssl_version"] = quic.openssl_version;
        q["disabled_reason"] = quic.disabled_reason;
        // Encrypted relay data can flow only when ngtcp2 + a crypto bridge are
        // compiled in AND the mainnet gate is open.
        q["relay_data_ready"] =
            quic.ngtcp2_available && quic.crypto_available && quic.mainnet_relay_ready;
        result["quic_transport"] = q;
    }

    din::Json dynamic_p2p;
    dynamic_p2p["enabled"] = status.dynamic_p2p_enabled;
    dynamic_p2p["mode"] = status.dynamic_p2p_mode;
    dynamic_p2p["message"] = status.dynamic_p2p_enabled
        ? "Dynamic P2P governor is active in conservative slow-churn mode"
        : "Dynamic P2P governor is not active; these are read-only baseline metrics";

    din::Json dynamic_addrman;
    dynamic_addrman["available"] = status.addrman.available;
    dynamic_addrman["total"] = static_cast<Json::UInt64>(status.addrman.total_addresses);
    dynamic_addrman["new"] = static_cast<Json::UInt64>(status.addrman.new_addresses);
    dynamic_addrman["tried"] = static_cast<Json::UInt64>(status.addrman.tried_addresses);
    dynamic_addrman["terrible"] = static_cast<Json::UInt64>(status.addrman.terrible_addresses);
    dynamic_addrman["banned"] = static_cast<Json::UInt64>(status.addrman.banned_addresses);
    dynamic_addrman["relay_candidates"] =
        static_cast<Json::UInt64>(status.addrman.relay_candidates);
    dynamic_addrman["avg_success_rate"] = status.addrman.avg_success_rate;
    dynamic_p2p["addrman"] = dynamic_addrman;

    din::Json dynamic_peers;
    dynamic_peers["connections"] = static_cast<Json::UInt64>(status.connections);
    dynamic_peers["inbound"] = static_cast<Json::UInt64>(status.inbound);
    dynamic_peers["outbound"] = static_cast<Json::UInt64>(status.outbound);
    dynamic_peers["configured_seed_peers"] =
        static_cast<Json::UInt64>(status.configured_seed_peers);
    dynamic_peers["configured_seed_connections"] =
        static_cast<Json::UInt64>(status.configured_seed_connections);
    dynamic_peers["non_configured_seed_connections"] =
        static_cast<Json::UInt64>(status.discovered_connections);
    dynamic_peers["relay_peer_connections"] =
        static_cast<Json::UInt64>(status.relay_peer_connections);
    dynamic_p2p["peers"] = dynamic_peers;

    din::Json dynamic_reachability;
    dynamic_reachability["direct_reachable"] = direct_reachable;
    dynamic_reachability["direct_inbound_observed"] = direct_inbound_observed;
    dynamic_reachability["relay_fallback_eligible"] = relay_fallback_eligible;
    dynamic_reachability["relay_local"] = status.local_relay;
    dynamic_reachability["relay_active"] = status.relay_active;
    dynamic_reachability["relay_directory_entries"] =
        static_cast<Json::UInt64>(status.relay_directory_entries);
    dynamic_reachability["relay_hint_targets_observed"] =
        static_cast<Json::UInt64>(status.relay_hints_received_self +
                                  status.relay_hints_received_relay);
    dynamic_p2p["reachability"] = dynamic_reachability;

    din::Json dynamic_governor;
    dynamic_governor["available"] = status.dynamic_p2p_governor.available;
    dynamic_governor["mode"] = status.dynamic_p2p_governor.mode;
    dynamic_governor["candidate_source"] =
        status.dynamic_p2p_governor.candidate_source;
    dynamic_governor["connected_outbound"] =
        static_cast<Json::UInt64>(status.dynamic_p2p_governor.connected_outbound);
    dynamic_governor["configured_seed_hot"] =
        static_cast<Json::UInt64>(status.dynamic_p2p_governor.configured_seed_hot);
    dynamic_governor["relay_capable_seen"] =
        static_cast<Json::UInt64>(status.dynamic_p2p_governor.relay_capable_seen);
    dynamic_governor["recommended_hot_peers"] =
        StringArrayJson(status.dynamic_p2p_governor.hot_peers);
    dynamic_governor["warm_candidates"] =
        StringArrayJson(status.dynamic_p2p_governor.warm_candidates);
    dynamic_governor["relay_registration_candidates"] =
        StringArrayJson(status.dynamic_p2p_governor.relay_registration_candidates);
    dynamic_governor["demote_candidates"] =
        StringArrayJson(status.dynamic_p2p_governor.demote_candidates);
    dynamic_p2p["governor"] = dynamic_governor;

    result["dynamic_p2p"] = dynamic_p2p;

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
    portmap["upnp_compiled"] = status.port_mapping_upnp_compiled;
    portmap["natpmp_compiled"] = status.port_mapping_natpmp_compiled;
    portmap["attempts"] = static_cast<Json::UInt64>(status.port_mapping_attempts);
    portmap["renewals"] = static_cast<Json::UInt64>(status.port_mapping_renewals);
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

    // Local node identity (20-byte node_id, 40-char lowercase hex).
    // Empty string if node_identity_ has not yet been initialized.
    result["node_id_hex"] = p2p
        ? p2p->get().get_local_node_id_hex()
        : std::string();

    std::string warning;
    // Reuse the single reachability predicate (direct_reachable) rather than the
    // pre-Gap1 `advertised_addresses.empty()` check — otherwise a NAT'd node with
    // only a Gap-1-LEARNED (dead) address would suppress this warning despite not
    // being directly reachable.
    if (status.network_active && status.listening && !direct_reachable) {
        warning = "No confirmed direct reachability (no inbound peer, explicit advertised "
                  "address, or active port mapping) yet; outbound P2P still works";
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

// node.status — one operator-facing health summary answering the questions an
// operator actually asks right after install: am I synced? do I have peers? am
// I directly reachable or behind NAT? am I registered with relays? can someone
// reach me through a relay? Reuses getnetworkinfo (so the reachability/relay
// predicates keep ONE source of truth — see IsDirectlyReachable) and
// getsynchealth, then distills them into clear booleans + a one-line summary.
extern din::Json rpc_context_getsynchealth(const ExecutionContext& ctx,
                                           const din::Json& params);

din::Json rpc_context_nodestatus(const ExecutionContext& ctx, const din::Json& params) {
    const din::Json net = rpc_context_getnetworkinfo(ctx, params);
    const din::Json sync = rpc_context_getsynchealth(ctx, params);

    din::Json result;

    // ── sync ──
    const bool sync_ok = !sync.isMember("error");
    const bool in_ibd =
        sync.isMember("initialblockdownload") && sync["initialblockdownload"].asBool();
    const bool synced = sync_ok && !in_ibd;
    const uint64_t height =
        sync.isMember("active_height")
            ? static_cast<uint64_t>(sync["active_height"].asInt())
            : 0;
    din::Json sync_obj;
    sync_obj["synced"] = synced;
    sync_obj["height"] = static_cast<Json::UInt64>(height);
    sync_obj["initial_block_download"] = in_ibd;
    if (sync.isMember("chain")) {
        sync_obj["chain"] = sync["chain"].asString();
    }
    result["sync"] = sync_obj;

    // ── peers ──
    const uint64_t cin =
        net.isMember("connections_in") ? net["connections_in"].asUInt64() : 0;
    const uint64_t cout =
        net.isMember("connections_out") ? net["connections_out"].asUInt64() : 0;
    din::Json peers;
    peers["in"] = static_cast<Json::UInt64>(cin);
    peers["out"] = static_cast<Json::UInt64>(cout);
    peers["total"] = static_cast<Json::UInt64>(cin + cout);
    result["peers"] = peers;

    // ── reachability (reuse getnetworkinfo's computed predicates) ──
    const bool direct =
        net.isMember("direct_reachable") && net["direct_reachable"].asBool();
    const bool eligible =
        net.isMember("relay_fallback_eligible") &&
        net["relay_fallback_eligible"].asBool();
    uint64_t registered_relays = 0;
    if (net.isMember("dynamic_p2p") &&
        net["dynamic_p2p"].isMember("peers") &&
        net["dynamic_p2p"]["peers"].isMember("relay_peer_connections")) {
        registered_relays =
            net["dynamic_p2p"]["peers"]["relay_peer_connections"].asUInt64();
    }
    bool relay_data_ready = false;
    if (net.isMember("quic_transport") &&
        net["quic_transport"].isMember("relay_data_ready")) {
        relay_data_ready = net["quic_transport"]["relay_data_ready"].asBool();
    }
    const bool behind_nat = !direct && eligible;
    const bool reachable_via_relay = relay_data_ready && registered_relays > 0;
    din::Json reach;
    reach["direct_reachable"] = direct;
    reach["behind_nat"] = behind_nat;
    reach["relay_fallback_eligible"] = eligible;
    reach["registered_relays"] = static_cast<Json::UInt64>(registered_relays);
    reach["relay_data_ready"] = relay_data_ready;
    reach["reachable_via_relay"] = reachable_via_relay;
    result["reachability"] = reach;

    // ── overall verdict + one-line operator summary ──
    const bool reachable_somehow = direct || reachable_via_relay;
    const bool ok = synced && (cin + cout) > 0 && reachable_somehow;
    result["ok"] = ok;

    std::string summary = synced ? "synced" : "syncing";
    summary += " @ height " + std::to_string(height);
    summary += "; " + std::to_string(cin + cout) + " peers (" +
               std::to_string(cin) + " in / " + std::to_string(cout) + " out)";
    if (direct) {
        summary += "; directly reachable";
    } else if (reachable_via_relay) {
        summary += "; behind NAT, reachable via relay (" +
                   std::to_string(registered_relays) + " relays)";
    } else if (eligible) {
        summary += "; behind NAT, relay-eligible but NOT yet relay-reachable (" +
                   std::to_string(registered_relays) + " relays, data_ready=" +
                   std::string(relay_data_ready ? "yes" : "no") + ")";
    } else {
        summary += "; reachability not yet determined";
    }
    result["summary"] = summary;
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

    // Operator one-shot health summary (install/dashboard/CLI).
    g_rpcRegistry.registerHandler("node.status",
                                 rpc_context_nodestatus,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerHandler("node.health",
                                 rpc_context_nodestatus,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 12 network context-aware methods");
}
