#pragma once

#include <json/json.h>

namespace dinero {
namespace rpc {

/**
 * RPC response helpers for JSON-RPC 2.0 compliance
 * Every response MUST have either "result" or "error", never both null
 */

// Success response with result
inline Json::Value RpcSuccess(Json::Value result = Json::Value(Json::objectValue)) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["result"] = result;
    response["error"] = Json::nullValue;
    return response;
}

// Error response
inline Json::Value RpcError(int code, const std::string& message) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["result"] = Json::nullValue;
    
    Json::Value error;
    error["code"] = code;
    error["message"] = message;
    response["error"] = error;
    
    return response;
}

// Common error codes
constexpr int RPC_INVALID_REQUEST = -32600;
constexpr int RPC_METHOD_NOT_FOUND = -32601;
constexpr int RPC_INVALID_PARAMS = -32602;
constexpr int RPC_INTERNAL_ERROR = -32603;
constexpr int RPC_PARSE_ERROR = -32700;

// Dinero-specific error codes
constexpr int RPC_FORBIDDEN = -32099;       // Admin-only method in read-only mode
constexpr int RPC_WALLET_ERROR = -4;
constexpr int RPC_WALLET_NOT_FOUND = -18;
constexpr int RPC_WALLET_LOCKED = -13;
constexpr int RPC_INVALID_ADDRESS = -5;
constexpr int RPC_INSUFFICIENT_FUNDS = -6;

} // namespace rpc
} // namespace dinero

