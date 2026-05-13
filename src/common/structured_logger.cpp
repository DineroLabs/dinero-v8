#include "common/structured_logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <fstream>
#include <sys/resource.h>

namespace dinero {
namespace logging {

// Global state access functions (implement based on your architecture)
extern std::string GetActiveNetworkHRP();
extern int64_t GetBestHeight();
extern std::string GetBestHashHex();
extern int32_t GetActiveWebSocketConnections();

void StructuredLogger::Context::refresh() {
    try {
        hrp = GetActiveNetworkHRP();
        height = GetBestHeight();
        best_hash = GetBestHashHex();
        ws_connections = GetActiveWebSocketConnections();
        
        // Get memory usage
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            mem_rss_mb = static_cast<double>(usage.ru_maxrss) / 1024.0; // KB to MB on Linux
#ifdef __APPLE__
            mem_rss_mb /= 1024.0; // Bytes to MB on macOS
#endif
        }
    } catch (...) {
        // Fallback values if global state not available
        hrp = "din";
        height = 0;
        best_hash = "";
        ws_connections = 0;
        mem_rss_mb = 0.0;
    }
}

StructuredLogger& StructuredLogger::getInstance() {
    static StructuredLogger instance;
    return instance;
}

std::string StructuredLogger::levelToString(Level level) const {
    switch (level) {
        case DEBUG: return "DEBUG";
        case INFO: return "INFO";
        case WARN: return "WARN";
        case ERROR: return "ERROR";
        case FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string StructuredLogger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

void StructuredLogger::log(Level level, const std::string& component, 
                          const std::string& event, const std::string& message,
                          const std::map<std::string, std::string>& extra) {
    
    // Update context before logging
    context_.refresh();
    
    Json::Value j;
    j["ts"] = getCurrentTimestamp();
    j["level"] = levelToString(level);
    j["comp"] = component;
    j["event"] = event;
    j["msg"] = message;
    j["hrp"] = context_.hrp;
    j["height"] = static_cast<int64_t>(context_.height);
    j["best_hash"] = context_.best_hash;
    j["ws_conns"] = static_cast<Json::Int>(context_.ws_connections);
    j["mem_rss_mb"] = static_cast<Json::Int>(context_.mem_rss_mb);
    
    // Add extra fields
    for (const auto& [key, value] : extra) {
        j[key] = value;
    }
    
    // Output to stdout for systemd journal capture
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::cout << Json::writeString(builder, j) << std::endl;
}

// RPC Logger Implementation
void RpcLogger::request(const std::string& method, const std::string& client_ip,
                       int http_code, double duration_ms) {
    std::map<std::string, std::string> extra{
        {"rpc_method", method},
        {"client_ip", client_ip},
        {"http_code", std::to_string(http_code)},
        {"duration_ms", std::to_string(duration_ms)}
    };
    
    auto level = (http_code >= 500) ? StructuredLogger::ERROR :
                 (http_code >= 400) ? StructuredLogger::WARN :
                 StructuredLogger::INFO;
    
    jlog(level, "rpc", "request", 
         "RPC " + method + " " + std::to_string(http_code), extra);
}

void RpcLogger::error(const std::string& method, const std::string& error_msg,
                     int http_code) {
    std::map<std::string, std::string> extra{
        {"rpc_method", method},
        {"http_code", std::to_string(http_code)},
        {"err", error_msg}
    };
    
    jlog(StructuredLogger::ERROR, "rpc", "error", 
         "RPC error: " + error_msg, extra);
}

// WebSocket Logger Implementation
void WebSocketLogger::connection(const std::string& event, const std::string& client_ip,
                               int total_connections) {
    std::map<std::string, std::string> extra{
        {"client_ip", client_ip},
        {"total_conns", std::to_string(total_connections)}
    };
    
    jlog(StructuredLogger::INFO, "websocket", event,
         "WebSocket " + event + " from " + client_ip, extra);
}

void WebSocketLogger::message(const std::string& type, int message_size,
                            const std::string& client_ip) {
    std::map<std::string, std::string> extra{
        {"msg_type", type},
        {"msg_size", std::to_string(message_size)}
    };
    
    if (!client_ip.empty()) {
        extra["client_ip"] = client_ip;
    }
    
    jlog(StructuredLogger::DEBUG, "websocket", "message",
         "WebSocket " + type + " message", extra);
}

void WebSocketLogger::error(const std::string& error_msg, const std::string& client_ip) {
    std::map<std::string, std::string> extra{
        {"err", error_msg}
    };
    
    if (!client_ip.empty()) {
        extra["client_ip"] = client_ip;
    }
    
    jlog(StructuredLogger::ERROR, "websocket", "error",
         "WebSocket error: " + error_msg, extra);
}

// Database Logger Implementation
void DatabaseLogger::operation(const std::string& db_name, const std::string& operation,
                             double duration_ms, bool success) {
    std::map<std::string, std::string> extra{
        {"db_name", db_name},
        {"operation", operation},
        {"duration_ms", std::to_string(duration_ms)},
        {"success", success ? "true" : "false"}
    };
    
    auto level = success ? StructuredLogger::DEBUG : StructuredLogger::ERROR;
    
    jlog(level, "database", "operation",
         db_name + " " + operation + (success ? " OK" : " FAILED"), extra);
}

void DatabaseLogger::checkpoint(const std::string& db_name, int wal_frames,
                              double duration_ms) {
    std::map<std::string, std::string> extra{
        {"db_name", db_name},
        {"wal_frames", std::to_string(wal_frames)},
        {"duration_ms", std::to_string(duration_ms)}
    };
    
    jlog(StructuredLogger::INFO, "database", "checkpoint",
         db_name + " WAL checkpoint: " + std::to_string(wal_frames) + " frames", extra);
}

void DatabaseLogger::integrity_check(const std::string& db_name, bool passed) {
    std::map<std::string, std::string> extra{
        {"db_name", db_name},
        {"passed", passed ? "true" : "false"}
    };
    
    auto level = passed ? StructuredLogger::INFO : StructuredLogger::ERROR;
    
    jlog(level, "database", "integrity_check",
         db_name + " integrity check " + (passed ? "PASSED" : "FAILED"), extra);
}

// Blockchain Logger Implementation
void BlockchainLogger::new_block(int64_t height, const std::string& hash,
                               int tx_count, double validation_time_ms) {
    std::map<std::string, std::string> extra{
        {"block_height", std::to_string(height)},
        {"block_hash", hash},
        {"tx_count", std::to_string(tx_count)},
        {"validation_ms", std::to_string(validation_time_ms)}
    };
    
    jlog(StructuredLogger::INFO, "blockchain", "new_block",
         "New block " + std::to_string(height) + " with " + std::to_string(tx_count) + " txs", extra);
}

void BlockchainLogger::reorg(int64_t old_height, int64_t new_height,
                           const std::string& new_tip) {
    std::map<std::string, std::string> extra{
        {"old_height", std::to_string(old_height)},
        {"new_height", std::to_string(new_height)},
        {"new_tip", new_tip}
    };
    
    jlog(StructuredLogger::WARN, "blockchain", "reorg",
         "Chain reorg from " + std::to_string(old_height) + " to " + std::to_string(new_height), extra);
}

void BlockchainLogger::sync_progress(int64_t current_height, int64_t target_height,
                                   double sync_rate_blocks_per_sec) {
    std::map<std::string, std::string> extra{
        {"current_height", std::to_string(current_height)},
        {"target_height", std::to_string(target_height)},
        {"sync_rate_bps", std::to_string(sync_rate_blocks_per_sec)},
        {"progress_pct", std::to_string((double)current_height / target_height * 100.0)}
    };
    
    jlog(StructuredLogger::INFO, "blockchain", "sync_progress",
         "Sync progress: " + std::to_string(current_height) + "/" + std::to_string(target_height), extra);
}

// Mining Logger Implementation
void MiningLogger::start(int threads, const std::string& mining_address) {
    std::map<std::string, std::string> extra{
        {"threads", std::to_string(threads)},
        {"mining_address", mining_address}
    };
    
    jlog(StructuredLogger::INFO, "mining", "start",
         "Mining started with " + std::to_string(threads) + " threads", extra);
}

void MiningLogger::stop(double total_runtime_sec, int64_t total_hashes) {
    std::map<std::string, std::string> extra{
        {"runtime_sec", std::to_string(total_runtime_sec)},
        {"total_hashes", std::to_string(total_hashes)},
        {"avg_hashrate", std::to_string(total_hashes / total_runtime_sec)}
    };
    
    jlog(StructuredLogger::INFO, "mining", "stop",
         "Mining stopped after " + std::to_string(total_runtime_sec) + "s", extra);
}

void MiningLogger::block_found(int64_t height, const std::string& hash,
                             double difficulty, int64_t nonce) {
    std::map<std::string, std::string> extra{
        {"block_height", std::to_string(height)},
        {"block_hash", hash},
        {"difficulty", std::to_string(difficulty)},
        {"nonce", std::to_string(nonce)}
    };
    
    jlog(StructuredLogger::INFO, "mining", "block_found",
         "Block found! Height " + std::to_string(height), extra);
}

void MiningLogger::hashrate_update(double hashrate_hps, int active_threads) {
    std::map<std::string, std::string> extra{
        {"hashrate_hps", std::to_string(hashrate_hps)},
        {"active_threads", std::to_string(active_threads)}
    };
    
    jlog(StructuredLogger::DEBUG, "mining", "hashrate_update",
         "Hashrate: " + std::to_string(hashrate_hps) + " H/s", extra);
}

// Stub implementations for global state functions
// These should be implemented to access your actual global state
std::string GetActiveNetworkHRP() {
    return "din"; // Default mainnet
}

int64_t GetBestHeight() {
    return 0; // Implement to return actual chain height
}

std::string GetBestHashHex() {
    return ""; // Implement to return actual best block hash
}

int32_t GetActiveWebSocketConnections() {
    return 0; // Implement to return actual WS connection count
}

} // namespace logging
} // namespace dinero
