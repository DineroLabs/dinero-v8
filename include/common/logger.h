#pragma once

#include <string>
#include <fstream>
#include <atomic> // Added for atomic flag
#include <cstddef>
#include <cstdint>
#include <mutex>

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

    // issue #224: bound the on-disk log so unattended nodes can't fill the disk.
    // max_bytes == 0 disables rotation (unbounded, legacy behavior). When the
    // active file reaches max_bytes it is rotated: debug.log -> debug.log.1 ->
    // ... up to max_files archives (older dropped), and a fresh debug.log opens.
    void setRotation(std::size_t max_bytes, std::uint32_t max_files);

    // Signal-safe: request the active log file be reopened on the next write
    // (SIGHUP / external logrotate compatibility). Only sets an atomic flag.
    void requestReopen();

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

    // issue #224: rotation + reopen state. file_mutex_ serializes the file
    // write, rotation, and reopen so concurrent log() calls can't race the
    // rename/reopen (console output is unaffected).
    std::mutex file_mutex_;
    std::string log_path_;
    std::size_t bytes_written_{0};
    std::size_t max_log_bytes_{0};        // 0 = rotation disabled
    std::uint32_t max_log_files_{5};
    std::atomic<bool> reopen_requested_{false};

    std::string getCurrentTimestamp();
    std::string levelToString(LogLevel level);
    void rotateLocked_();   // file_mutex_ held: close, shift archives, open fresh
    void reopenLocked_();   // file_mutex_ held: close + reopen log_path_ (append)
};

// Global logger instance
extern Logger g_logger;

// Signal-safe free function: request the active file logger reopen its file
// (SIGHUP handler / external logrotate). No-op if no file logger is active.
void requestLogReopen();

} // namespace dinero 