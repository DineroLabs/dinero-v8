#include "rpc/rpc_registry.h"
#include "rpc/peer_json_utils.h"
#include "daemon/daemon_context.h"
#include "daemon/services/p2p_service.h"
#include "din_json.h"
#include <chrono>

namespace dinero {
namespace rpc {

/**
 * getpeerinfo - Returns information about connected peers
 */
din::Json rpc_getpeerinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::Json::array();

    if (ctx.daemon && ctx.daemon->p2p) {
        auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
        if (p2p) {
            for (const auto& peer_info : p2p->GetConnectedPeers()) {
                result.append(dinero::rpc::BuildPeerInfoJson(peer_info));
            }
            return result;
        }
    }

    return result;
}

/**
 * getconnectioncount - Returns the number of connections to other nodes
 */
din::Json rpc_getconnectioncount(const ExecutionContext& ctx, const din::Json& params) {
    if (ctx.daemon && ctx.daemon->p2p) {
        auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
        if (p2p) {
            return static_cast<int>(p2p->GetPeerCount());
        }
    }
    return 0;
}

/**
 * getnettotals - Returns network traffic statistics
 */
din::Json rpc_getnettotals(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    
    result["totalbytesrecv"] = 12345678;
    result["totalbytessent"] = 8765432;
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
 */
din::Json rpc_ping(const ExecutionContext& ctx, const din::Json& params) {
    // TODO: Send ping to all connected peers
    return din::null();
}

/**
 * addnode - Attempts to add or remove a node from the addnode list
 */
din::Json rpc_addnode(const ExecutionContext& ctx, const din::Json& params) {
    if (!params.isArray() || params.size() < 2 || 
        !params[0].isString() || !params[1].isString()) {
        din::Json error;
        error["code"] = -8;
        error["message"] = "node address and command required";
        throw std::runtime_error(din::dump(error));
    }
    
    std::string node = params[0];
    std::string command = params[1];
    
    if (command != "add" && command != "remove" && command != "onetry") {
        din::Json error;
        error["code"] = -8;
        error["message"] = "command must be 'add', 'remove', or 'onetry'";
        throw std::runtime_error(din::dump(error));
    }
    
    // TODO: Implement actual node management
    return din::null();
}

/**
 * disconnectnode - Immediately disconnects from the specified peer node
 */
din::Json rpc_disconnectnode(const ExecutionContext& ctx, const din::Json& params) {
    if (!params.isArray() || params.size() < 1) {
        din::Json error;
        error["code"] = -8;
        error["message"] = "node address or nodeid required";
        throw std::runtime_error(din::dump(error));
    }
    
    if (params[0].isString()) {
        std::string address = params[0];
        // TODO: Disconnect by address
    } else if (params[0].isNumeric()) {
        uint32_t nodeid = params[0];
        // TODO: Disconnect by node ID
    } else {
        din::Json error;
        error["code"] = -8;
        error["message"] = "invalid node identifier";
        throw std::runtime_error(din::dump(error));
    }
    
    return din::null();
}

/**
 * getaddednodeinfo - Returns information about the given added node
 */
din::Json rpc_getaddednodeinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::Json::array();
    
    std::string node;
    if (params.isArray() && params.size() > 0 && params[0].isString()) {
        node = params[0];
    }
    
    // TODO: Get actual added node information
    // For now, return empty array
    return result;
}

/**
 * setnetworkactive - Disable/enable all p2p network activity
 */
din::Json rpc_setnetworkactive(const ExecutionContext& ctx, const din::Json& params) {
    if (!params.isArray() || params.size() < 1 || !params[0].isBool()) {
        din::Json error;
        error["code"] = -8;
        error["message"] = "state (boolean) required";
        throw std::runtime_error(din::dump(error));
    }
    
    bool state = params[0];
    
    // TODO: Enable/disable network activity
    return state;
}

/**
 * Register network RPC methods
 */
void registerNetworkMethods() {
    // Peer information
    RpcMethodMeta peerInfoMeta;
    peerInfoMeta.name = "getpeerinfo";
    peerInfoMeta.ns = "network";
    peerInfoMeta.description = "Returns information about connected peers";
    peerInfoMeta.result.type = "array";
    peerInfoMeta.result.desc = "Array of peer information objects";
    
    g_rpcRegistry.registerHandler("getpeerinfo", rpc_getpeerinfo, peerInfoMeta, "network");
    
    // Connection count
    RpcMethodMeta connCountMeta;
    connCountMeta.name = "getconnectioncount";
    connCountMeta.ns = "network";
    connCountMeta.description = "Returns the number of connections to other nodes";
    connCountMeta.result.type = "number";
    connCountMeta.result.desc = "Number of active connections";
    
    g_rpcRegistry.registerHandler("getconnectioncount", rpc_getconnectioncount, connCountMeta, "network");
    
    // Network totals
    RpcMethodMeta netTotalsMeta;
    netTotalsMeta.name = "getnettotals";
    netTotalsMeta.ns = "network";
    netTotalsMeta.description = "Returns network traffic statistics";
    netTotalsMeta.result.type = "object";
    netTotalsMeta.result.desc = "Network traffic and upload target information";
    
    g_rpcRegistry.registerHandler("getnettotals", rpc_getnettotals, netTotalsMeta, "network");
    
    // Ping
    RpcMethodMeta pingMeta;
    pingMeta.name = "ping";
    pingMeta.ns = "network";
    pingMeta.description = "Requests that a ping be sent to all other nodes";
    pingMeta.result.type = "null";
    pingMeta.result.desc = "No return value";
    
    g_rpcRegistry.registerHandler("ping", rpc_ping, pingMeta, "network");
    
    // Add node
    RpcMethodMeta addNodeMeta;
    addNodeMeta.name = "addnode";
    addNodeMeta.ns = "network";
    addNodeMeta.description = "Attempts to add or remove a node from the addnode list";
    
    RpcParamMeta nodeParam;
    nodeParam.name = "node";
    nodeParam.type = "string";
    nodeParam.desc = "Node address (host:port)";
    nodeParam.required = true;
    addNodeMeta.params.append(nodeParam);
    
    RpcParamMeta commandParam;
    commandParam.name = "command";
    commandParam.type = "string";
    commandParam.desc = "Command: 'add', 'remove', or 'onetry'";
    commandParam.required = true;
    addNodeMeta.params.append(commandParam);
    
    addNodeMeta.result.type = "null";
    addNodeMeta.result.desc = "No return value";
    
    g_rpcRegistry.registerHandler("addnode", rpc_addnode, addNodeMeta, "network");
    
    // Disconnect node
    RpcMethodMeta disconnectMeta;
    disconnectMeta.name = "disconnectnode";
    disconnectMeta.ns = "network";
    disconnectMeta.description = "Immediately disconnects from the specified peer node";
    
    RpcParamMeta nodeIdParam;
    nodeIdParam.name = "address_or_nodeid";
    nodeIdParam.type = "string|number";
    nodeIdParam.desc = "Node address or node ID";
    nodeIdParam.required = true;
    disconnectMeta.params.append(nodeIdParam);
    
    disconnectMeta.result.type = "null";
    disconnectMeta.result.desc = "No return value";
    
    g_rpcRegistry.registerHandler("disconnectnode", rpc_disconnectnode, disconnectMeta, "network");
    
    // Added node info
    RpcMethodMeta addedNodeMeta;
    addedNodeMeta.name = "getaddednodeinfo";
    addedNodeMeta.ns = "network";
    addedNodeMeta.description = "Returns information about the given added node";
    
    RpcParamMeta optionalNodeParam;
    optionalNodeParam.name = "node";
    optionalNodeParam.type = "string";
    optionalNodeParam.desc = "Node address (optional)";
    optionalNodeParam.required = false;
    addedNodeMeta.params.append(optionalNodeParam);
    
    addedNodeMeta.result.type = "array";
    addedNodeMeta.result.desc = "Array of added node information";
    
    g_rpcRegistry.registerHandler("getaddednodeinfo", rpc_getaddednodeinfo, addedNodeMeta, "network");
    
    // Network active
    RpcMethodMeta networkActiveMeta;
    networkActiveMeta.name = "setnetworkactive";
    networkActiveMeta.ns = "network";
    networkActiveMeta.description = "Disable/enable all p2p network activity";
    
    RpcParamMeta stateParam;
    stateParam.name = "state";
    stateParam.type = "boolean";
    stateParam.desc = "Network activity state";
    stateParam.required = true;
    networkActiveMeta.params.append(stateParam);
    
    networkActiveMeta.result.type = "boolean";
    networkActiveMeta.result.desc = "Current network activity state";
    
    g_rpcRegistry.registerHandler("setnetworkactive", rpc_setnetworkactive, networkActiveMeta, "network");
    
    std::cout << "[RPC] Registered network RPC methods" << std::endl;
}

} // namespace rpc
} // namespace dinero
