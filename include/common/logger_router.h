#pragma once

#include "common/ilogger.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <deque>
#include <thread>
#include <map>
#include <queue>
#include <condition_variable>
#include <atomic>

namespace dinero {

/**
 * Log Entry - Structured log message with metadata
 */
struct LogEntry {
    std::string timestamp;      // ISO8601 timestamp
    std::string level;          // "debug", "info", "warning", "error"
    std::string service;        // "wallet", "p2p", "mining", "mempool", "global"
    std::string message;        // Log message content
    std::string thread_id;      // Thread ID (for debugging concurrent operations)

    // Convert to JSON string for streaming
    std::string toJson() const;
};

/**
 * LoggerRouter - Unified Log Aggregator
 *
 * Aggregates logs from multiple sources (file-based loggers) and provides:
 * - Real-time log streaming via WebSocket/HTTP
 * - Centralized log viewing across all services
 * - Filtering by service and log level
 * - Ring buffer for recent logs
 *
 * Architecture:
 *   Per-Service JsonLoggers → Log Files → LoggerRouter (tails files) → WebSocket/HTTP Streams
 *
 * Usage:
 *   LoggerRouter router(datadir);
 *   router.start();  // Start tailing log files
 *   router.subscribe([](const LogEntry& entry) {
 *       // Send to WebSocket clients
 *   });
 */
class LoggerRouter {
public:
    using LogCallback = std::function<void(const LogEntry&)>;

    /**
     * Constructor
     * @param datadir Directory containing log files (wallet.log, p2p.log, etc.)
     * @param buffer_size Maximum number of recent logs to keep in memory (default: 1000)
     */
    explicit LoggerRouter(const std::string& datadir, size_t buffer_size = 1000);

    ~LoggerRouter();

    /**
     * Start tailing log files and aggregating logs
     */
    void start();

    /**
     * Stop log aggregation
     */
    void stop();

    /**
     * Subscribe to real-time log stream
     * @param callback Function called for each new log entry
     * @return Subscription ID (for unsubscribing)
     */
    int subscribe(LogCallback callback);

    /**
     * Unsubscribe from log stream
     * @param subscription_id ID returned from subscribe()
     */
    void unsubscribe(int subscription_id);

    /**
     * Get recent logs from ring buffer
     * @param service Filter by service (empty = all services)
     * @param level Minimum log level (DEBUG, INFO, WARNING, ERROR)
     * @param limit Maximum number of logs to return
     * @return Vector of log entries
     */
    std::vector<LogEntry> getRecentLogs(
        const std::string& service = "",
        LogLevel min_level = LogLevel::DEBUG,
        size_t limit = 100
    );

    /**
     * Filter logs with advanced criteria (for logs.filter RPC)
     * @param service Filter by service (empty = all services)
     * @param level Minimum log level (DEBUG, INFO, WARNING, ERROR)
     * @param thread_id Filter by thread ID (empty = all threads)
     * @param limit Maximum number of logs to return
     * @return Vector of log entries
     */
    std::vector<LogEntry> filterLogs(
        const std::string& service = "",
        LogLevel min_level = LogLevel::DEBUG,
        const std::string& thread_id = "",
        size_t limit = 100
    );

    /**
     * Get aggregated logs from all services (for HTTP endpoint)
     * @param since_timestamp Only return logs after this timestamp
     * @param service Filter by service
     * @param level Minimum log level
     * @return JSON array of log entries
     */
    std::string getLogsJson(
        const std::string& since_timestamp = "",
        const std::string& service = "",
        LogLevel min_level = LogLevel::DEBUG
    );

    /**
     * Push log entry directly to router (low-latency path)
     * This allows loggers to bypass file I/O and push entries directly to the router
     * @param entry Log entry to push
     */
    void pushLogEntry(const LogEntry& entry);

    /**
     * Check and rotate log files if they exceed size limits
     * Step 4: Automatic log rotation at 50MB per file
     */
    void checkAndRotateLogs();

private:
    std::string datadir_;
    size_t buffer_size_;
    std::atomic<bool> running_;  // Step 5: Memory-safe shutdown with atomic flag

    // Ring buffer for recent logs
    std::deque<LogEntry> log_buffer_;
    std::mutex buffer_mutex_;

    // Subscribers
    std::map<int, LogCallback> subscribers_;
    int next_subscriber_id_;
    std::mutex subscribers_mutex_;

    // Log file paths
    std::vector<std::string> log_files_;

    // Background thread for tailing log files
    std::unique_ptr<std::thread> tail_thread_;

    // Step 1: Thread-safe queue for low-latency log input
    std::queue<LogEntry> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    /**
     * Background thread function - tails log files and emits log entries
     */
    void tailLogFiles();

    /**
     * Parse a JSON log line into LogEntry
     */
    bool parseLogLine(const std::string& line, const std::string& service, LogEntry& entry);

    /**
     * Add log entry to ring buffer and notify subscribers
     */
    void emitLogEntry(const LogEntry& entry);
};

} // namespace dinero
