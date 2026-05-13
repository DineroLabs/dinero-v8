#include "config/nodeinfo_parser.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

namespace dinero {

std::optional<NodeInfoConfig> NodeInfoParser::parseFromFile(const std::string& filename) {
    // Add a small delay to avoid read-after-write race condition
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        // Use logger instead of std::cerr to avoid race with main thread logging
        // std::cerr << "[NodeInfo] Failed to open file: " << filename << std::endl;
        return std::nullopt;
    }
    
    std::string json_str((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();
    
    return parseFromString(json_str);
}

std::optional<NodeInfoConfig> NodeInfoParser::parseFromString(const std::string& json_str) {
    Json::CharReaderBuilder reader_builder;
    std::string parse_errors;
    Json::Value json;
    
    std::istringstream json_stream(json_str);
    if (!Json::parseFromStream(reader_builder, json_stream, &json, &parse_errors)) {
        std::cerr << "[NodeInfo] JSON parse error: " << parse_errors << std::endl;
        return std::nullopt;
    }
    
    return parseFromJson(json);
}

std::optional<NodeInfoConfig> NodeInfoParser::parseFromJson(const Json::Value& json) {
    NodeInfoConfig config;
    
    try {
        // Schema version validation
        if (json.isMember("schema") && json["schema"].isString()) {
            config.schema_version = json["schema"].asString();
            if (config.schema_version != "din.nodeinfo.v1") {
                std::cerr << "[NodeInfo] Unsupported schema version: " 
                         << config.schema_version << std::endl;
                return std::nullopt;
            }
        }
        
        // Basic configuration
        if (json.isMember("network") && json["network"].isString()) {
            config.network = json["network"].asString();
        }
        
        if (json.isMember("data_dir") && json["data_dir"].isString()) {
            config.data_dir = json["data_dir"].asString();
        }
        
        // Parse subsections
        if (json.isMember("rpc") && json["rpc"].isObject()) {
            parseRpcConfig(json["rpc"], config.rpc);
        }
        
        if (json.isMember("http") && json["http"].isObject()) {
            parseHttpConfig(json["http"], config.http);
        }
        
        if (json.isMember("tls") && json["tls"].isObject()) {
            parseTlsConfig(json["tls"], config.tls);
        }
        
        if (json.isMember("logging") && json["logging"].isObject()) {
            parseLoggingConfig(json["logging"], config.logging);
        }
        
        if (json.isMember("mining") && json["mining"].isObject()) {
            parseMiningConfig(json["mining"], config.mining);
        }
        
        if (json.isMember("p2p") && json["p2p"].isObject()) {
            parseP2pConfig(json["p2p"], config.p2p);
        }
        
        if (json.isMember("wallet") && json["wallet"].isObject()) {
            parseWalletConfig(json["wallet"], config.wallet);
        }
        
        if (json.isMember("security") && json["security"].isObject()) {
            parseSecurityConfig(json["security"], config.security);
        }
        
        // Validate the parsed configuration
        std::string error_message;
        if (!validateConfig(config, error_message)) {
            std::cerr << "[NodeInfo] Configuration validation failed: " 
                     << error_message << std::endl;
            return std::nullopt;
        }
        
        return config;
        
    } catch (const std::exception& e) {
        std::cerr << "[NodeInfo] Parse exception: " << e.what() << std::endl;
        return std::nullopt;
    }
}

NodeInfoConfig NodeInfoParser::generateDefault(const std::string& network) {
    NodeInfoConfig config;
    config.network = network;
    
    // Set network-specific defaults aligned with consensus chainparams.
    if (network == "mainnet") {
        config.rpc.port = 20998;
        config.http.port = 8080;
        config.p2p.port = 20999;
    } else if (network == "testnet") {
        config.rpc.port = 20998;
        config.http.port = 18080;
        config.p2p.port = 21000;
    } else if (network == "regtest") {
        config.rpc.port = 20996;
        config.http.port = 18880;
        config.p2p.port = 21001;
    }
    
    return config;
}

bool NodeInfoParser::writeToFile(const NodeInfoConfig& config, const std::string& filename) {
    try {
        // Create directory if it doesn't exist
        std::filesystem::path file_path(filename);
        std::filesystem::create_directories(file_path.parent_path());
        
        Json::Value json = configToJson(config);
        
        std::ofstream file(filename);
        if (!file.is_open()) {
            std::cerr << "[NodeInfo] Failed to create file: " << filename << std::endl;
            return false;
        }
        
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
        writer->write(json, &file);
        
        file.close();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[NodeInfo] Write exception: " << e.what() << std::endl;
        return false;
    }
}

Json::Value NodeInfoParser::configToJson(const NodeInfoConfig& config) {
    Json::Value json;
    
    json["schema"] = config.schema_version;
    json["network"] = config.network;
    json["data_dir"] = config.data_dir;
    
    json["rpc"] = rpcConfigToJson(config.rpc);
    json["http"] = httpConfigToJson(config.http);
    json["tls"] = tlsConfigToJson(config.tls);
    json["logging"] = loggingConfigToJson(config.logging);
    json["mining"] = miningConfigToJson(config.mining);
    json["p2p"] = p2pConfigToJson(config.p2p);
    json["wallet"] = walletConfigToJson(config.wallet);
    json["security"] = securityConfigToJson(config.security);
    
    return json;
}

bool NodeInfoParser::validateConfig(const NodeInfoConfig& config, std::string& error_message) {
    // Validate network
    if (config.network != "mainnet" && config.network != "testnet" && config.network != "regtest") {
        error_message = "Invalid network: " + config.network;
        return false;
    }
    
    // Validate ports
    if (config.rpc.port < 1 || config.rpc.port > 65535) {
        error_message = "Invalid RPC port: " + std::to_string(config.rpc.port);
        return false;
    }
    
    if (config.http.port < 1 || config.http.port > 65535) {
        error_message = "Invalid HTTP port: " + std::to_string(config.http.port);
        return false;
    }
    
    if (config.p2p.port < 1 || config.p2p.port > 65535) {
        error_message = "Invalid P2P port: " + std::to_string(config.p2p.port);
        return false;
    }
    
    // Validate TLS configuration
    if (config.tls.mode == TlsMode::PUBLIC) {
        if (config.tls.cert_file.empty() || config.tls.key_file.empty()) {
            error_message = "TLS public mode requires cert_file and key_file";
            return false;
        }
    }
    
    // Validate mining configuration
    if (config.mining.enabled && !config.mining.auto_generate_address && 
        config.mining.mining_address.empty()) {
        error_message = "Mining enabled but no mining address specified";
        return false;
    }
    
    return true;
}

// Helper parsing methods
void NodeInfoParser::parseRpcConfig(const Json::Value& json, NodeInfoConfig::RpcConfig& rpc) {
    if (json.isMember("bind_address") && json["bind_address"].isString()) {
        rpc.bind_address = json["bind_address"].asString();
    }
    if (json.isMember("port") && json["port"].isInt()) {
        rpc.port = json["port"].asInt();
    }
    if (json.isMember("enabled") && json["enabled"].isBool()) {
        rpc.enabled = json["enabled"].asBool();
    }
    if (json.isMember("cookie_file") && json["cookie_file"].isString()) {
        rpc.cookie_file = json["cookie_file"].asString();
    }
    if (json.isMember("max_connections") && json["max_connections"].isInt()) {
        rpc.max_connections = json["max_connections"].asInt();
    }
    if (json.isMember("request_timeout_seconds") && json["request_timeout_seconds"].isInt()) {
        rpc.request_timeout_seconds = json["request_timeout_seconds"].asInt();
    }
}

void NodeInfoParser::parseHttpConfig(const Json::Value& json, NodeInfoConfig::HttpConfig& http) {
    if (json.isMember("bind_address") && json["bind_address"].isString()) {
        http.bind_address = json["bind_address"].asString();
    }
    if (json.isMember("port") && json["port"].isInt()) {
        http.port = json["port"].asInt();
    }
    if (json.isMember("enabled") && json["enabled"].isBool()) {
        http.enabled = json["enabled"].asBool();
    }
    if (json.isMember("threads") && json["threads"].isInt()) {
        http.threads = json["threads"].asInt();
    }
    if (json.isMember("health_endpoint") && json["health_endpoint"].isBool()) {
        http.health_endpoint = json["health_endpoint"].asBool();
    }
    if (json.isMember("metrics_endpoint") && json["metrics_endpoint"].isBool()) {
        http.metrics_endpoint = json["metrics_endpoint"].asBool();
    }
    if (json.isMember("max_request_size") && json["max_request_size"].isUInt64()) {
        http.max_request_size = json["max_request_size"].asUInt64();
    }
}

void NodeInfoParser::parseTlsConfig(const Json::Value& json, NodeInfoConfig::TlsConfig& tls) {
    if (json.isMember("mode") && json["mode"].isString()) {
        tls.mode = stringToTlsMode(json["mode"].asString());
    }
    if (json.isMember("cert_file") && json["cert_file"].isString()) {
        tls.cert_file = json["cert_file"].asString();
    }
    if (json.isMember("key_file") && json["key_file"].isString()) {
        tls.key_file = json["key_file"].asString();
    }
    if (json.isMember("ca_file") && json["ca_file"].isString()) {
        tls.ca_file = json["ca_file"].asString();
    }
    if (json.isMember("require_client_cert") && json["require_client_cert"].isBool()) {
        tls.require_client_cert = json["require_client_cert"].asBool();
    }
    if (json.isMember("allowed_fingerprints") && json["allowed_fingerprints"].isArray()) {
        tls.allowed_fingerprints = jsonArrayToStringVector(json["allowed_fingerprints"]);
    }
}

void NodeInfoParser::parseLoggingConfig(const Json::Value& json, NodeInfoConfig::LoggingConfig& logging) {
    if (json.isMember("level") && json["level"].isString()) {
        logging.level = json["level"].asString();
    }
    if (json.isMember("structured") && json["structured"].isBool()) {
        logging.structured = json["structured"].asBool();
    }
    if (json.isMember("console_output") && json["console_output"].isBool()) {
        logging.console_output = json["console_output"].asBool();
    }
    if (json.isMember("file_output") && json["file_output"].isBool()) {
        logging.file_output = json["file_output"].asBool();
    }
    if (json.isMember("log_file") && json["log_file"].isString()) {
        logging.log_file = json["log_file"].asString();
    }
    if (json.isMember("trace_rpc") && json["trace_rpc"].isBool()) {
        logging.trace_rpc = json["trace_rpc"].asBool();
    }
    if (json.isMember("trace_http") && json["trace_http"].isBool()) {
        logging.trace_http = json["trace_http"].asBool();
    }
}

void NodeInfoParser::parseMiningConfig(const Json::Value& json, NodeInfoConfig::MiningConfig& mining) {
    if (json.isMember("enabled") && json["enabled"].isBool()) {
        mining.enabled = json["enabled"].asBool();
    }
    if (json.isMember("mining_address") && json["mining_address"].isString()) {
        mining.mining_address = json["mining_address"].asString();
    }
    if (json.isMember("threads") && json["threads"].isInt()) {
        mining.threads = json["threads"].asInt();
    }
    if (json.isMember("auto_generate_address") && json["auto_generate_address"].isBool()) {
        mining.auto_generate_address = json["auto_generate_address"].asBool();
    }
}

void NodeInfoParser::parseP2pConfig(const Json::Value& json, NodeInfoConfig::P2pConfig& p2p) {
    if (json.isMember("bind_address") && json["bind_address"].isString()) {
        p2p.bind_address = json["bind_address"].asString();
    }
    if (json.isMember("port") && json["port"].isInt()) {
        p2p.port = json["port"].asInt();
    }
    if (json.isMember("enabled") && json["enabled"].isBool()) {
        p2p.enabled = json["enabled"].asBool();
    }
    if (json.isMember("max_connections") && json["max_connections"].isInt()) {
        p2p.max_connections = json["max_connections"].asInt();
    }
    if (json.isMember("seed_nodes") && json["seed_nodes"].isArray()) {
        p2p.seed_nodes = jsonArrayToStringVector(json["seed_nodes"]);
    }
    if (json.isMember("discover_peers") && json["discover_peers"].isBool()) {
        p2p.discover_peers = json["discover_peers"].asBool();
    }
}

void NodeInfoParser::parseWalletConfig(const Json::Value& json, NodeInfoConfig::WalletConfig& wallet) {
    if (json.isMember("enabled") && json["enabled"].isBool()) {
        wallet.enabled = json["enabled"].asBool();
    }
    if (json.isMember("default_wallet") && json["default_wallet"].isString()) {
        wallet.default_wallet = json["default_wallet"].asString();
    }
    if (json.isMember("auto_create_default") && json["auto_create_default"].isBool()) {
        wallet.auto_create_default = json["auto_create_default"].asBool();
    }
    if (json.isMember("keypool_size") && json["keypool_size"].isInt()) {
        wallet.keypool_size = json["keypool_size"].asInt();
    }
}

void NodeInfoParser::parseSecurityConfig(const Json::Value& json, NodeInfoConfig::SecurityConfig& security) {
    if (json.isMember("cookie_rotation") && json["cookie_rotation"].isBool()) {
        security.cookie_rotation = json["cookie_rotation"].asBool();
    }
    if (json.isMember("cookie_rotation_minutes") && json["cookie_rotation_minutes"].isInt()) {
        security.cookie_rotation_minutes = json["cookie_rotation_minutes"].asInt();
    }
    if (json.isMember("request_rate_limit") && json["request_rate_limit"].isInt()) {
        security.request_rate_limit = json["request_rate_limit"].asInt();
    }
    if (json.isMember("dos_protection") && json["dos_protection"].isBool()) {
        security.dos_protection = json["dos_protection"].asBool();
    }
    if (json.isMember("allowed_ips") && json["allowed_ips"].isArray()) {
        security.allowed_ips = jsonArrayToStringVector(json["allowed_ips"]);
    }
}

// Helper serialization methods
Json::Value NodeInfoParser::rpcConfigToJson(const NodeInfoConfig::RpcConfig& rpc) {
    Json::Value json;
    json["bind_address"] = rpc.bind_address;
    json["port"] = rpc.port;
    json["enabled"] = rpc.enabled;
    json["cookie_file"] = rpc.cookie_file;
    json["max_connections"] = rpc.max_connections;
    json["request_timeout_seconds"] = rpc.request_timeout_seconds;
    return json;
}

Json::Value NodeInfoParser::httpConfigToJson(const NodeInfoConfig::HttpConfig& http) {
    Json::Value json;
    json["bind_address"] = http.bind_address;
    json["port"] = http.port;
    json["enabled"] = http.enabled;
    json["threads"] = http.threads;
    json["health_endpoint"] = http.health_endpoint;
    json["metrics_endpoint"] = http.metrics_endpoint;
    json["max_request_size"] = static_cast<uint64_t>(http.max_request_size);
    return json;
}

Json::Value NodeInfoParser::tlsConfigToJson(const NodeInfoConfig::TlsConfig& tls) {
    Json::Value json;
    json["mode"] = tlsModeToString(tls.mode);
    json["cert_file"] = tls.cert_file;
    json["key_file"] = tls.key_file;
    json["ca_file"] = tls.ca_file;
    json["require_client_cert"] = tls.require_client_cert;
    json["allowed_fingerprints"] = stringVectorToJsonArray(tls.allowed_fingerprints);
    return json;
}

Json::Value NodeInfoParser::loggingConfigToJson(const NodeInfoConfig::LoggingConfig& logging) {
    Json::Value json;
    json["level"] = logging.level;
    json["structured"] = logging.structured;
    json["console_output"] = logging.console_output;
    json["file_output"] = logging.file_output;
    json["log_file"] = logging.log_file;
    json["trace_rpc"] = logging.trace_rpc;
    json["trace_http"] = logging.trace_http;
    return json;
}

Json::Value NodeInfoParser::miningConfigToJson(const NodeInfoConfig::MiningConfig& mining) {
    Json::Value json;
    json["enabled"] = mining.enabled;
    json["mining_address"] = mining.mining_address;
    json["threads"] = mining.threads;
    json["auto_generate_address"] = mining.auto_generate_address;
    return json;
}

Json::Value NodeInfoParser::p2pConfigToJson(const NodeInfoConfig::P2pConfig& p2p) {
    Json::Value json;
    json["bind_address"] = p2p.bind_address;
    json["port"] = p2p.port;
    json["enabled"] = p2p.enabled;
    json["max_connections"] = p2p.max_connections;
    json["seed_nodes"] = stringVectorToJsonArray(p2p.seed_nodes);
    json["discover_peers"] = p2p.discover_peers;
    return json;
}

Json::Value NodeInfoParser::walletConfigToJson(const NodeInfoConfig::WalletConfig& wallet) {
    Json::Value json;
    json["enabled"] = wallet.enabled;
    json["default_wallet"] = wallet.default_wallet;
    json["auto_create_default"] = wallet.auto_create_default;
    json["keypool_size"] = wallet.keypool_size;
    return json;
}

Json::Value NodeInfoParser::securityConfigToJson(const NodeInfoConfig::SecurityConfig& security) {
    Json::Value json;
    json["cookie_rotation"] = security.cookie_rotation;
    json["cookie_rotation_minutes"] = security.cookie_rotation_minutes;
    json["request_rate_limit"] = security.request_rate_limit;
    json["dos_protection"] = security.dos_protection;
    json["allowed_ips"] = stringVectorToJsonArray(security.allowed_ips);
    return json;
}

// Utility methods
TlsMode NodeInfoParser::stringToTlsMode(const std::string& mode_str) {
    if (mode_str == "off") return TlsMode::OFF;
    if (mode_str == "loopback_only") return TlsMode::LOOPBACK_ONLY;
    if (mode_str == "public") return TlsMode::PUBLIC;
    return TlsMode::LOOPBACK_ONLY; // default
}

std::string NodeInfoParser::tlsModeToString(TlsMode mode) {
    switch (mode) {
        case TlsMode::OFF: return "off";
        case TlsMode::LOOPBACK_ONLY: return "loopback_only";
        case TlsMode::PUBLIC: return "public";
        default: return "loopback_only";
    }
}

std::vector<std::string> NodeInfoParser::jsonArrayToStringVector(const Json::Value& array) {
    std::vector<std::string> result;
    if (array.isArray()) {
        for (const auto& item : array) {
            if (item.isString()) {
                result.push_back(item.asString());
            }
        }
    }
    return result;
}

Json::Value NodeInfoParser::stringVectorToJsonArray(const std::vector<std::string>& vec) {
    Json::Value array(Json::arrayValue);
    for (const auto& str : vec) {
        array.append(str);
    }
    return array;
}

} // namespace dinero
