//SPDX-License-Identifier: MIT
// LoggerRouter - Unified Log Aggregation with Real-Time Streaming

#include "common/logger_router.h"
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <iomanip>
#include <filesystem>

namespace dinero {

// Helper: Get current timestamp in ISO8601 format
static std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// Helper: Get current thread ID as string
[[maybe_unused]] static std::string getCurrentThreadId() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

// Helper: Convert LogLevel to string
[[maybe_unused]] static std::string logLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "debug";
        case LogLevel::INFO: return "info";
        case LogLevel::WARNING: return "warning";
        case LogLevel::ERROR: return "error";
        default: return "unknown";
    }
}

// Helper: Convert string to LogLevel
static LogLevel stringToLogLevel(const std::string& level) {
    if (level == "debug" || level == "DEBUG") return LogLevel::DEBUG;
    if (level == "info" || level == "INFO") return LogLevel::INFO;
    if (level == "warning" || level == "WARNING") return LogLevel::WARNING;
    if (level == "error" || level == "ERROR") return LogLevel::ERROR;
    return LogLevel::INFO;  // Default
}

// LogEntry implementation
std::string LogEntry::toJson() const {
    Json::Value json;
    json["timestamp"] = timestamp;
    json["level"] = level;
    json["service"] = service;
    json["message"] = message;
    json["thread_id"] = thread_id;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";  // Compact JSON
    return Json::writeString(builder, json);
}

// LoggerRouter implementation
LoggerRouter::LoggerRouter(const std::string& datadir, size_t buffer_size)
    : datadir_(datadir)
    , buffer_size_(buffer_size)
    , running_(false)  // Step 5: Atomic bool initialized to false
    , next_subscriber_id_(1)
{
    // Initialize log file paths
    log_files_ = {
        datadir_ + "/wallet.log",
        datadir_ + "/p2p.log",
        datadir_ + "/mining.log",
        datadir_ + "/mempool.log",
        datadir_ + "/dinero.log"  // Global log file
    };
}

LoggerRouter::~LoggerRouter() {
    stop();
}

void LoggerRouter::start() {
    if (running_) return;

    running_ = true;

    // Start background thread for tailing log files
    tail_thread_ = std::make_unique<std::thread>([this]() {
        tailLogFiles();
    });
}

void LoggerRouter::stop() {
    if (!running_) return;

    running_ = false;

    // Step 5: Wake up background thread for graceful shutdown
    queue_cv_.notify_all();

    if (tail_thread_ && tail_thread_->joinable()) {
        tail_thread_->join();
    }
}

int LoggerRouter::subscribe(LogCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    int id = next_subscriber_id_++;
    subscribers_[id] = callback;
    return id;
}

void LoggerRouter::unsubscribe(int subscription_id) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(subscription_id);
}

// Step 1: Push log entry directly to router (low-latency path)
void LoggerRouter::pushLogEntry(const LogEntry& entry) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        log_queue_.push(entry);
    }
    // Wake up background thread to process the entry
    queue_cv_.notify_one();
}

// Step 4: Check and rotate log files if they exceed 50MB
void LoggerRouter::checkAndRotateLogs() {
    constexpr size_t MAX_LOG_SIZE = 50 * 1024 * 1024;  // 50MB in bytes

    for (const auto& log_file : log_files_) {
        try {
            // Check if file exists and get size
            if (!std::filesystem::exists(log_file)) {
                continue;
            }

            size_t file_size = std::filesystem::file_size(log_file);

            if (file_size >= MAX_LOG_SIZE) {
                // Generate timestamp for rotated file
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                std::stringstream ss;
                ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
                std::string timestamp = ss.str();

                // Create rotated filename: wallet.log -> wallet.log.20251115_160000
                std::string rotated_file = log_file + "." + timestamp;

                // Rename current log file to rotated name
                std::filesystem::rename(log_file, rotated_file);

                // Note: File will be automatically recreated by JsonLogger on next write
                // This is a simple rotation strategy. For production, consider:
                // - Keeping N most recent rotated files
                // - Compressing rotated files with gzip
                // - Deleting very old rotated files
            }
        } catch (const std::exception& e) {
            // Silently ignore rotation errors to avoid blocking log processing
            // In production, you might want to log this to a separate error log
            (void)e;  // Suppress unused warning
        }
    }
}

std::vector<LogEntry> LoggerRouter::getRecentLogs(
    const std::string& service,
    LogLevel min_level,
    size_t limit
) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    std::vector<LogEntry> result;

    // Iterate through ring buffer (newest first)
    for (auto it = log_buffer_.rbegin(); it != log_buffer_.rend() && result.size() < limit; ++it) {
        const LogEntry& entry = *it;

        // Filter by service
        if (!service.empty() && entry.service != service) {
            continue;
        }

        // Filter by log level
        LogLevel entry_level = stringToLogLevel(entry.level);
        if (entry_level < min_level) {
            continue;
        }

        result.push_back(entry);
    }

    return result;
}

std::vector<LogEntry> LoggerRouter::filterLogs(
    const std::string& service,
    LogLevel min_level,
    const std::string& thread_id,
    size_t limit
) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    std::vector<LogEntry> result;

    // Iterate through ring buffer (newest first)
    for (auto it = log_buffer_.rbegin(); it != log_buffer_.rend() && result.size() < limit; ++it) {
        const LogEntry& entry = *it;

        // Filter by service
        if (!service.empty() && entry.service != service) {
            continue;
        }

        // Filter by log level
        LogLevel entry_level = stringToLogLevel(entry.level);
        if (entry_level < min_level) {
            continue;
        }

        // Filter by thread_id
        if (!thread_id.empty() && entry.thread_id != thread_id) {
            continue;
        }

        result.push_back(entry);
    }

    return result;
}

std::string LoggerRouter::getLogsJson(
    const std::string& since_timestamp,
    const std::string& service,
    LogLevel min_level
) {
    auto logs = getRecentLogs(service, min_level, buffer_size_);

    Json::Value json_array(Json::arrayValue);
    for (const auto& entry : logs) {
        // Filter by timestamp if specified
        if (!since_timestamp.empty() && entry.timestamp <= since_timestamp) {
            continue;
        }

        Json::Value json_entry;
        json_entry["timestamp"] = entry.timestamp;
        json_entry["level"] = entry.level;
        json_entry["service"] = entry.service;
        json_entry["message"] = entry.message;
        json_entry["thread_id"] = entry.thread_id;
        json_array.append(json_entry);
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";  // Pretty JSON for HTTP responses
    return Json::writeString(builder, json_array);
}

void LoggerRouter::tailLogFiles() {
    // File positions for each log file (for tailing)
    std::map<std::string, std::streampos> file_positions;

    // Initialize file positions to end of file (only tail new logs)
    for (const auto& log_file : log_files_) {
        std::ifstream file(log_file, std::ios::ate);  // Open at end
        if (file.is_open()) {
            file_positions[log_file] = file.tellg();
        }
    }

    // Step 4: Track last rotation check time
    auto last_rotation_check = std::chrono::steady_clock::now();
    constexpr auto ROTATION_CHECK_INTERVAL = std::chrono::seconds(60);  // Check every 60 seconds

    // Background loop: process queue + tail log files
    while (running_) {
        // Step 1: Process all queued log entries (low-latency path)
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            while (!log_queue_.empty()) {
                LogEntry entry = log_queue_.front();
                log_queue_.pop();
                lock.unlock();  // Release lock before processing

                emitLogEntry(entry);

                lock.lock();  // Reacquire lock for next iteration
            }
        }

        // Step 2: Tail log files (fallback for external processes)
        for (const auto& log_file : log_files_) {
            // Extract service name from filename (e.g., "wallet.log" → "wallet")
            std::filesystem::path path(log_file);
            std::string filename = path.filename().string();
            std::string service = filename.substr(0, filename.find('.'));

            // Open file and seek to last position
            std::ifstream file(log_file);
            if (!file.is_open()) continue;

            file.seekg(file_positions[log_file]);

            // Read new lines
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty()) continue;

                LogEntry entry;
                if (parseLogLine(line, service, entry)) {
                    emitLogEntry(entry);
                }
            }

            // Update file position
            file_positions[log_file] = file.tellg();
        }

        // Step 4: Periodically check for log rotation (every 60 seconds)
        auto now = std::chrono::steady_clock::now();
        if (now - last_rotation_check >= ROTATION_CHECK_INTERVAL) {
            checkAndRotateLogs();
            last_rotation_check = now;
        }

        // Step 1: Wait for new queue entries or timeout (100ms)
        // This allows immediate wake-up when entries are pushed, or periodic file polling
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !log_queue_.empty() || !running_;
            });
        }
    }
}

bool LoggerRouter::parseLogLine(
    const std::string& line,
    const std::string& service,
    LogEntry& entry
) {
    try {
        // Parse JSON log line
        Json::Value json;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream ss(line);

        if (!Json::parseFromStream(builder, ss, &json, &errors)) {
            return false;  // Not a valid JSON line
        }

        // Extract fields
        entry.timestamp = json.get("timestamp", getCurrentTimestamp()).asString();
        entry.level = json.get("level", "info").asString();
        entry.service = service;  // Use service from filename
        entry.message = json.get("message", "").asString();
        entry.thread_id = json.get("thread_id", "").asString();  // Optional thread_id

        return true;

    } catch (const std::exception&) {
        return false;  // Parsing error
    }
}

void LoggerRouter::emitLogEntry(const LogEntry& entry) {
    // Add to ring buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        log_buffer_.push_back(entry);

        // Maintain ring buffer size
        if (log_buffer_.size() > buffer_size_) {
            log_buffer_.pop_front();
        }
    }

    // Notify all subscribers
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        for (const auto& [id, callback] : subscribers_) {
            try {
                callback(entry);
            } catch (...) {
                // Ignore subscriber errors
            }
        }
    }
}

} // namespace dinero
