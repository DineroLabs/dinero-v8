#include "logging/structured_logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <random>
#include <ctime>

namespace dinero {

StructuredLogger& StructuredLogger::getInstance() {
    static StructuredLogger instance;
    return instance;
}

StructuredLogger::~StructuredLogger() {
    if (log_file_ && log_file_->is_open()) {
        log_file_->close();
    }
}

void StructuredLogger::setOutputFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    if (log_file_ && log_file_->is_open()) {
        log_file_->close();
    }
    
    log_filename_ = filename;
    log_file_ = std::make_unique<std::ofstream>(filename, std::ios::app);
    
    if (!log_file_->is_open()) {
        std::cerr << "[LOGGER] Failed to open log file: " << filename << std::endl;
        file_output_ = false;
    } else {
        file_output_ = true;
    }
}

void StructuredLogger::log(StructuredLogLevel level, const std::string& message, 
                          const std::string& component, const std::string& trace_id) {
    if (level < min_level_) return;
    Json::Value log_entry = createBaseLogEntry(level, message, component, trace_id);
    writeLog(log_entry);
}

void StructuredLogger::logWithFields(StructuredLogLevel level, const std::string& message, 
                                    const Json::Value& fields,
                                    const std::string& component,
                                    const std::string& trace_id) {
    if (level < min_level_) return;
    Json::Value log_entry = createBaseLogEntry(level, message, component, trace_id);
    
    // Merge additional fields
    for (const auto& key : fields.getMemberNames()) {
        log_entry[key] = fields[key];
    }
    
    writeLog(log_entry);
}

void StructuredLogger::debug(const std::string& message, const std::string& component, 
                            const std::string& trace_id) {
    log(StructuredLogLevel::DEBUG, message, component, trace_id);
}

void StructuredLogger::info(const std::string& message, const std::string& component, 
                           const std::string& trace_id) {
    log(StructuredLogLevel::INFO, message, component, trace_id);
}

void StructuredLogger::warn(const std::string& message, const std::string& component, 
                           const std::string& trace_id) {
    log(StructuredLogLevel::WARNING, message, component, trace_id);
}

void StructuredLogger::error(const std::string& message, const std::string& component, 
                            const std::string& trace_id) {
    log(StructuredLogLevel::ERROR, message, component, trace_id);
}

void StructuredLogger::fatal(const std::string& message, const std::string& component, 
                            const std::string& trace_id) {
    log(StructuredLogLevel::CRITICAL, message, component, trace_id);
}

void StructuredLogger::logRpcRequest(const std::string& method, const std::string& trace_id,
                                    const Json::Value& params) {
    Json::Value fields;
    fields["rpc_method"] = method;
    fields["rpc_type"] = "request";
    if (!params.isNull()) {
        fields["rpc_params"] = params;
    }
    
    logWithFields(StructuredLogLevel::INFO, "RPC request: " + method, fields, "rpc", trace_id);
}

void StructuredLogger::logRpcResponse(const std::string& method, const std::string& trace_id,
                                     bool success, const std::string& error) {
    Json::Value fields;
    fields["rpc_method"] = method;
    fields["rpc_type"] = "response";
    fields["rpc_success"] = success;
    if (!error.empty()) {
        fields["rpc_error"] = error;
    }
    
    StructuredLogLevel level = success ? StructuredLogLevel::INFO : StructuredLogLevel::ERROR;
    std::string message = success ? "RPC response: " + method : "RPC error: " + method;
    
    logWithFields(level, message, fields, "rpc", trace_id);
}

void StructuredLogger::logHttpRequest(const std::string& method, const std::string& path,
                                     const std::string& trace_id, int status_code) {
    Json::Value fields;
    fields["http_method"] = method;
    fields["http_path"] = path;
    fields["http_status"] = status_code;
    
    StructuredLogLevel level = (status_code >= 200 && status_code < 300) ? StructuredLogLevel::INFO : StructuredLogLevel::WARNING;
    std::string message = method + " " + path + " -> " + std::to_string(status_code);
    
    logWithFields(level, message, fields, "http", trace_id);
}

void StructuredLogger::logMiningEvent(const std::string& event, const Json::Value& data,
                                     const std::string& trace_id) {
    Json::Value fields;
    fields["mining_event"] = event;
    if (!data.isNull()) {
        fields["mining_data"] = data;
    }
    
    logWithFields(StructuredLogLevel::INFO, "Mining event: " + event, fields, "mining", trace_id);
}

void StructuredLogger::logWalletEvent(const std::string& event, const std::string& wallet_name,
                                     const Json::Value& data, const std::string& trace_id) {
    Json::Value fields;
    fields["wallet_event"] = event;
    fields["wallet_name"] = wallet_name;
    if (!data.isNull()) {
        fields["wallet_data"] = data;
    }
    
    logWithFields(StructuredLogLevel::INFO, "Wallet event: " + event, fields, "wallet", trace_id);
}

void StructuredLogger::writeLog(const Json::Value& log_entry) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";  // Compact output
    std::string log_line = Json::writeString(builder, log_entry);
    
    // Console output
    if (console_output_) {
        std::cout << log_line << std::endl;
    }
    
    // File output
    if (file_output_ && log_file_ && log_file_->is_open()) {
        *log_file_ << log_line << std::endl;
        log_file_->flush();
    }
}

Json::Value StructuredLogger::createBaseLogEntry(StructuredLogLevel level, const std::string& message,
                                                 const std::string& component,
                                                 const std::string& trace_id) {
    Json::Value entry;
    entry["timestamp"] = getCurrentTimestamp();
    entry["level"] = levelToString(level);
    entry["message"] = message;
    entry["component"] = component.empty() ? "daemon" : component;
    entry["trace_id"] = trace_id.empty() ? generateTraceId() : trace_id;
    entry["version"] = "din.daemon.v1";
    
    return entry;
}

std::string StructuredLogger::levelToString(StructuredLogLevel level) const {
    switch (level) {
        case StructuredLogLevel::DEBUG: return "DEBUG";
        case StructuredLogLevel::INFO:  return "INFO";
        case StructuredLogLevel::WARNING:  return "WARN";
        case StructuredLogLevel::ERROR: return "ERROR";
        case StructuredLogLevel::CRITICAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string StructuredLogger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    
    return oss.str();
}

std::string StructuredLogger::generateTraceId() const {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;
    
    uint64_t trace_id = dis(gen);
    
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << trace_id;
    return oss.str();
}

} // namespace dinero
