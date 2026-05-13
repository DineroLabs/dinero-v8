#ifndef DINERO_RPC_CLIENT_H
#define DINERO_RPC_CLIENT_H

#include <string>
#include <vector>
#include "compat/jsoncpp_compat.h"

namespace Dinero {
namespace Common {

// RPC client configuration
struct RPCConfig {
    std::string url = "http://127.0.0.1:8332";
    std::string user = "dinero_Dinero_USB_1754372740";
    std::string pass = "5431cfe6e2b435637ec89dc2a85324c3";
    int timeout = 30;
    bool verbose = false;
};

// RPC client for communicating with Dinero daemon
class RPCClient {
private:
    RPCConfig config;
    
    // Internal helper functions
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp);
    std::string make_request(const std::string& method, const Json::Value& params = Json::Value());
    
public:
    RPCClient(const RPCConfig& config = RPCConfig{});
    ~RPCClient();
    
    // Configuration
    void set_config(const RPCConfig& config);
    RPCConfig get_config() const;
    
    // RPC methods
    Json::Value getblockchaininfo();
    Json::Value getbalance(const std::string& account = "");
    Json::Value getnewaddress(const std::string& account = "");
    Json::Value sendtoaddress(const std::string& address, double amount);
    Json::Value getmininginfo();
    Json::Value getblock(const std::string& block_hash);
    Json::Value getblockhash(int height);
    Json::Value getdifficulty();
    Json::Value getconnectioncount();
    Json::Value getblocktemplate(const std::string& address = "");
    Json::Value submitblock(const std::string& block_data);
    
    // Generic RPC call
    Json::Value call(const std::string& method, const Json::Value& params = Json::Value());
    
    // Utility methods
    bool is_connected();
    std::string get_last_error() const;
    
private:
    std::string last_error;
};

// Utility functions
std::string json_to_string(const Json::Value& json, bool pretty = true);
Json::Value string_to_json(const std::string& str);
bool is_valid_json(const std::string& str);

} // namespace Common
} // namespace Dinero

#endif // DINERO_RPC_CLIENT_H 