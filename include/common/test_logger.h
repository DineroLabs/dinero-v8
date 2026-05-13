#pragma once

#include "common/ilogger.h"  // Includes logger.h for LogLevel
#include <vector>
#include <string>
#include <mutex>

namespace dinero {

/**
 * NullLogger - Discards all log messages
 *
 * Use this in tests where logging output is not needed.
 * All methods are no-ops, making tests faster and cleaner.
 *
 * Example:
 *   NullLogger logger;
 *   MyService service(logger);  // Service logs nothing during test
 *   // Test service functionality without log spam
 */
class NullLogger : public ILogger {
public:
    NullLogger() = default;
    ~NullLogger() override = default;

    // All methods are no-ops
    void setLogLevel(LogLevel level) override {}
    void setLogFile(const std::string& filename) override {}
    void shutdown() override {}
    void log(LogLevel level, const std::string& message) override {}
    void debug(const std::string& message) override {}
    void info(const std::string& message) override {}
    void warning(const std::string& message) override {}
    void error(const std::string& message) override {}
};

/**
 * TestLogger - Captures log messages for test assertions
 *
 * Use this in tests where you need to verify that specific messages
 * were logged at specific levels.
 *
 * Example:
 *   TestLogger logger;
 *   MyService service(logger);
 *   service.doSomething();
 *
 *   // Verify logging behavior
 *   ASSERT_EQ(logger.messageCount(), 2);
 *   ASSERT_TRUE(logger.hasMessage(LogLevel::INFO, "Started operation"));
 *   ASSERT_TRUE(logger.hasMessage(LogLevel::ERROR, "Operation failed"));
 *
 * Thread Safety:
 * - All methods are thread-safe (uses mutex internally)
 */
class TestLogger : public ILogger {
public:
    struct LogEntry {
        LogLevel level;
        std::string message;

        LogEntry(LogLevel lvl, const std::string& msg)
            : level(lvl), message(msg) {}
    };

    TestLogger() = default;
    ~TestLogger() override = default;

    // Configuration (captured but not enforced)
    void setLogLevel(LogLevel level) override {
        std::lock_guard<std::mutex> lock(mutex_);
        log_level_ = level;
    }

    void setLogFile(const std::string& filename) override {
        std::lock_guard<std::mutex> lock(mutex_);
        log_file_ = filename;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_called_ = true;
    }

    // Logging methods (capture messages)
    void log(LogLevel level, const std::string& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.emplace_back(level, message);
    }

    void debug(const std::string& message) override {
        log(LogLevel::DEBUG, message);
    }

    void info(const std::string& message) override {
        log(LogLevel::INFO, message);
    }

    void warning(const std::string& message) override {
        log(LogLevel::WARNING, message);
    }

    void error(const std::string& message) override {
        log(LogLevel::ERROR, message);
    }

    // Test assertion helpers
    size_t messageCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    size_t messageCount(LogLevel level) const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& entry : entries_) {
            if (entry.level == level) {
                ++count;
            }
        }
        return count;
    }

    bool hasMessage(LogLevel level, const std::string& substring) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry.level == level && entry.message.find(substring) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    bool hasMessage(const std::string& substring) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry.message.find(substring) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::vector<LogEntry> getEntries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

    std::vector<LogEntry> getEntries(LogLevel level) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<LogEntry> filtered;
        for (const auto& entry : entries_) {
            if (entry.level == level) {
                filtered.push_back(entry);
            }
        }
        return filtered;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    bool wasShutdownCalled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdown_called_;
    }

    LogLevel getLogLevel() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return log_level_;
    }

    std::string getLogFile() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return log_file_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<LogEntry> entries_;
    LogLevel log_level_ = LogLevel::INFO;
    std::string log_file_;
    bool shutdown_called_ = false;
};

} // namespace dinero
