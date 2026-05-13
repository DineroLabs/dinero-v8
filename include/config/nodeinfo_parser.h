#pragma once

#include "compat/jsoncpp_compat.h"
#include <string>
#include <vector>
#include <optional>

namespace dinero {

// TLS configuration modes
enum class TlsMode {
    OFF,
    LOOPBACK_ONLY,
    PUBLIC
};

// Node configuration from nodeinfo.json v1
struct NodeInfoConfig {
    // Schema version
    std::string schema_version{"din.nodeinfo.v1"};
    
    // Network configuration
    std::string network{"mainnet"};  // mainnet, testnet, regtest
    std::string data_dir;
    
    // RPC configuration
    struct RpcConfig {
        std::string bind_address{"127.0.0.1"};
        int port{20998};
        bool enabled{true};
        std::string cookie_file;
        int max_connections{100};
        int request_timeout_seconds{30};
    } rpc;
    
    // HTTP configuration
    struct HttpConfig {
        std::string bind_address{"127.0.0.1"};
        int port{20998};
        bool enabled{true};
        int threads{4};
        bool health_endpoint{true};
        bool metrics_endpoint{true};
        size_t max_request_size{1048576}; // 1MB
    } http;
    
    // TLS configuration
    struct TlsConfig {
        TlsMode mode{TlsMode::LOOPBACK_ONLY};
        std::string cert_file;
        std::string key_file;
        std::string ca_file;
        bool require_client_cert{false};
        std::vector<std::string> allowed_fingerprints;
    } tls;
    
    // Logging configuration
    struct LoggingConfig {
        std::string level{"INFO"};
        bool structured{true};
        bool console_output{true};
        bool file_output{false};
        std::string log_file;
        bool trace_rpc{false};
        bool trace_http{false};
    } logging;
    
    // Mining configuration
    struct MiningConfig {
        bool enabled{false};
        std::string mining_address;
        int threads{1};
        bool auto_generate_address{true};
    } mining;
    
    // P2P configuration
    struct P2pConfig {
        std::string bind_address{"0.0.0.0"};
        int port{20999};
        bool enabled{true};
        int max_connections{125};
        std::vector<std::string> seed_nodes;
        bool discover_peers{true};
    } p2p;
    
    // Wallet configuration
    struct WalletConfig {
        bool enabled{true};
        std::string default_wallet{"default"};
        bool auto_create_default{true};
        int keypool_size{1000};
    } wallet;
    
    // Security configuration
    struct SecurityConfig {
        bool cookie_rotation{true};
        int cookie_rotation_minutes{60};
        int request_rate_limit{1000};
        bool dos_protection{true};
        std::vector<std::string> allowed_ips;
    } security;
};

// Parser for nodeinfo.json v1 schema
class NodeInfoParser {
public:
    // Parse from file
    static std::optional<NodeInfoConfig> parseFromFile(const std::string& filename);
    
    // Parse from JSON string
    static std::optional<NodeInfoConfig> parseFromString(const std::string& json_str);
    
    // Parse from Json::Value
    static std::optional<NodeInfoConfig> parseFromJson(const Json::Value& json);
    
    // Generate default configuration
    static NodeInfoConfig generateDefault(const std::string& network = "mainnet");
    
    // Write configuration to file
    static bool writeToFile(const NodeInfoConfig& config, const std::string& filename);
    
    // Convert configuration to JSON
    static Json::Value configToJson(const NodeInfoConfig& config);
    
    // Validate configuration
    static bool validateConfig(const NodeInfoConfig& config, std::string& error_message);
    
private:
    // Helper parsing methods
    static void parseRpcConfig(const Json::Value& json, NodeInfoConfig::RpcConfig& rpc);
    static void parseHttpConfig(const Json::Value& json, NodeInfoConfig::HttpConfig& http);
    static void parseTlsConfig(const Json::Value& json, NodeInfoConfig::TlsConfig& tls);
    static void parseLoggingConfig(const Json::Value& json, NodeInfoConfig::LoggingConfig& logging);
    static void parseMiningConfig(const Json::Value& json, NodeInfoConfig::MiningConfig& mining);
    static void parseP2pConfig(const Json::Value& json, NodeInfoConfig::P2pConfig& p2p);
    static void parseWalletConfig(const Json::Value& json, NodeInfoConfig::WalletConfig& wallet);
    static void parseSecurityConfig(const Json::Value& json, NodeInfoConfig::SecurityConfig& security);
    
    // Helper serialization methods
    static Json::Value rpcConfigToJson(const NodeInfoConfig::RpcConfig& rpc);
    static Json::Value httpConfigToJson(const NodeInfoConfig::HttpConfig& http);
    static Json::Value tlsConfigToJson(const NodeInfoConfig::TlsConfig& tls);
    static Json::Value loggingConfigToJson(const NodeInfoConfig::LoggingConfig& logging);
    static Json::Value miningConfigToJson(const NodeInfoConfig::MiningConfig& mining);
    static Json::Value p2pConfigToJson(const NodeInfoConfig::P2pConfig& p2p);
    static Json::Value walletConfigToJson(const NodeInfoConfig::WalletConfig& wallet);
    static Json::Value securityConfigToJson(const NodeInfoConfig::SecurityConfig& security);
    
    // Utility methods
    static TlsMode stringToTlsMode(const std::string& mode_str);
    static std::string tlsModeToString(TlsMode mode);
    static std::vector<std::string> jsonArrayToStringVector(const Json::Value& array);
    static Json::Value stringVectorToJsonArray(const std::vector<std::string>& vec);
};

} // namespace dinero
