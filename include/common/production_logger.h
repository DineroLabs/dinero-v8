#pragma once

#include "common/ilogger.h"
#include "common/logger.h"

namespace dinero {

/**
 * ProductionLogger - Wrapper around the global Logger singleton
 *
 * This class implements ILogger by delegating all calls to g_logger.
 * It enables gradual migration from global logger to dependency injection:
 *
 * OLD CODE (still works):
 *   dinero::g_logger.info("message");
 *
 * NEW CODE (using dependency injection):
 *   ILogger& logger = ProductionLogger::instance();
 *   logger.info("message");
 *
 * Usage in services:
 *   class MyService {
 *   public:
 *       MyService(ILogger& logger) : logger_(logger) {}
 *   private:
 *       ILogger& logger_;
 *   };
 *
 * Thread Safety:
 * - Delegates to g_logger, inheriting its thread safety properties
 * - Safe to use from multiple threads
 */
class ProductionLogger : public ILogger {
public:
    // Singleton access for convenient usage
    static ProductionLogger& instance() {
        static ProductionLogger instance;
        return instance;
    }

    // ILogger interface implementation (delegates to g_logger)
    void setLogLevel(LogLevel level) override {
        g_logger.setLogLevel(level);
    }

    void setLogFile(const std::string& filename) override {
        g_logger.setLogFile(filename);
    }

    void shutdown() override {
        g_logger.shutdown();
    }

    void log(LogLevel level, const std::string& message) override {
        g_logger.log(level, message);
    }

    void debug(const std::string& message) override {
        g_logger.debug(message);
    }

    void info(const std::string& message) override {
        g_logger.info(message);
    }

    void warning(const std::string& message) override {
        g_logger.warning(message);
    }

    void error(const std::string& message) override {
        g_logger.error(message);
    }

private:
    ProductionLogger() = default;
    ~ProductionLogger() override = default;

    // Prevent copying and moving
    ProductionLogger(const ProductionLogger&) = delete;
    ProductionLogger& operator=(const ProductionLogger&) = delete;
    ProductionLogger(ProductionLogger&&) = delete;
    ProductionLogger& operator=(ProductionLogger&&) = delete;
};

} // namespace dinero
