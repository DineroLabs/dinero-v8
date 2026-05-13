#pragma once
#include <memory>

// Forward declarations for metrics types
class MetricCounter;
class MetricGauge;
class MetricHistogram;

/**
 * @brief Mining metrics handles for Prometheus-style monitoring
 * 
 * This structure holds pointers to metric objects that MiningEngine
 * can use to update metrics without knowing about the MetricsRegistry.
 * This keeps MiningEngine decoupled from the metrics system.
 */
struct MiningMetrics {
    // Counters (monotonic, never decrease)
    MetricCounter* blocks_found = nullptr;
    MetricCounter* shares_submitted = nullptr;
    MetricCounter* shares_accepted = nullptr;
    MetricCounter* shares_rejected = nullptr;

    // Gauges (can go up and down)
    MetricGauge* threads = nullptr;
    MetricGauge* hashrate_hps = nullptr;
    MetricGauge* job_height = nullptr;
    MetricGauge* current_bits = nullptr;
    MetricGauge* uptime_seconds = nullptr;

    // Histograms (for latency distribution)
    MetricHistogram* solution_latency_secs = nullptr;
};
