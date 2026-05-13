#pragma once

#include <string>

// Include LogLevel from existing logger (reuse the enum, don't duplicate)
#include "common/logger.h"

namespace dinero {

/**
 * ILogger - Logger Interface for Dependency Injection
 *
 * This interface allows different logging implementations to be injected
 * into services, improving testability and modularity.
 *
 * Implementations:
 * - ProductionLogger: Wraps the existing Logger class for production use
 * - NullLogger: Discards all log messages (for tests)
 * - TestLogger: Captures log messages for test assertions
 */
class ILogger {
public:
    virtual ~ILogger() = default;

    // Configuration
    virtual void setLogLevel(LogLevel level) = 0;
    virtual void setLogFile(const std::string& filename) = 0;
    virtual void shutdown() = 0;

    // Logging methods
    virtual void log(LogLevel level, const std::string& message) = 0;
    virtual void debug(const std::string& message) = 0;
    virtual void info(const std::string& message) = 0;
    virtual void warning(const std::string& message) = 0;
    virtual void error(const std::string& message) = 0;
};

} // namespace dinero
