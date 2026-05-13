#pragma once

#include "compat/jsoncpp_compat.h"
#include <string>
#include <chrono>
#include <memory>
#include <mutex>
#include <fstream>

// Windows wingdi.h defines ERROR as a macro
#ifdef ERROR
#undef ERROR
#endif

namespace dinero {

// Log levels for structured logging
enum class StructuredLogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

// Structured logger for vNext integration
class StructuredLogger {
public:
    static StructuredLogger& getInstance();
    
    // Configuration
    void setLogLevel(StructuredLogLevel level) { min_level_ = level; }
    void setOutputFile(const std::string& filename);
    void enableConsoleOutput(bool enable) { console_output_ = enable; }
    void enableFileOutput(bool enable) { file_output_ = enable; }
    
    // Core logging methods
    void log(StructuredLogLevel level, const std::string& message, 
             const std::string& component = "",
             const std::string& trace_id = "");
    
    void logWithFields(StructuredLogLevel level, const std::string& message,
                      const Json::Value& fields,
                      const std::string& component = "",
                      const std::string& trace_id = "");
    
    // Convenience methods
    void debug(const std::string& message, const std::string& component = "", 
               const std::string& trace_id = "");
    void info(const std::string& message, const std::string& component = "", 
              const std::string& trace_id = "");
    void warn(const std::string& message, const std::string& component = "", 
              const std::string& trace_id = "");
    void error(const std::string& message, const std::string& component = "", 
               const std::string& trace_id = "");
    void fatal(const std::string& message, const std::string& component = "", 
               const std::string& trace_id = "");
    
    // RPC-specific logging
    void logRpcRequest(const std::string& method, const std::string& trace_id,
                      const Json::Value& params = Json::Value::null);
    void logRpcResponse(const std::string& method, const std::string& trace_id,
                       bool success, const std::string& error = "");
    
    // HTTP-specific logging
    void logHttpRequest(const std::string& method, const std::string& path,
                       const std::string& trace_id, int status_code);
    
    // Mining-specific logging
    void logMiningEvent(const std::string& event, const Json::Value& data,
                       const std::string& trace_id = "");
    
    // Wallet-specific logging
    void logWalletEvent(const std::string& event, const std::string& wallet_name,
                       const Json::Value& data, const std::string& trace_id = "");
    
private:
    StructuredLogger() = default;
    ~StructuredLogger();
    
    // Internal logging implementation
    void writeLog(const Json::Value& log_entry);
    Json::Value createBaseLogEntry(StructuredLogLevel level, const std::string& message,
                                  const std::string& component,
                                  const std::string& trace_id);
    
    std::string levelToString(StructuredLogLevel level) const;
    std::string getCurrentTimestamp() const;
    std::string generateTraceId() const;
    
    // Configuration
    StructuredLogLevel min_level_{StructuredLogLevel::INFO};
    bool console_output_{true};
    bool file_output_{false};
    std::string log_filename_;
    
    // Output streams
    std::unique_ptr<std::ofstream> log_file_;
    std::mutex log_mutex_;
    
    // Singleton enforcement
    StructuredLogger(const StructuredLogger&) = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;
};

// Convenience macros for structured logging
#define LOG_DEBUG(msg, component, trace_id) \
    dinero::StructuredLogger::getInstance().debug(msg, component, trace_id)

#define LOG_INFO(msg, component, trace_id) \
    dinero::StructuredLogger::getInstance().info(msg, component, trace_id)

#define LOG_WARN(msg, component, trace_id) \
    dinero::StructuredLogger::getInstance().warn(msg, component, trace_id)

#define LOG_ERROR(msg, component, trace_id) \
    dinero::StructuredLogger::getInstance().error(msg, component, trace_id)

#define LOG_FATAL(msg, component, trace_id) \
    dinero::StructuredLogger::getInstance().fatal(msg, component, trace_id)

// Specialized logging macros
#define LOG_RPC_REQUEST(method, trace_id, params) \
    dinero::StructuredLogger::getInstance().logRpcRequest(method, trace_id, params)

#define LOG_RPC_RESPONSE(method, trace_id, success, error) \
    dinero::StructuredLogger::getInstance().logRpcResponse(method, trace_id, success, error)

#define LOG_HTTP_REQUEST(method, path, trace_id, status) \
    dinero::StructuredLogger::getInstance().logHttpRequest(method, path, trace_id, status)

} // namespace dinero
