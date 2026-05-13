/**
 * Logging Control RPC Methods - Context-Aware
 *
 * Step C: Dynamic Runtime Control - Change log levels without restarting the daemon.
 *
 * Modern handlers using DaemonContext (no globals).
 *
 * Methods:
 *   - logging.setlevel: Dynamically change log levels (global or per-service)
 *   - logging.getlevel: Query current log levels for all services
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "common/ilogger.h"
#include "common/logger.h"
#include <stdexcept>
#include <string>

// Helper: Convert string to LogLevel
static dinero::LogLevel stringToLogLevel(const std::string& level) {
    if (level == "debug" || level == "DEBUG") return dinero::LogLevel::DEBUG;
    if (level == "info" || level == "INFO") return dinero::LogLevel::INFO;
    if (level == "warning" || level == "WARNING" || level == "warn") return dinero::LogLevel::WARNING;
    if (level == "error" || level == "ERROR") return dinero::LogLevel::ERROR;
    throw std::invalid_argument("Invalid log level: " + level + " (valid: debug, info, warning, error)");
}

// Helper: Convert LogLevel to string
static std::string logLevelToString(dinero::LogLevel level) {
    switch (level) {
        case dinero::LogLevel::DEBUG: return "debug";
        case dinero::LogLevel::INFO: return "info";
        case dinero::LogLevel::WARNING: return "warning";
        case dinero::LogLevel::ERROR: return "error";
        default: return "unknown";
    }
}

/**
 * logging.setlevel - Dynamically change log levels
 *
 * Parameters:
 *   {
 *     "level": "debug|info|warning|error",        // Required: Log level to set
 *     "service": "wallet|p2p|mining|mempool"      // Optional: Target specific service
 *   }
 *
 * Returns:
 *   {
 *     "success": true,
 *     "message": "Log level set to debug for wallet service"
 *   }
 *
 * Examples:
 *   // Set global log level
 *   {"method": "logging.setlevel", "params": {"level": "debug"}}
 *
 *   // Set per-service log level
 *   {"method": "logging.setlevel", "params": {"level": "debug", "service": "wallet"}}
 */
din::Json rpc_context_logging_setlevel(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Validate required parameter: level
    if (!params.isMember("level") || !params["level"].isString()) {
        throw std::invalid_argument("Missing or invalid 'level' parameter (must be string: debug|info|warning|error)");
    }

    std::string levelStr = params["level"].asString();
    dinero::LogLevel level = stringToLogLevel(levelStr);

    // Check if service-specific or global
    std::string service;
    if (params.isMember("service") && params["service"].isString()) {
        service = params["service"].asString();
    }

    // Access DaemonContext via ExecutionContext
    auto daemonCtx = DaemonContext::instance();
    if (!daemonCtx) {
        throw std::runtime_error("DaemonContext not available");
    }

    result["success"] = true;

    if (service.empty()) {
        // Set global log level
        if (daemonCtx->logger_interface) {
            daemonCtx->logger_interface->setLogLevel(level);
            result["message"] = "Global log level set to " + levelStr;
            dinero::g_logger.info("[RPC] Global log level set to " + levelStr);
        } else {
            throw std::runtime_error("Global logger not available");
        }
    } else {
        // Set service-specific log level
        dinero::ILogger* logger = nullptr;

        if (service == "wallet" && daemonCtx->wallet_logger) {
            logger = daemonCtx->wallet_logger;
        } else if (service == "p2p" && daemonCtx->p2p_logger) {
            logger = daemonCtx->p2p_logger;
        } else if (service == "mining" && daemonCtx->mining_logger) {
            logger = daemonCtx->mining_logger;
        } else if (service == "mempool" && daemonCtx->mempool_logger) {
            logger = daemonCtx->mempool_logger;
        } else {
            throw std::invalid_argument("Invalid or unavailable service: " + service +
                                       " (valid: wallet, p2p, mining, mempool)");
        }

        logger->setLogLevel(level);
        result["message"] = "Log level set to " + levelStr + " for " + service + " service";
        dinero::g_logger.info("[RPC] " + service + " log level set to " + levelStr);
    }

    return result;
}

/**
 * logging.getlevel - Get current log levels
 *
 * Parameters: None
 *
 * Returns:
 *   {
 *     "global": "info",
 *     "services": {
 *       "wallet": "debug",
 *       "p2p": "info",
 *       "mining": "info",
 *       "mempool": "warning"
 *     }
 *   }
 *
 * Example:
 *   {"method": "logging.getlevel"}
 *
 * Note: ILogger currently supports dynamic setLogLevel() but not read-back.
 *       This endpoint reports logger availability and capability limits.
 */
din::Json rpc_context_logging_getlevel(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Access DaemonContext via ExecutionContext
    auto daemonCtx = DaemonContext::instance();
    if (!daemonCtx) {
        throw std::runtime_error("DaemonContext not available");
    }

    // ILogger currently exposes write-only level control.
    result["global"] = "unavailable";
    result["capability"] = "write_only";
    result["note"] = "Current log-level readback is not exposed by ILogger";

    // Get service-specific log levels
    din::Json services;

    // Helper lambda to report logger state without pretending to know its level.
    auto getServiceLevel = [](dinero::ILogger* logger) -> std::string {
        if (!logger) return "not_available";
        return "configured";
    };

    services["wallet"] = getServiceLevel(daemonCtx->wallet_logger);
    services["p2p"] = getServiceLevel(daemonCtx->p2p_logger);
    services["mining"] = getServiceLevel(daemonCtx->mining_logger);
    services["mempool"] = getServiceLevel(daemonCtx->mempool_logger);

    result["services"] = services;

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION
// ═══════════════════════════════════════════════════════════════

// Global RPC registry (defined in rpc_registry.cpp)
extern RpcRegistry g_rpcRegistry;

void WireLoggingRpcContext() {
    // Register context-aware logging control methods
    g_rpcRegistry.registerHandler("logging.setlevel",
                                 rpc_context_logging_setlevel,
                                 RegisterMode::Overwrite,
                                 "Dynamically change log levels without restart");

    g_rpcRegistry.registerHandler("logging.getlevel",
                                 rpc_context_logging_getlevel,
                                 RegisterMode::Overwrite,
                                 "Get current log levels for all services");

    dinero::g_logger.info("[RPC] Registered context-aware logging control methods");
}
