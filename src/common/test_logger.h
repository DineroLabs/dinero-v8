#pragma once
#include "common/ilogger.h"
#include <mutex>
#include <vector>
#include <string>

namespace dinero {

/**
 * TestLogger - In-Memory Logger for Unit Testing
 *
 * Thread-safe logger that captures log messages in memory
 * for deterministic assertions in unit tests.
 *
 * Example usage:
 *   TestLogger test_logger;
 *   DaemonContext ctx;
 *   ctx.wallet_logger = &test_logger;
 *
 *   // ... perform operations ...
 *
 *   assert(test_logger.contains("WalletService"));
 *   assert(test_logger.count("ERROR") == 0);
 */
class TestLogger : public ILogger {
public:
    void info(const std::string& msg) override  { add("INFO", msg); }
    void warn(const std::string& msg) override  { add("WARN", msg); }
    void error(const std::string& msg) override { add("ERROR", msg); }
    void debug(const std::string& msg) override { add("DEBUG", msg); }

    /**
     * Clear all captured messages
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        messages_.clear();
    }

    /**
     * Get all captured messages (thread-safe copy)
     */
    std::vector<std::string> messages() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return messages_;
    }

    /**
     * Check if any message contains the given substring
     */
    bool contains(const std::string& substring) const {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& msg : messages_) {
            if (msg.find(substring) != std::string::npos)
                return true;
        }
        return false;
    }

    /**
     * Count messages matching a given substring
     */
    size_t count(const std::string& substring) const {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t cnt = 0;
        for (const auto& msg : messages_) {
            if (msg.find(substring) != std::string::npos)
                cnt++;
        }
        return cnt;
    }

    /**
     * Get messages matching a specific level
     */
    std::vector<std::string> getByLevel(const std::string& level) const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::string> result;
        std::string prefix = level + ": ";
        for (const auto& msg : messages_) {
            if (msg.find(prefix) == 0) {
                result.push_back(msg);
            }
        }
        return result;
    }

    /**
     * Get the number of captured messages
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return messages_.size();
    }

    /**
     * Check if no messages were captured
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return messages_.empty();
    }

private:
    void add(const std::string& level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        messages_.push_back(level + ": " + msg);
    }

    mutable std::mutex mtx_;
    std::vector<std::string> messages_;
};

} // namespace dinero
