#pragma once
#include "daemon/iservice.h"
#include "common/logger.h"
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace dinero {

/**
 * LoggerService - Wraps Logger as an IService
 *
 * This is the first service migrated to the new architecture.
 * It has no dependencies (Init() does nothing).
 *
 * Example:
 *   auto logger = std::make_shared<LoggerService>("dinero.log");
 *   logger->Init(ctx);
 *   logger->Start();  // Opens log file
 *   logger->info("Hello world");
 *   logger->Stop();   // Flushes and closes
 */
class LoggerService : public IService {
public:
    explicit LoggerService(const std::string& log_path = "dinero.log")
        : log_path_(log_path)
        , logger_(std::make_unique<Logger>()) {}

    std::string Name() const override { return "Logger"; }

    bool Init(DaemonContext& ctx) override {
        // Logger has no dependencies
        return true;
    }

    bool Start() override {
        // Phase D.1 (Dinero Core 1.0): file logging is OPT-IN. An empty
        // log_path_ means "no file destination — log to stderr / journal
        // only" per spec §1.2 row 6 (manual mode default) and §1.1 row 6
        // (packaged mode default; packaged mode flows through the systemd
        // journal regardless). To enable file logging, set
        // `debug.log_file = <path>` in dinero.conf or pass
        // `--debug.log_file=<path>` on the CLI.
        if (!log_path_.empty()) {
            logger_->setLogFile(log_path_);
            logger_->info("[LoggerService] Log file opened: " + log_path_);
        } else {
            logger_->info("[LoggerService] No file destination configured "
                          "(debug.log_file unset); logging to stderr/journal");
        }
        return true;
    }

    // Phase D.1: deferred file-log enablement. Called from daemon startup
    // AFTER the config-file loader has populated `debug.log_file` but
    // BEFORE Start() runs, so log_path_ reflects the operator's choice.
    void SetLogPath(const std::string& path) { log_path_ = path; }

    void Stop() override {
        logger_->info("[LoggerService] Shutting down logger");
        logger_->shutdown();
    }

    // Convenience accessors
    Logger& get() { return *logger_; }
    const Logger& get() const { return *logger_; }

    // Forward common methods
    void debug(const std::string& msg) { logger_->debug(msg); }
    void info(const std::string& msg) { logger_->info(msg); }
    void warning(const std::string& msg) { logger_->warning(msg); }
    void error(const std::string& msg) {
        // Phase D.4 (Dinero Core 1.0) health check: count FATAL log lines for
        // the "no FATAL in last 5 min" health signal. We auto-detect FATAL
        // markers in error() messages so existing call sites that emit
        // "[FATAL] ..." or "FATAL: ..." get counted without a codebase-wide
        // refactor. New "node-about-to-die" sites can call fatal() directly.
        if (msg.find("[FATAL]") != std::string::npos ||
            msg.find("FATAL:") != std::string::npos) {
            recordFatalNow();
        }
        logger_->error(msg);
    }

    // Explicit fatal: log via error() AND record for health counter. Use
    // this when a code path is genuinely "the node is about to die" — distinct
    // from recoverable error states. Currently no call sites use this; the
    // auto-detection in error() catches all existing FATAL log lines.
    void fatal(const std::string& msg) {
        recordFatalNow();
        logger_->error("[FATAL] " + msg);
    }

    // Returns the count of FATAL log entries recorded in the last 5 minutes.
    // Purges expired entries on each call. Thread-safe.
    int FatalCountLast5Min() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(fatal_mu_);
        // Purge entries older than 5 minutes from the front (deque is ordered
        // by insertion time, which is monotonic via steady_clock).
        while (!fatal_timestamps_.empty() &&
               (now - fatal_timestamps_.front()) > std::chrono::minutes(5)) {
            fatal_timestamps_.pop_front();
        }
        return static_cast<int>(fatal_timestamps_.size());
    }

private:
    void recordFatalNow() {
        std::lock_guard<std::mutex> lock(fatal_mu_);
        fatal_timestamps_.push_back(std::chrono::steady_clock::now());
        // Bound memory: a node spamming FATALs shouldn't blow heap. 1024
        // entries per 5-min window is far more than realistic FATAL rates;
        // the purge in FatalCountLast5Min() is the primary cleanup path.
        if (fatal_timestamps_.size() > 1024) {
            fatal_timestamps_.pop_front();
        }
    }

    std::string log_path_;
    std::unique_ptr<Logger> logger_;

    // FATAL ring for health-check signal. Mutex-guarded. Steady clock so
    // wall-clock skew (NTP corrections etc.) doesn't desync the window.
    std::mutex fatal_mu_;
    std::deque<std::chrono::steady_clock::time_point> fatal_timestamps_;
};

} // namespace dinero
