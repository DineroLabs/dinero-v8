#include "common/rpc_client.h"
#include <curl/curl.h>
#include "compat/jsoncpp_compat.h"
#include <sstream>
#include <iostream>

namespace Dinero {
namespace Common {

// Write callback for CURL
size_t RPCClient::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

RPCClient::RPCClient(const RPCConfig& config) : config(config) {
    // Initialize CURL globally if not already done
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
}

RPCClient::~RPCClient() {
    // Note: We don't call curl_global_cleanup() here as other instances might still be using CURL
}

void RPCClient::set_config(const RPCConfig& config) {
    this->config = config;
}

RPCConfig RPCClient::get_config() const {
    return config;
}

std::string RPCClient::make_request(const std::string& method, const Json::Value& params) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (curl) {
        // Prepare JSON request
        Json::Value request;
        request["jsonrpc"] = "2.0";
        request["id"] = "dinero_client";
        request["method"] = method;
        request["params"] = params;
        
        Json::FastWriter writer;
        std::string post_data = writer.write(request);
        
        // Set up headers
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        // Configure CURL
        curl_easy_setopt(curl, CURLOPT_URL, config.url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_USERNAME, config.user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, config.pass.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, config.timeout);
        
        if (config.verbose) {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }
        
        // Perform request
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            last_error = "RPC request failed: " + std::string(curl_easy_strerror(res));
            return "";
        }
    }
    
    return response;
}

Json::Value RPCClient::call(const std::string& method, const Json::Value& params) {
    std::string response = make_request(method, params);
    
    if (response.empty()) {
        Json::Value error;
        error["error"] = last_error;
        return error;
    }
    
    Json::Value result;
    Json::Reader reader;
    if (!reader.parse(response, result)) {
        last_error = "Failed to parse RPC response";
        Json::Value error;
        error["error"] = last_error;
        return error;
    }
    
    return result;
}

Json::Value RPCClient::getblockchaininfo() {
    return call("getblockchaininfo");
}

Json::Value RPCClient::getbalance(const std::string& account) {
    Json::Value params;
    if (!account.empty()) {
        params.append(account);
    }
    return call("getbalance", params);
}

Json::Value RPCClient::getnewaddress(const std::string& account) {
    Json::Value params;
    if (!account.empty()) {
        params.append(account);
    }
    return call("getnewaddress", params);
}

Json::Value RPCClient::sendtoaddress(const std::string& address, double amount) {
    Json::Value params;
    params.append(address);
    params.append(amount);
    return call("sendtoaddress", params);
}

Json::Value RPCClient::getmininginfo() {
    return call("getmininginfo");
}

Json::Value RPCClient::getblock(const std::string& block_hash) {
    Json::Value params;
    params.append(block_hash);
    return call("getblock", params);
}

Json::Value RPCClient::getblockhash(int height) {
    Json::Value params;
    params.append(height);
    return call("getblockhash", params);
}

Json::Value RPCClient::getdifficulty() {
    return call("getdifficulty");
}

Json::Value RPCClient::getconnectioncount() {
    return call("getconnectioncount");
}

Json::Value RPCClient::getblocktemplate(const std::string& address) {
    Json::Value params;
    if (!address.empty()) {
        params.append(address);
    }
    return call("getblocktemplate", params);
}

Json::Value RPCClient::submitblock(const std::string& block_data) {
    Json::Value params;
    params.append(block_data);
    return call("submitblock", params);
}

bool RPCClient::is_connected() {
    Json::Value response = getblockchaininfo();
    return !response.isMember("error") || response["error"].isNull();
}

std::string RPCClient::get_last_error() const {
    return last_error;
}

// Utility functions
std::string json_to_string(const Json::Value& json, bool pretty) {
    if (pretty) {
        Json::StyledWriter writer;
        return writer.write(json);
    } else {
        Json::FastWriter writer;
        return writer.write(json);
    }
}

Json::Value string_to_json(const std::string& str) {
    Json::Value result;
    Json::Reader reader;
    if (!reader.parse(str, result)) {
        result["error"] = "Failed to parse JSON string";
    }
    return result;
}

bool is_valid_json(const std::string& str) {
    Json::Value result;
    Json::Reader reader;
    return reader.parse(str, result);
}

} // namespace Common
} // namespace Dinero 
