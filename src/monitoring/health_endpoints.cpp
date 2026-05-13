#include "monitoring/health_endpoints.h"
#include "compat/jsoncpp_compat.h"
#include <sstream>
#include <iomanip>
#include <mutex>
#include <algorithm>

namespace dinero {
namespace monitoring {

HealthEndpoints& HealthEndpoints::getInstance() {
    static HealthEndpoints instance;
    return instance;
}

HealthEndpoints::HealthStatus HealthEndpoints::getHealth() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    
    HealthStatus status;
    status.last_check = std::chrono::system_clock::now();
    status.healthy = true; // Assume healthy unless proven otherwise
    
    // Check if process is responsive (we're executing, so yes)
    status.checks["process"] = "ok";
    
    // Check memory usage (basic threshold)
    double mem_mb = system_metrics_.memory_rss_mb.load();
    if (mem_mb > 0) {
        status.checks["memory"] = mem_mb < 2048 ? "ok" : "high"; // 2GB threshold
        if (mem_mb >= 4096) { // 4GB critical threshold
            status.healthy = false;
        }
    } else {
        status.checks["memory"] = "unknown";
    }
    
    // Check database connectivity
    bool any_db_unhealthy = false;
    for (const auto& [db_name, db_status] : database_status_) {
        if (db_status.healthy) {
            status.checks["db_" + db_name] = "ok";
        } else {
            status.checks["db_" + db_name] = "error: " + db_status.last_error;
            any_db_unhealthy = true;
        }
    }
    
    if (any_db_unhealthy) {
        status.healthy = false;
    }
    
    status.status = status.healthy ? "healthy" : "unhealthy";
    return status;
}

std::string HealthEndpoints::getHealthJson() {
    auto health = getHealth();
    
    Json::Value j;
    j["status"] = health.status;
    j["healthy"] = health.healthy;
    j["timestamp"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        health.last_check.time_since_epoch()).count());
    
    Json::Value checks;
    for (const auto& [key, value] : health.checks) {
        checks[key] = value;
    }
    j["checks"] = checks;
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, j);
}

HealthEndpoints::ReadinessStatus HealthEndpoints::getReadiness() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    
    ReadinessStatus status;
    status.last_check = std::chrono::system_clock::now();
    status.ready = true; // Assume ready unless proven otherwise
    
    // Check databases are opened and healthy
    for (const auto& [db_name, db_status] : database_status_) {
        if (db_status.healthy) {
            // Check WAL checkpoint recency (< 10 minutes)
            auto now = std::chrono::system_clock::now();
            auto checkpoint_age = std::chrono::duration_cast<std::chrono::minutes>(
                now - db_status.last_checkpoint);
            
            if (checkpoint_age.count() < 10) {
                status.checks["db_" + db_name] = "ready";
            } else {
                status.checks["db_" + db_name] = "stale_checkpoint";
                status.ready = false;
            }
        } else {
            status.checks["db_" + db_name] = "not_ready";
            status.ready = false;
        }
    }
    
    // Check RPC service is bound
    if (service_status_.rpc_bound) {
        status.checks["rpc"] = "ready";
    } else {
        status.checks["rpc"] = "not_bound";
        status.ready = false;
    }
    
    // Check WebSocket service is bound
    if (service_status_.ws_bound) {
        status.checks["websocket"] = "ready";
    } else {
        status.checks["websocket"] = "not_bound";
        status.ready = false;
    }
    
    // Check chain height >= 1
    if (service_status_.chain_height >= 1) {
        status.checks["blockchain"] = "ready";
    } else {
        status.checks["blockchain"] = "no_blocks";
        status.ready = false;
    }
    
    // Check explorer is caught up (if enabled)
    if (service_status_.explorer_caught_up || service_status_.explorer_height == 0) {
        status.checks["explorer"] = "ready";
    } else {
        status.checks["explorer"] = "syncing";
        // Don't fail readiness for explorer lag
    }
    
    status.status = status.ready ? "ready" : "not_ready";
    return status;
}

std::string HealthEndpoints::getReadinessJson() {
    auto readiness = getReadiness();
    
    Json::Value j;
    j["status"] = readiness.status;
    j["ready"] = readiness.ready;
    j["timestamp"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        readiness.last_check.time_since_epoch()).count());
    
    Json::Value checks;
    for (const auto& [key, value] : readiness.checks) {
        checks[key] = value;
    }
    j["checks"] = checks;
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, j);
}

std::string HealthEndpoints::getMetrics() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return formatPrometheusMetrics();
}

void HealthEndpoints::setDatabaseStatus(const std::string& db_name, bool healthy,
                                       const std::string& last_error) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    database_status_[db_name].healthy = healthy;
    database_status_[db_name].last_error = last_error;
    if (healthy) {
        database_status_[db_name].last_checkpoint = std::chrono::system_clock::now();
    }
}

void HealthEndpoints::setRpcStatus(bool bound, int port, const std::string& bind_address) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    service_status_.rpc_bound = bound;
    service_status_.rpc_port = port;
    service_status_.rpc_bind_address = bind_address;
}

void HealthEndpoints::setWebSocketStatus(bool bound, int port, int active_connections) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    service_status_.ws_bound = bound;
    service_status_.ws_port = port;
    service_status_.ws_connections = active_connections;
    system_metrics_.ws_connections_current.store(active_connections);
}

void HealthEndpoints::setChainStatus(int64_t height, bool synced, const std::string& best_hash) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    service_status_.chain_height = height;
    service_status_.chain_synced = synced;
    service_status_.best_hash = best_hash;
    system_metrics_.chain_height.store(height);
}

void HealthEndpoints::setExplorerStatus(bool caught_up, int64_t indexed_height) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    service_status_.explorer_caught_up = caught_up;
    service_status_.explorer_height = indexed_height;
}

void HealthEndpoints::recordRpcRequest(const std::string& method, int http_code,
                                     double duration_seconds) {
    rpc_metrics_.total_requests.fetch_add(1);
    
    if (http_code >= 400) {
        rpc_metrics_.error_requests.fetch_add(1);
    }
    
    // Update latency histogram
    if (duration_seconds <= 0.1) {
        rpc_metrics_.latency_bucket_100ms.fetch_add(1);
    } else if (duration_seconds <= 0.5) {
        rpc_metrics_.latency_bucket_500ms.fetch_add(1);
    } else if (duration_seconds <= 1.0) {
        rpc_metrics_.latency_bucket_1s.fetch_add(1);
    } else if (duration_seconds <= 5.0) {
        rpc_metrics_.latency_bucket_5s.fetch_add(1);
    } else {
        rpc_metrics_.latency_bucket_inf.fetch_add(1);
    }
    
    // Update sum for average calculation
    double current_sum = rpc_metrics_.latency_sum.load();
    while (!rpc_metrics_.latency_sum.compare_exchange_weak(current_sum, 
                                                          current_sum + duration_seconds)) {
        // Retry on contention
    }
    
    // Update method and status code counters
    // Note: This is simplified - in production you'd want thread-safe maps
    // or use a proper metrics library like Prometheus C++ client
}

void HealthEndpoints::recordWebSocketConnection(bool connected) {
    int32_t current = system_metrics_.ws_connections_current.load();
    if (connected) {
        system_metrics_.ws_connections_current.store(current + 1);
    } else {
        system_metrics_.ws_connections_current.store(std::max(0, current - 1));
    }
}

void HealthEndpoints::recordDatabaseOperation(const std::string& db_name,
                                            const std::string& operation,
                                            double duration_seconds, bool success) {
    system_metrics_.db_operation_counts[db_name + "_" + operation].fetch_add(1);
    
    if (!success) {
        system_metrics_.db_operation_counts[db_name + "_errors"].fetch_add(1);
    }
}

void HealthEndpoints::updateChainHeight(int64_t height) {
    system_metrics_.chain_height.store(height);
}

void HealthEndpoints::updateWalFrames(const std::string& db_name, int64_t frames) {
    system_metrics_.sqlite_wal_frames[db_name].store(frames);
}

void HealthEndpoints::updateMemoryUsage(double rss_mb) {
    system_metrics_.memory_rss_mb.store(rss_mb);
}

std::string HealthEndpoints::formatPrometheusMetrics() const {
    std::ostringstream metrics;
    
    // RPC metrics
    metrics << "# HELP dinero_rpc_requests_total Total RPC requests.\n";
    metrics << "# TYPE dinero_rpc_requests_total counter\n";
    metrics << "dinero_rpc_requests_total " << rpc_metrics_.total_requests.load() << "\n\n";
    
    metrics << "# HELP dinero_rpc_errors_total Total RPC errors (4xx/5xx).\n";
    metrics << "# TYPE dinero_rpc_errors_total counter\n";
    metrics << "dinero_rpc_errors_total " << rpc_metrics_.error_requests.load() << "\n\n";
    
    // RPC latency histogram
    metrics << "# HELP dinero_rpc_request_duration_seconds RPC request latency.\n";
    metrics << "# TYPE dinero_rpc_request_duration_seconds histogram\n";
    metrics << "dinero_rpc_request_duration_seconds_bucket{le=\"0.1\"} " 
            << rpc_metrics_.latency_bucket_100ms.load() << "\n";
    metrics << "dinero_rpc_request_duration_seconds_bucket{le=\"0.5\"} " 
            << rpc_metrics_.latency_bucket_500ms.load() << "\n";
    metrics << "dinero_rpc_request_duration_seconds_bucket{le=\"1.0\"} " 
            << rpc_metrics_.latency_bucket_1s.load() << "\n";
    metrics << "dinero_rpc_request_duration_seconds_bucket{le=\"5.0\"} " 
            << rpc_metrics_.latency_bucket_5s.load() << "\n";
    metrics << "dinero_rpc_request_duration_seconds_bucket{le=\"+Inf\"} " 
            << rpc_metrics_.latency_bucket_inf.load() << "\n";
    metrics << "dinero_rpc_request_duration_seconds_sum " 
            << rpc_metrics_.latency_sum.load() << "\n";
    metrics << "dinero_rpc_request_duration_seconds_count " 
            << rpc_metrics_.total_requests.load() << "\n\n";
    
    // WebSocket metrics
    metrics << "# HELP dinero_ws_connections_current Current WebSocket connections.\n";
    metrics << "# TYPE dinero_ws_connections_current gauge\n";
    metrics << "dinero_ws_connections_current " 
            << system_metrics_.ws_connections_current.load() << "\n\n";
    
    // Chain metrics
    metrics << "# HELP dinero_chain_height Current blockchain height.\n";
    metrics << "# TYPE dinero_chain_height gauge\n";
    metrics << "dinero_chain_height " << system_metrics_.chain_height.load() << "\n\n";
    
    // Memory metrics
    metrics << "# HELP dinero_memory_rss_bytes Resident set size in bytes.\n";
    metrics << "# TYPE dinero_memory_rss_bytes gauge\n";
    metrics << "dinero_memory_rss_bytes " 
            << (system_metrics_.memory_rss_mb.load() * 1024 * 1024) << "\n\n";
    
    // SQLite WAL metrics
    metrics << "# HELP dinero_sqlite_wal_frames WAL frames since last checkpoint.\n";
    metrics << "# TYPE dinero_sqlite_wal_frames gauge\n";
    for (const auto& [db_name, frames] : system_metrics_.sqlite_wal_frames) {
        metrics << "dinero_sqlite_wal_frames{database=\"" 
                << escapePrometheusLabel(db_name) << "\"} " 
                << frames.load() << "\n";
    }
    metrics << "\n";
    
    // Database operation metrics
    metrics << "# HELP dinero_database_operations_total Database operations by type.\n";
    metrics << "# TYPE dinero_database_operations_total counter\n";
    for (const auto& [key, count] : system_metrics_.db_operation_counts) {
        // Parse key like "blockchain_db_select" or "wallet_db_errors"
        size_t first_underscore = key.find('_');
        size_t last_underscore = key.rfind('_');
        
        if (first_underscore != std::string::npos && last_underscore != std::string::npos) {
            std::string db_name = key.substr(0, last_underscore);
            std::string operation = key.substr(last_underscore + 1);
            
            metrics << "dinero_database_operations_total{database=\"" 
                    << escapePrometheusLabel(db_name) << "\",operation=\""
                    << escapePrometheusLabel(operation) << "\"} " 
                    << count.load() << "\n";
        }
    }
    
    return metrics.str();
}

std::string HealthEndpoints::escapePrometheusLabel(const std::string& value) const {
    std::string escaped = value;
    // Escape backslashes, quotes, and newlines for Prometheus labels
    std::replace(escaped.begin(), escaped.end(), '\\', '/');
    std::replace(escaped.begin(), escaped.end(), '"', '\'');
    std::replace(escaped.begin(), escaped.end(), '\n', ' ');
    return escaped;
}

// HTTP Handler Implementation
std::string HealthHttpHandler::handleRequest(const std::string& path, const std::string& method) {
    if (method != "GET") {
        return makeHttpResponse(405, "text/plain", "Method Not Allowed");
    }
    
    auto& health = HealthEndpoints::getInstance();
    
    if (path == "/healthz") {
        auto status = health.getHealth();
        int status_code = status.healthy ? 200 : 503;
        return makeHttpResponse(status_code, "application/json", health.getHealthJson());
        
    } else if (path == "/readyz") {
        auto status = health.getReadiness();
        int status_code = status.ready ? 200 : 503;
        return makeHttpResponse(status_code, "application/json", health.getReadinessJson());
        
    } else if (path == "/metrics") {
        return makeHttpResponse(200, "text/plain; version=0.0.4; charset=utf-8", 
                              health.getMetrics());
        
    } else {
        return makeHttpResponse(404, "text/plain", "Not Found");
    }
}

std::string HealthHttpHandler::makeHttpResponse(int status_code, 
                                              const std::string& content_type,
                                              const std::string& body) {
    std::ostringstream response;
    
    std::string status_text;
    switch (status_code) {
        case 200: status_text = "OK"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 503: status_text = "Service Unavailable"; break;
        default: status_text = "Unknown"; break;
    }
    
    response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Cache-Control: no-cache, no-store, must-revalidate\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    
    return response.str();
}

} // namespace monitoring
} // namespace dinero
