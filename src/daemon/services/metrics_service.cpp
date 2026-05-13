#include "daemon/services/metrics_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/daemon_context.h"
#include <stdexcept>

namespace dinero {

bool MetricsService::Init(DaemonContext& ctx) {
    // Wire dependencies from context
    logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    config_ = std::dynamic_pointer_cast<ConfigService>(ctx.config);

    if (!logger_) {
        throw std::runtime_error("[MetricsService] Logger dependency missing");
    }
    if (!config_) {
        logger_->error("[MetricsService] Config dependency missing");
        return false;
    }

    logger_->info("[MetricsService] Metrics service initialized");
    return true;
}

bool MetricsService::Start() {
    if (!logger_) {
        return false;
    }

    logger_->info("[MetricsService] Starting metrics service...");

    try {
        // Initialize the global MetricsRegistry singleton
        metrics::MetricsRegistry::Initialize();

        logger_->info("[MetricsService] MetricsRegistry initialized successfully");
        logger_->info("[MetricsService] Metrics available at /metrics endpoint");

        started_ = true;
        return true;

    } catch (const std::exception& e) {
        logger_->error("[MetricsService] Failed to initialize MetricsRegistry: " +
                       std::string(e.what()));
        return false;
    }
}

void MetricsService::Stop() {
    if (!started_) {
        logger_->info("[MetricsService] Already stopped");
        return;
    }

    logger_->info("[MetricsService] Stopping metrics service...");

    try {
        // Export final metrics snapshot for debugging
        std::string final_metrics = metrics::MetricsRegistry::ExportMetrics();

        logger_->info("[MetricsService] Final metrics snapshot:");
        logger_->info("[MetricsService] ----------------------------------------");

        // Log first 1000 characters of metrics (to avoid log spam)
        if (final_metrics.length() > 1000) {
            logger_->info(final_metrics.substr(0, 1000) + "... (truncated)");
        } else {
            logger_->info(final_metrics);
        }

        logger_->info("[MetricsService] ----------------------------------------");
        logger_->info("[MetricsService] Metrics service stopped cleanly");

        started_ = false;

    } catch (const std::exception& e) {
        logger_->error("[MetricsService] Error during shutdown: " + std::string(e.what()));
        started_ = false;
    }
}

} // namespace dinero
