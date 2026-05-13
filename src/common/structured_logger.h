#pragma once

#include <string>
#include <map>
#include <chrono>
#include <memory>
#include "compat/jsoncpp_compat.h"

namespace dinero {
namespace logging {

/**
 * Structured JSON logger for production monitoring
 * Provides consistent schema across all components
 */
class StructuredLogger {
public:
    enum Level {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        ERROR = 3,
        FATAL = 4
    };

    struct Context {
        std::string hrp = "din";
        int64_t height = 0;
        std::string best_hash = "";
        int32_t ws_connections = 0;
        double mem_rss_mb = 0.0;
        
        // Update context from global state
        void refresh();
    };

    static StructuredLogger& getInstance();
    
    void setContext(const Context& ctx) { context_ = ctx; }
    void updateContext() { context_.refresh(); }
    
    // Main logging interface
    void log(Level level, const std::string& component, const std::string& event, 
             const std::string& message, const std::map<std::string, std::string>& extra = {});
    
    // Convenience methods
    void debug(const std::string& comp, const std::string& event, const std::string& msg,
               const std::map<std::string, std::string>& extra = {}) {
        log(DEBUG, comp, event, msg, extra);
    }
    
    void info(const std::string& comp, const std::string& event, const std::string& msg,
              const std::map<std::string, std::string>& extra = {}) {
        log(INFO, comp, event, msg, extra);
    }
    
    void warn(const std::string& comp, const std::string& event, const std::string& msg,
              const std::map<std::string, std::string>& extra = {}) {
        log(WARN, comp, event, msg, extra);
    }
    
    void error(const std::string& comp, const std::string& event, const std::string& msg,
               const std::map<std::string, std::string>& extra = {}) {
        log(ERROR, comp, event, msg, extra);
    }
    
    void fatal(const std::string& comp, const std::string& event, const std::string& msg,
               const std::map<std::string, std::string>& extra = {}) {
        log(FATAL, comp, event, msg, extra);
    }

private:
    StructuredLogger() = default;
    Context context_;
    
    std::string levelToString(Level level) const;
    std::string getCurrentTimestamp() const;
};

// Global convenience function
inline auto jlog = [](StructuredLogger::Level level, const std::string& comp, 
                      const std::string& event, const std::string& msg,
                      const std::map<std::string, std::string>& extra = {}) {
    StructuredLogger::getInstance().log(level, comp, event, msg, extra);
};

// Component-specific loggers
class RpcLogger {
public:
    static void request(const std::string& method, const std::string& client_ip, 
                       int http_code, double duration_ms);
    static void error(const std::string& method, const std::string& error_msg, 
                     int http_code = 500);
};

class WebSocketLogger {
public:
    static void connection(const std::string& event, const std::string& client_ip,
                          int total_connections);
    static void message(const std::string& type, int message_size, 
                       const std::string& client_ip = "");
    static void error(const std::string& error_msg, const std::string& client_ip = "");
};

class DatabaseLogger {
public:
    static void operation(const std::string& db_name, const std::string& operation,
                         double duration_ms, bool success = true);
    static void checkpoint(const std::string& db_name, int wal_frames, 
                          double duration_ms);
    static void integrity_check(const std::string& db_name, bool passed);
};

class BlockchainLogger {
public:
    static void new_block(int64_t height, const std::string& hash, 
                         int tx_count, double validation_time_ms);
    static void reorg(int64_t old_height, int64_t new_height, 
                     const std::string& new_tip);
    static void sync_progress(int64_t current_height, int64_t target_height,
                            double sync_rate_blocks_per_sec);
};

class MiningLogger {
public:
    static void start(int threads, const std::string& mining_address);
    static void stop(double total_runtime_sec, int64_t total_hashes);
    static void block_found(int64_t height, const std::string& hash, 
                           double difficulty, int64_t nonce);
    static void hashrate_update(double hashrate_hps, int active_threads);
};

} // namespace logging
} // namespace dinero
