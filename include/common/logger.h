#pragma once

#include <string>
#include <fstream>
#include <atomic> // Added for atomic flag

// Windows wingdi.h defines ERROR as a macro — undefine it
#ifdef ERROR
#undef ERROR
#endif

namespace dinero {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3
};

class Logger {
public:
    Logger();
    ~Logger();
    
    void setLogLevel(LogLevel level);
    void setLogFile(const std::string& filename);
    void shutdown();  // Safe shutdown method
    
    void log(LogLevel level, const std::string& message);
    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void warn(const std::string& message) { warning(message); }  // Alias for warning
    void error(const std::string& message);
    
private:
    LogLevel log_level_;
    std::ofstream log_file_;
    std::atomic<bool> shutdown_flag_{false};  // Safe shutdown flag
    
    std::string getCurrentTimestamp();
    std::string levelToString(LogLevel level);
};

// Global logger instance
extern Logger g_logger;

} // namespace dinero 