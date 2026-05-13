#include "node/rpc_executor.h"
#include "daemon/rpc_server.h"
#include "common/logger.h"
#include <sstream>
#include <regex>
#include <stdexcept>

namespace dinero {
namespace interfaces {

RpcExecutor::RpcExecutor(dinero::RPCServer* rpc_server)
    : m_rpc_server(rpc_server) {
    if (!m_rpc_server) {
        throw std::invalid_argument("RPC server cannot be null");
    }
}

std::string RpcExecutor::executeCommand(const std::string& command) {
    validateRpcServer();
    
    std::string method;
    Json::Value params;
    
    if (!parseCommand(command, method, params)) {
        Json::Value error = createErrorResponse(-32700, "Parse error: Invalid command format");
        return dump(error);
    }
    
    try {
        Json::Value result = executeJson(method, params);
        return dump(result);
    } catch (const std::exception& e) {
        Json::Value error = createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
        return dump(error);
    }
}

Json::Value RpcExecutor::executeJson(const std::string& method, const Json::Value& params) {
    validateRpcServer();
    
    try {
        // Call RPC method directly without HTTP overhead
        if (m_rpc_server->hasMethod(method)) {
            return m_rpc_server->callMethod(method, params);
        } else {
            return createErrorResponse(-32601, "Method not found: " + method);
        }
    } catch (const std::exception& e) {
        dinero::g_logger.error("In-process RPC error for method '" + method + "': " + e.what());
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

std::vector<Json::Value> RpcExecutor::executeBatch(
    const std::vector<std::pair<std::string, Json::Value>>& requests) {
    
    validateRpcServer();
    
    std::vector<Json::Value> results;
    results.reserve(requests.size());
    
    for (const auto& [method, params] : requests) {
        try {
            results.append(executeJson(method, params));
        } catch (const std::exception& e) {
            results.append(createErrorResponse(-32603, "Batch error: " + std::string(e.what())));
        }
    }
    
    return results;
}

bool RpcExecutor::parseCommand(const std::string& command, std::string& method, Json::Value& params) {
    // Simple command parsing - handles most common cases
    // Examples:
    //   "getblockcount" -> method="getblockcount", params=[]
    //   "getblockhash 100" -> method="getblockhash", params=[100]
    //   "getnewaddress \"\" bech32" -> method="getnewaddress", params=["", "bech32"]
    
    std::istringstream iss(command);
    std::string token;
    
    // Get method name
    if (!(iss >> method)) {
        return false;
    }
    
    // Parse parameters
    params = Json::Value(Json::arrayValue);
    std::string remaining;
    std::getline(iss, remaining);
    
    if (remaining.empty()) {
        return true; // No parameters
    }
    
    // Simple parameter parsing - split by spaces, handle quoted strings
    std::regex param_regex(R"(\"([^\"]*)\"|(\S+))");
    std::sregex_iterator iter(remaining.begin(), remaining.end(), param_regex);
    std::sregex_iterator end;
    
    for (; iter != end; ++iter) {
        std::string param = (*iter)[1].matched ? (*iter)[1].str() : (*iter)[2].str();
        
        if (param.empty()) continue;
        
        // Try to parse as number
        if (std::regex_match(param, std::regex(R"(-?\d+)"))) {
            params.append(std::stoi(param));
        } else if (std::regex_match(param, std::regex(R"(-?\d+\.\d+)"))) {
            params.append(std::stod(param));
        } else if (param == "true") {
            params.append(true);
        } else if (param == "false") {
            params.append(false);
        } else if (param == "null") {
            params.append(Json::Value::null);
        } else {
            params.append(param);
        }
    }
    
    return true;
}

// High-level convenience methods
RpcExecutor::WalletInfo RpcExecutor::getWalletInfo() {
    WalletInfo info;
    
    try {
        // Get wallet info without RPC parsing overhead
        Json::Value result = executeJson("getwalletinfo", Json::Value(Json::arrayValue));
        
        if (result.isObject() && !result.isMember("error")) {
            info.balance = result.isMember("balance") ? "balance" : 0.0;
            info.encrypted = result.isMember("encrypted") ? "encrypted" : false;
            info.unlocked = result.isMember("unlocked") ? "unlocked" : false;
            info.name = result.isMember("walletname") ? "walletname" : "";
        }
    } catch (const std::exception& e) {
        dinero::g_logger.warning("Failed to get wallet info: " + std::string(e.what()));
    }
    
    return info;
}

RpcExecutor::MiningInfo RpcExecutor::getMiningInfo() {
    MiningInfo info;
    
    try {
        Json::Value result = executeJson("getmininginfo", Json::Value(Json::arrayValue));
        
        if (result.isObject() && !result.isMember("error")) {
            info.generating = result.isMember("generate") ? "generate" : false;
            info.threads = result.isMember("genproclimit") ? "genproclimit" : 0;
            info.hashrate = result.isMember("networkhashps") ? "networkhashps" : 0.0;
            info.mining_address = result.isMember("miningaddress") ? "miningaddress" : "";
        }
    } catch (const std::exception& e) {
        dinero::g_logger.warning("Failed to get mining info: " + std::string(e.what()));
    }
    
    return info;
}

RpcExecutor::BlockchainInfo RpcExecutor::getBlockchainInfo() {
    BlockchainInfo info;
    
    try {
        Json::Value result = executeJson("getblockchaininfo", Json::Value(Json::arrayValue));
        
        if (result.isObject() && !result.isMember("error")) {
            info.blocks = result.isMember("blocks") ? "blocks" : 0;
            info.bestblockhash = result.isMember("bestblockhash") ? "bestblockhash" : "";
            info.difficulty = result.isMember("difficulty") ? "difficulty" : 0.0;
            info.chain = result.isMember("chain") ? "chain" : "";
            info.initialblockdownload = result.isMember("initialblockdownload") ? "initialblockdownload" : false;
        }
    } catch (const std::exception& e) {
        dinero::g_logger.warning("Failed to get blockchain info: " + std::string(e.what()));
    }
    
    return info;
}

std::string RpcExecutor::getNewAddress(const std::string& addressType) {
    try {
        Json::Value params(Json::arrayValue);
        params.append(""); // Empty label
        params.append(addressType);
        
        Json::Value result = executeJson("getnewaddress", params);
        
        if (result.isObject() && result.isMember("error")) {
            throw std::runtime_error(result["error"]["message"].asString());
        }
        
        if (result.isString()) {
            return result.asString();
        } else if (result.isObject() && result.isMember("address")) {
            return result["address"].asString();
        }
        
        throw std::runtime_error("Unexpected response format");
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to get new address: " + std::string(e.what()));
        throw;
    }
}

bool RpcExecutor::validateAddress(const std::string& address) {
    try {
        Json::Value params(Json::arrayValue);
        params.append(address);
        
        Json::Value result = executeJson("validateaddress", params);
        
        if (result.isObject() && !result.isMember("error")) {
            return result.isMember("isvalid") ? "isvalid" : false;
        }
        
        return false;
    } catch (const std::exception& e) {
        dinero::g_logger.warning("Failed to validate address: " + std::string(e.what()));
        return false;
    }
}

// Helper methods
Json::Value RpcExecutor::createErrorResponse(int code, const std::string& message) {
    Json::Value response;
    Json::Value error;
    error["code"] = code;
    error["message"] = message;
    response["error"] = error;
    response["result"] = Json::Value::null;
    response["id"] = Json::Value::null;
    return response;
}

void RpcExecutor::validateRpcServer() {
    if (!m_rpc_server) {
        throw std::runtime_error("RPC server not available");
    }
}

} // namespace interfaces
} // namespace dinero
