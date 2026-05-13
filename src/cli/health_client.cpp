#include "cli/health_client.h"
#include <iostream>
#include <sstream>
#include <regex>
#include <curl/curl.h>

namespace dinero {
namespace cli {

// Callback for curl to write response data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string HealthClient::httpGet(const std::string& url, int timeoutSeconds) {
    CURL* curl;
    CURLcode res;
    std::string response;
    
    curl = curl_easy_init();
    if (!curl) {
        return "";
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dinero-cli/1.0.0");
    
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        return "";
    }
    
    return response;
}

std::optional<HealthStatus> HealthClient::getHealthStatus(const std::string& baseUrl) {
    try {
        std::string healthUrl = baseUrl + "/healthz";
        std::string response = httpGet(healthUrl);
        
        if (response.empty()) {
            return std::nullopt;
        }
        
        Json::CharReaderBuilder reader_builder;
        std::string parse_errors;
        Json::Value json;
        std::istringstream json_stream(response);
        
        if (!Json::parseFromStream(reader_builder, json_stream, &json, &parse_errors)) {
            return std::nullopt;
        }
        
        HealthStatus status;
        status.ok = json.isMember("ok") ? json["ok"].asBool() : false;
        status.tipHeight = json.isMember("tip_height") ? json["tip_height"].asInt() : 0;
        status.peers = json.isMember("peers") ? json["peers"].asInt() : 0;
        status.mempoolSize = json.isMember("mempool_size") ? json["mempool_size"].asInt() : 0;
        status.status = json.isMember("status") ? json["status"].asString() : "unknown";
        
        if (json.isMember("error")) {
            status.error = json["error"].asString();
        }
        
        return status;
        
    } catch (const std::exception& e) {
        return std::nullopt;
    }
}

std::optional<KeyMetrics> HealthClient::parseMetrics(const std::string& metricsText) {
    KeyMetrics metrics = {};
    
    // Parse Prometheus format using regex
    std::regex chainTipRegex(R"(dinerod_chain_tip_height\s+(\d+))");
    std::regex peersRegex(R"(dinerod_peers_connected\s+(\d+))");
    std::regex mempoolRegex(R"(dinerod_mempool_tx_count\s+(\d+))");
    std::regex uptimeRegex(R"(dinerod_uptime_seconds\s+([\d.]+))");
    std::regex rpcCallsRegex(R"(dinerod_rpc_calls_total\s+(\d+))");
    
    std::smatch match;
    
    if (std::regex_search(metricsText, match, chainTipRegex)) {
        metrics.chainTip = std::stoi(match[1]);
    }
    
    if (std::regex_search(metricsText, match, peersRegex)) {
        metrics.peers = std::stoi(match[1]);
    }
    
    if (std::regex_search(metricsText, match, mempoolRegex)) {
        metrics.mempoolTxCount = std::stoi(match[1]);
    }
    
    if (std::regex_search(metricsText, match, uptimeRegex)) {
        metrics.uptimeSeconds = std::stod(match[1]);
    }
    
    if (std::regex_search(metricsText, match, rpcCallsRegex)) {
        metrics.rpcCallsTotal = std::stoi(match[1]);
    }
    
    return metrics;
}

std::optional<KeyMetrics> HealthClient::getKeyMetrics(const std::string& baseUrl) {
    try {
        std::string metricsUrl = baseUrl + "/metrics";
        std::string response = httpGet(metricsUrl);
        
        if (response.empty()) {
            return std::nullopt;
        }
        
        return parseMetrics(response);
        
    } catch (const std::exception& e) {
        return std::nullopt;
    }
}

bool HealthClient::isReady(const std::string& baseUrl, int timeoutSeconds) {
    auto health = getHealthStatus(baseUrl);
    if (!health) {
        return false;
    }
    
    return health->ok && health->peers > 0;
}

} // namespace cli
} // namespace dinero
