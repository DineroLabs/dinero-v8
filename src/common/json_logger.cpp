#include "common/json_logger.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <thread>
#include <iostream>

namespace dinero {

JsonLogger::JsonLogger(const std::string& filename, const std::string& service_name)
    : filename_(filename), service_name_(service_name) {
    if (!filename_.empty()) {
        file_.open(filename_, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "Warning: Failed to open JSON log file: " << filename_ << std::endl;
        }
    }
}

JsonLogger::~JsonLogger() {
    shutdown();
}

void JsonLogger::setLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_level_ = level;
}

void JsonLogger::setLogFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Close existing file
    if (file_.is_open()) {
        file_.close();
    }

    filename_ = filename;

    // Open new file
    if (!filename_.empty()) {
        file_.open(filename_, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "Warning: Failed to open JSON log file: " << filename_ << std::endl;
        }
    }
}

void JsonLogger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void JsonLogger::log(LogLevel level, const std::string& message) {
    // Check log level filtering
    if (level < current_level_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::string json_entry = formatJson(level, message);

    // Write to file if available
    if (file_.is_open()) {
        file_ << json_entry << std::endl;
        file_.flush();  // Ensure immediate write for real-time monitoring
    } else {
        // Fallback to stdout
        std::cout << json_entry << std::endl;
    }
}

void JsonLogger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void JsonLogger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void JsonLogger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void JsonLogger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void JsonLogger::setServiceName(const std::string& service_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    service_name_ = service_name;
}

std::string JsonLogger::formatJson(LogLevel level, const std::string& message) const {
    std::ostringstream oss;

    oss << "{";

    // Timestamp
    oss << "\"timestamp\":\"" << getTimestamp() << "\"";

    // Log level
    oss << ",\"level\":\"" << levelToString(level) << "\"";

    // Service name (if set)
    if (!service_name_.empty()) {
        oss << ",\"service\":\"" << escapeJson(service_name_) << "\"";
    }

    // Message
    oss << ",\"message\":\"" << escapeJson(message) << "\"";

    // Thread ID
    std::ostringstream thread_oss;
    thread_oss << std::this_thread::get_id();
    oss << ",\"thread_id\":\"" << thread_oss.str() << "\"";

    oss << "}";

    return oss.str();
}

std::string JsonLogger::getTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &now_time_t);
#else
    gmtime_r(&now_time_t, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << now_ms.count();
    oss << 'Z';

    return oss.str();
}

std::string JsonLogger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "debug";
        case LogLevel::INFO:    return "info";
        case LogLevel::WARNING: return "warning";
        case LogLevel::ERROR:   return "error";
        default:                return "unknown";
    }
}

std::string JsonLogger::escapeJson(const std::string& str) {
    std::ostringstream oss;

    for (char c : str) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                // Print non-control characters as-is
                if (c >= 32 && c < 127) {
                    oss << c;
                } else {
                    // Escape control characters as \uXXXX
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                }
                break;
        }
    }

    return oss.str();
}

} // namespace dinero
