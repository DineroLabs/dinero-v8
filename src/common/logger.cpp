#include "common/logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>   // issue #224: log rotation (rename/remove/file_size)
#include <system_error>

// Phase 8.5: NO <chrono> include - wall time is FORBIDDEN
// Logs are now deterministic (no timestamps)

namespace dinero {

// issue #224: the logger instance that currently owns a file destination, so a
// signal handler (SIGHUP) can request a reopen without touching the Logger
// object directly. atomic<Logger*> load is async-signal-safe.
static std::atomic<Logger*> g_file_logger{nullptr};

Logger::Logger() : log_level_(LogLevel::INFO) {
}

Logger::~Logger() {
    // issue #224: deregister so a late SIGHUP can't requestReopen() on a
    // destroyed logger (e.g. LoggerService's owned Logger is torn down).
    Logger* self = this;
    g_file_logger.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
    // Safe shutdown: let RAII handle the stream cleanup
    // Don't call close() explicitly during global destruction
    // The std::ofstream destructor will handle cleanup safely
}

void Logger::setLogLevel(LogLevel level) {
    log_level_ = level;
}

void Logger::setLogFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    // Safe file handling: let RAII manage the stream
    // Close existing file if open, then open new one
    if (log_file_.is_open()) {
        log_file_.close();
    }
    log_path_ = filename;
    log_file_.open(filename, std::ios::app);
    // issue #224: seed the rotation byte counter from the existing file size so
    // appends to an already-large log still rotate at the configured threshold.
    std::error_code ec;
    auto existing = std::filesystem::file_size(filename, ec);
    bytes_written_ = ec ? 0 : static_cast<std::size_t>(existing);
    // Register as the active file logger so SIGHUP can reach us.
    g_file_logger.store(this, std::memory_order_release);
}

void Logger::setRotation(std::size_t max_bytes, std::uint32_t max_files) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    max_log_bytes_ = max_bytes;                  // 0 disables rotation
    max_log_files_ = max_files == 0 ? 1 : max_files;
}

void Logger::requestReopen() {
    // Async-signal-safe: just flip the flag; the reopen happens on the next write.
    reopen_requested_.store(true, std::memory_order_relaxed);
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
    // issue #224: deregister as the active file logger before closing.
    Logger* self = this;
    g_file_logger.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(file_mutex_);
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
    
    // Also output to file if configured and not shut down. issue #224: the
    // file write, reopen, and rotation are serialized under file_mutex_ so
    // concurrent log() calls can't tear a rotation.
    if (!shutdown_flag_.load()) {
        std::lock_guard<std::mutex> lock(file_mutex_);
        if (reopen_requested_.exchange(false, std::memory_order_relaxed)) {
            reopenLocked_();
        }
        if (log_file_.is_open()) {
            log_file_ << log_line << '\n';
            log_file_.flush();
            bytes_written_ += log_line.size() + 1;  // +1 for newline
            if (max_log_bytes_ > 0 && bytes_written_ >= max_log_bytes_) {
                rotateLocked_();
            }
        }
    }
}

void Logger::reopenLocked_() {
    // Caller holds file_mutex_. Used after external logrotate renamed/removed the
    // file: drop the stale handle and reopen the configured path.
    if (log_path_.empty()) {
        return;
    }
    if (log_file_.is_open()) {
        log_file_.close();
    }
    log_file_.open(log_path_, std::ios::app);
    std::error_code ec;
    auto existing = std::filesystem::file_size(log_path_, ec);
    bytes_written_ = ec ? 0 : static_cast<std::size_t>(existing);
}

void Logger::rotateLocked_() {
    // Caller holds file_mutex_. debug.log -> debug.log.1 -> ... -> .max_files
    // (oldest dropped), then a fresh debug.log is opened.
    if (log_path_.empty()) {
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (log_file_.is_open()) {
        log_file_.close();
    }
    // Drop the oldest archive.
    fs::remove(log_path_ + "." + std::to_string(max_log_files_), ec);
    // Shift archives up: .(N-1) -> .N, ..., .1 -> .2
    for (std::uint32_t i = max_log_files_; i > 1; --i) {
        const std::string from = log_path_ + "." + std::to_string(i - 1);
        const std::string to = log_path_ + "." + std::to_string(i);
        if (fs::exists(from, ec)) {
            fs::rename(from, to, ec);
        }
    }
    // Active -> .1, then open a fresh active file.
    fs::rename(log_path_, log_path_ + ".1", ec);
    log_file_.open(log_path_, std::ios::trunc);
    bytes_written_ = 0;
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

// issue #224: signal-safe entry point for SIGHUP / external logrotate.
void requestLogReopen() {
    if (Logger* active = g_file_logger.load(std::memory_order_acquire)) {
        active->requestReopen();
    }
}

} // namespace dinero 