#pragma once

#include <string>
#include <map>
#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>

namespace dinero {
namespace monitoring {

/**
 * Health and metrics endpoints for production monitoring
 * Provides /healthz, /readyz, and /metrics endpoints
 */
class HealthEndpoints {
public:
    struct HealthStatus {
        bool healthy = false;
        std::string status = "unknown";
        std::map<std::string, std::string> checks;
        std::chrono::system_clock::time_point last_check;
    };

    struct ReadinessStatus {
        bool ready = false;
        std::string status = "not_ready";
        std::map<std::string, std::string> checks;
        std::chrono::system_clock::time_point last_check;
    };

    static HealthEndpoints& getInstance();
    
    // Health check (liveness probe)
    HealthStatus getHealth();
    std::string getHealthJson();
    
    // Readiness check (readiness probe)
    ReadinessStatus getReadiness();
    std::string getReadinessJson();
    
    // Prometheus metrics
    std::string getMetrics();
    
    // Update component status
    void setDatabaseStatus(const std::string& db_name, bool healthy, 
                          const std::string& last_error = "");
    void setRpcStatus(bool bound, int port, const std::string& bind_address);
    void setWebSocketStatus(bool bound, int port, int active_connections);
    void setChainStatus(int64_t height, bool synced, 
                       const std::string& best_hash = "");
    void setExplorerStatus(bool caught_up, int64_t indexed_height);
    
    // Metrics collection
    void recordRpcRequest(const std::string& method, int http_code, 
                         double duration_seconds);
    void recordWebSocketConnection(bool connected);
    void recordDatabaseOperation(const std::string& db_name, 
                               const std::string& operation,
                               double duration_seconds, bool success);
    void updateChainHeight(int64_t height);
    void updateWalFrames(const std::string& db_name, int64_t frames);
    void updateMemoryUsage(double rss_mb);

private:
    HealthEndpoints() = default;
    
    // Component status
    struct DatabaseStatus {
        bool healthy = false;
        std::string last_error = "";
        std::chrono::system_clock::time_point last_checkpoint;
        int64_t wal_frames = 0;
    };
    
    struct ServiceStatus {
        bool rpc_bound = false;
        int rpc_port = 0;
        std::string rpc_bind_address = "";
        
        bool ws_bound = false;
        int ws_port = 0;
        int ws_connections = 0;
        
        int64_t chain_height = 0;
        bool chain_synced = false;
        std::string best_hash = "";
        
        bool explorer_caught_up = false;
        int64_t explorer_height = 0;
    };
    
    // Metrics storage
    struct RpcMetrics {
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> error_requests{0};
        std::map<std::string, std::atomic<uint64_t>> method_counts;
        std::map<std::string, std::atomic<uint64_t>> status_counts;
        
        // Simple histogram buckets for latency
        std::atomic<uint64_t> latency_bucket_100ms{0};
        std::atomic<uint64_t> latency_bucket_500ms{0};
        std::atomic<uint64_t> latency_bucket_1s{0};
        std::atomic<uint64_t> latency_bucket_5s{0};
        std::atomic<uint64_t> latency_bucket_inf{0};
        std::atomic<double> latency_sum{0.0};
    };
    
    struct SystemMetrics {
        std::atomic<int32_t> ws_connections_current{0};
        std::atomic<int64_t> chain_height{0};
        std::atomic<double> memory_rss_mb{0.0};
        std::map<std::string, std::atomic<int64_t>> sqlite_wal_frames;
        std::map<std::string, std::atomic<uint64_t>> db_operation_counts;
    };
    
    std::map<std::string, DatabaseStatus> database_status_;
    ServiceStatus service_status_;
    RpcMetrics rpc_metrics_;
    SystemMetrics system_metrics_;
    
    mutable std::mutex status_mutex_;
    
    // Helper methods
    bool isDatabaseHealthy(const std::string& db_name) const;
    bool areServicesReady() const;
    bool isChainReady() const;
    std::string formatPrometheusMetrics() const;
    std::string escapePrometheusLabel(const std::string& value) const;
};

/**
 * HTTP handler for health endpoints
 * Integrates with your existing HTTP server
 */
class HealthHttpHandler {
public:
    static std::string handleRequest(const std::string& path, 
                                   const std::string& method = "GET");
    
private:
    static std::string makeHttpResponse(int status_code, 
                                      const std::string& content_type,
                                      const std::string& body);
};

} // namespace monitoring
} // namespace dinero
