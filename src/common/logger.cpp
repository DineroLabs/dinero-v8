#include "common/logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>

// Phase 8.5: NO <chrono> include - wall time is FORBIDDEN
// Logs are now deterministic (no timestamps)

namespace dinero {

Logger::Logger() : log_level_(LogLevel::INFO) {
}

Logger::~Logger() {
    // Safe shutdown: let RAII handle the stream cleanup
    // Don't call close() explicitly during global destruction
    // The std::ofstream destructor will handle cleanup safely
}

void Logger::setLogLevel(LogLevel level) {
    log_level_ = level;
}

void Logger::setLogFile(const std::string& filename) {
    // Safe file handling: let RAII manage the stream
    // Close existing file if open, then open new one
    if (log_file_.is_open()) {
        log_file_.close();
    }
    log_file_.open(filename, std::ios::app);
}

std::string Logger::getCurrentTimestamp() {
    // Phase 8.5: Deterministic logging - NO wall clock timestamps
    // For deterministic replay, logs must not contain non-deterministic data
    // Timestamps removed - sequence/causality is preserved by log order
    //
    // Future: Could use event sequence number or block height here
    return "";
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void Logger::shutdown() {
    shutdown_flag_.store(true);
    // Close the file stream to prevent UBSan errors during shutdown
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    // Check shutdown flag first
    if (shutdown_flag_.load()) {
        return;  // Logger is shut down, don't log
    }
    
    if (level < log_level_) {
        return;
    }
    
    std::string timestamp = getCurrentTimestamp();
    std::string level_str = levelToString(level);
    // Phase 8.5: Skip timestamp if empty (deterministic logging)
    std::string log_line = timestamp.empty()
        ? "[" + level_str + "] " + message
        : "[" + timestamp + "] [" + level_str + "] " + message;
    
    // Always output to console
    if (level >= LogLevel::ERROR) {
        std::cerr << log_line << std::endl;
    } else {
        std::cout << log_line << std::endl;
    }
    
    // Also output to file if configured and not shut down
    if (log_file_.is_open() && !shutdown_flag_.load()) {
        log_file_ << log_line << std::endl;
        log_file_.flush();
    }
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

// Global logger instance
Logger g_logger;

} // namespace dinero 