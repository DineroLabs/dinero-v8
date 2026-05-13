#pragma once
#include "daemon/iservice.h"
#include "metrics/metrics_registry.h"
#include <memory>
#include <string>

namespace dinero {

/**
 * MetricsService - IService wrapper for MetricsRegistry
 *
 * Wraps the metrics system into IService lifecycle:
 * - Init() wires logger and config dependencies
 * - Start() initializes the metrics registry
 * - Stop() exports final metrics snapshot
 *
 * Dependencies: Logger, Config
 *
 * The MetricsRegistry provides:
 * - Prometheus-format metrics export
 * - Mining metrics (blocks found, shares, hashrate)
 * - Blockchain metrics (height, difficulty)
 * - WebSocket metrics (connections, messages)
 * - Counter and gauge updates from all subsystems
 */
class MetricsService : public IService {
public:
    MetricsService() = default;
    ~MetricsService() override = default;

    std::string Name() const override { return "Metrics"; }

    /**
     * Initialize metrics service with dependencies from context
     * Stores logger and config references
     */
    bool Init(DaemonContext& ctx) override;

    /**
     * Start metrics service
     * - Initializes the MetricsRegistry singleton
     * - Logs initial metrics status
     */
    bool Start() override;

    /**
     * Stop metrics service
     * - Exports final metrics snapshot to log
     * - Metrics remain available for final queries
     */
    void Stop() override;

    /**
     * Export metrics in Prometheus format
     * @return Metrics snapshot as string
     */
    std::string ExportMetrics() const {
        return metrics::MetricsRegistry::ExportMetrics();
    }

    /**
     * Check if metrics are initialized
     */
    bool IsStarted() const { return started_; }

private:
    std::shared_ptr<class LoggerService> logger_;
    std::shared_ptr<class ConfigService> config_;
    bool started_ = false;
};

} // namespace dinero
