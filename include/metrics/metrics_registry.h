#pragma once
#include <string>
#include <cstdint>
#include <map>
#include <vector>

namespace dinero {
namespace metrics {

// Label map for Prometheus-style labels (key-value pairs)
using LabelMap = std::map<std::string, std::string>;

// Single metrics registry accessor - ensures everyone shares the same instance
// Avoids duplicate statics across libs and provides clean counter wiring
class MetricsRegistry {
public:
    // Get the singleton metrics registry
    static void Initialize();
    
    // Increment counters for mining operations
    static void IncrementSubmitAttempts();
    static void IncrementBlocksAccepted();
    static void IncrementBlocksRejected(const std::string& reason);

    // Getter methods for daemon-verifiable counters
    static uint64_t GetSubmitAttempts();
    static uint64_t GetBlocksAccepted();
    static uint64_t GetBlocksRejectedTotal();

    // Rejection reasons histogram (for miner diagnostics)
    static std::map<std::string, uint64_t> GetBlocksRejectedByReason();
    static std::string GetLastRejectionReason();

    // Source-labeled submit tracking (local vs external vs peer)
    static void IncrementSubmitAttempts(const std::string& source);
    static uint64_t GetSubmitAttemptsBySource(const std::string& source);
    static std::map<std::string, uint64_t> GetSubmitAttemptsByAllSources();

    // Update blockchain height gauge
    static void SetChainHeight(uint64_t height);
    
    // Mining metrics (with optional labels for per-miner tracking)
    static void IncrementMiningBlocksFound(const LabelMap& labels = {});
    static void IncrementMiningSharesSubmitted(const LabelMap& labels = {});
    static void IncrementMiningSharesAccepted(const LabelMap& labels = {});
    static void IncrementMiningSharesRejected(const LabelMap& labels = {});
    static void SetMiningThreads(int threads, const LabelMap& labels = {});
    static void SetMiningHashrate(double hashrate_hps, const LabelMap& labels = {});
    static void SetMiningJobHeight(uint64_t height, const LabelMap& labels = {});
    static void SetMiningCurrentBits(uint32_t bits, const LabelMap& labels = {});
    static void SetMiningUptime(double uptime_seconds, const LabelMap& labels = {});
    static void ObserveMiningSolutionLatency(double latency_seconds, const LabelMap& labels = {});

    // WebSocket metrics
    static void SetWebSocketClients(int clients);
    static void IncrementWebSocketMessages(const std::string& topic);
    static void IncrementWebSocketDropped();
    static void ObserveWebSocketLatency(double latency_ms);

    // Export metrics in Prometheus format
    static std::string ExportMetrics();
    
    // Week 5: Export metrics in JSON format
    static std::string ExportMetricsJSON();
    
    // Helper to format labels for Prometheus export (public for static helpers)
    static std::string FormatLabels(const LabelMap& labels);
    
    // Helper to format labels for JSON export (public for static helpers)
    static std::string FormatLabelsJSON(const LabelMap& labels);
    
private:
    MetricsRegistry() = default;
    static bool initialized_;
};

} // namespace metrics
} // namespace dinero
