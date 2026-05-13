#pragma once

#include <string>
#include <optional>
#include "compat/jsoncpp_compat.h"

namespace dinero {
namespace cli {

/**
 * Health status from /healthz endpoint
 */
struct HealthStatus {
    bool ok;
    int tipHeight;
    int peers;
    int mempoolSize;
    std::string status;
    std::optional<std::string> error;
};

/**
 * Key metrics from /metrics endpoint
 */
struct KeyMetrics {
    int chainTip;
    int peers;
    int mempoolTxCount;
    double uptimeSeconds;
    int rpcCallsTotal;
};

/**
 * HTTP client for health and metrics endpoints
 */
class HealthClient {
public:
    /**
     * Check daemon health via /healthz endpoint
     * @param baseUrl Base URL (e.g., "http://localhost:20998")
     * @return Health status or nullopt if unavailable
     */
    static std::optional<HealthStatus> getHealthStatus(const std::string& baseUrl);
    
    /**
     * Get key metrics via /metrics endpoint
     * @param baseUrl Base URL (e.g., "http://localhost:20998")
     * @return Key metrics or nullopt if unavailable
     */
    static std::optional<KeyMetrics> getKeyMetrics(const std::string& baseUrl);
    
    /**
     * Fast readiness check using /healthz
     * @param baseUrl Base URL
     * @param timeoutSeconds Timeout for HTTP request
     * @return true if daemon is ready
     */
    static bool isReady(const std::string& baseUrl, int timeoutSeconds = 5);
    
private:
    /**
     * Make HTTP GET request with timeout
     * @param url Full URL to request
     * @param timeoutSeconds Request timeout
     * @return Response body or empty string on error
     */
    static std::string httpGet(const std::string& url, int timeoutSeconds = 5);
    
    /**
     * Parse Prometheus metrics format
     * @param metricsText Raw metrics text
     * @return Parsed key metrics
     */
    static std::optional<KeyMetrics> parseMetrics(const std::string& metricsText);
};

} // namespace cli
} // namespace dinero
