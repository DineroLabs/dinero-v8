#pragma once
#include "common/ilogger.h"
#include <string>
#include <mutex>
#include <fstream>

namespace dinero {

/**
 * JsonLogger - Structured JSON logging implementation
 *
 * Outputs logs in JSON format for machine parsing, dashboards, and analytics.
 * Each log entry is a single-line JSON object with:
 * - timestamp: ISO8601 format
 * - level: debug/info/warning/error
 * - message: log message text
 * - service: optional service name (if set)
 * - thread_id: thread identifier
 *
 * Example output:
 * {"timestamp":"2025-01-15T10:30:45.123Z","level":"info","service":"wallet","message":"Wallet initialized","thread_id":"0x123456"}
 *
 * Benefits:
 * - Machine-parseable for log aggregators (ELK, Loki, Splunk)
 * - Structured fields enable filtering and analytics
 * - Single-line format simplifies parsing
 * - Compatible with standard JSON streaming tools (jq, etc.)
 *
 * Thread-safe: Uses mutex for concurrent logging
 */
class JsonLogger : public ILogger {
public:
    /**
     * Create JsonLogger with file output
     * @param filename Path to JSON log file
     * @param service_name Optional service name to include in all log entries
     */
    explicit JsonLogger(const std::string& filename = "", const std::string& service_name = "");

    ~JsonLogger() override;

    // ILogger interface implementation
    void setLogLevel(LogLevel level) override;
    void setLogFile(const std::string& filename) override;
    void shutdown() override;

    void log(LogLevel level, const std::string& message) override;
    void debug(const std::string& message) override;
    void info(const std::string& message) override;
    void warning(const std::string& message) override;
    void error(const std::string& message) override;

    /**
     * Set service name to be included in all log entries
     * Useful for per-service JsonLogger instances
     */
    void setServiceName(const std::string& service_name);

private:
    std::mutex mutex_;
    std::ofstream file_;
    std::string filename_;
    std::string service_name_;  // Optional service context
    LogLevel current_level_ = LogLevel::INFO;

    // Format a log entry as JSON
    std::string formatJson(LogLevel level, const std::string& message) const;

    // Get ISO8601 timestamp
    std::string getTimestamp() const;

    // Get log level as string
    static std::string levelToString(LogLevel level);

    // Escape JSON string (handle quotes, newlines, etc.)
    static std::string escapeJson(const std::string& str);
};

} // namespace dinero
