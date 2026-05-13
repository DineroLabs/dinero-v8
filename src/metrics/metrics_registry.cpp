#include "metrics/metrics_registry.h"
#include <sstream>
#include <atomic>
#include <map>
#include <mutex>
#include <algorithm>

namespace dinero {
namespace metrics {

// Simple in-memory counters (Prometheus-style without the dependency)
static std::atomic<uint64_t> g_submitAttempts{0};
static std::atomic<uint64_t> g_blocksAccepted{0};
static std::atomic<uint64_t> g_chainHeight{0};
static std::map<std::string, std::atomic<uint64_t>> g_blocksRejected;
static std::string g_lastRejectionReason;
static std::mutex g_rejectedMutex;

// Source-labeled submit attempts (local, external, peer)
static std::map<std::string, uint64_t> g_submitAttemptsBySource;
static std::mutex g_submitSourceMutex;

// Mining-specific metrics (with label support)
// Use regular types with mutex protection (atomics don't work in std::map)
static std::map<std::string, uint64_t> g_miningBlocksFound;
static std::map<std::string, uint64_t> g_miningSharesSubmitted;
static std::map<std::string, uint64_t> g_miningSharesAccepted;
static std::map<std::string, uint64_t> g_miningSharesRejected;
static std::map<std::string, int> g_miningThreads;
static std::map<std::string, double> g_miningHashrateHps;
static std::map<std::string, uint64_t> g_miningJobHeight;
static std::map<std::string, uint32_t> g_miningCurrentBits;
static std::map<std::string, double> g_miningUptimeSeconds;
static std::map<std::string, uint64_t> g_miningSolutionLatencyCount;
static std::map<std::string, double> g_miningSolutionLatencySum;
static std::mutex g_miningMetricsMutex;

// WebSocket metrics
static std::atomic<int> g_websocketClients{0};
static std::map<std::string, std::atomic<uint64_t>> g_websocketMessages;
static std::atomic<uint64_t> g_websocketDropped{0};
static std::atomic<uint64_t> g_websocketLatencyCount{0};
static std::atomic<double> g_websocketLatencySum{0.0};
static std::mutex g_websocketLatencyMutex;
static std::mutex g_websocketMessagesMutex;

bool MetricsRegistry::initialized_ = false;

void MetricsRegistry::Initialize() {
    if (!initialized_) {
        // Reset all counters
        g_submitAttempts.store(0);
        g_blocksAccepted.store(0);
        g_chainHeight.store(0);
        
        std::lock_guard<std::mutex> lock(g_rejectedMutex);
        g_blocksRejected.clear();
        
        std::lock_guard<std::mutex> miningLock(g_miningMetricsMutex);
        g_miningBlocksFound.clear();
        g_miningSharesSubmitted.clear();
        g_miningSharesAccepted.clear();
        g_miningSharesRejected.clear();
        g_miningThreads.clear();
        g_miningHashrateHps.clear();
        g_miningJobHeight.clear();
        g_miningCurrentBits.clear();
        g_miningUptimeSeconds.clear();
        g_miningSolutionLatencyCount.clear();
        g_miningSolutionLatencySum.clear();
        
        initialized_ = true;
    }
}

// Helper to format labels for Prometheus export
std::string MetricsRegistry::FormatLabels(const LabelMap& labels) {
    if (labels.empty()) {
        return "";
    }
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& [key, value] : labels) {
        if (!first) oss << ",";
        oss << key << "=\"" << value << "\"";
        first = false;
    }
    oss << "}";
    return oss.str();
}

// Helper to format labels for JSON export
std::string MetricsRegistry::FormatLabelsJSON(const LabelMap& labels) {
    if (labels.empty()) {
        return "{}";
    }
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& [key, value] : labels) {
        if (!first) oss << ",";
        oss << "\"" << key << "\":\"" << value << "\"";
        first = false;
    }
    oss << "}";
    return oss.str();
}

// Helper to get label key string for map lookup
static std::string GetLabelKey(const LabelMap& labels) {
    if (labels.empty()) {
        return "";  // Empty string for unlabeled metrics
    }
    std::ostringstream oss;
    bool first = true;
    for (const auto& [key, value] : labels) {
        if (!first) oss << ",";
        oss << key << "=" << value;
        first = false;
    }
    return oss.str();
}

// Helper to parse label key back into JSON object format
static LabelMap ParseLabelKeyToMap(const std::string& label_key) {
    LabelMap labels;
    if (label_key.empty()) {
        return labels;  // No labels
    }
    size_t pos = 0;
    while (pos < label_key.length()) {
        size_t eq_pos = label_key.find('=', pos);
        if (eq_pos == std::string::npos) break;
        std::string key = label_key.substr(pos, eq_pos - pos);
        pos = eq_pos + 1;
        size_t comma_pos = label_key.find(',', pos);
        std::string value = (comma_pos == std::string::npos) ? 
                           label_key.substr(pos) : 
                           label_key.substr(pos, comma_pos - pos);
        labels[key] = value;
        if (comma_pos == std::string::npos) break;
        pos = comma_pos + 1;
    }
    return labels;
}

// Helper to parse label key back into Prometheus format
static std::string ParseLabelKeyToPrometheus(const std::string& label_key) {
    if (label_key.empty()) {
        return "";  // No labels
    }
    LabelMap labels = ParseLabelKeyToMap(label_key);
    return MetricsRegistry::FormatLabels(labels);
}

void MetricsRegistry::IncrementSubmitAttempts() {
    g_submitAttempts.fetch_add(1);
}

void MetricsRegistry::IncrementBlocksAccepted() {
    g_blocksAccepted.fetch_add(1);
}

void MetricsRegistry::IncrementBlocksRejected(const std::string& reason) {
    std::lock_guard<std::mutex> lock(g_rejectedMutex);
    if (g_blocksRejected.find(reason) == g_blocksRejected.end()) {
        g_blocksRejected[reason].store(0);
    }
    g_blocksRejected[reason].fetch_add(1);
    g_lastRejectionReason = reason;  // Track last rejection for diagnostics
}

// Getter methods for daemon-verifiable counters
uint64_t MetricsRegistry::GetSubmitAttempts() {
    return g_submitAttempts.load();
}

uint64_t MetricsRegistry::GetBlocksAccepted() {
    return g_blocksAccepted.load();
}

uint64_t MetricsRegistry::GetBlocksRejectedTotal() {
    std::lock_guard<std::mutex> lock(g_rejectedMutex);
    uint64_t total = 0;
    for (const auto& [reason, count] : g_blocksRejected) {
        total += count.load();
    }
    return total;
}

std::map<std::string, uint64_t> MetricsRegistry::GetBlocksRejectedByReason() {
    std::lock_guard<std::mutex> lock(g_rejectedMutex);
    std::map<std::string, uint64_t> result;
    for (const auto& [reason, count] : g_blocksRejected) {
        result[reason] = count.load();
    }
    return result;
}

std::string MetricsRegistry::GetLastRejectionReason() {
    std::lock_guard<std::mutex> lock(g_rejectedMutex);
    return g_lastRejectionReason;
}

// Source-labeled submit tracking
void MetricsRegistry::IncrementSubmitAttempts(const std::string& source) {
    g_submitAttempts.fetch_add(1);  // Also increment total
    std::lock_guard<std::mutex> lock(g_submitSourceMutex);
    g_submitAttemptsBySource[source]++;
}

uint64_t MetricsRegistry::GetSubmitAttemptsBySource(const std::string& source) {
    std::lock_guard<std::mutex> lock(g_submitSourceMutex);
    auto it = g_submitAttemptsBySource.find(source);
    return (it != g_submitAttemptsBySource.end()) ? it->second : 0;
}

std::map<std::string, uint64_t> MetricsRegistry::GetSubmitAttemptsByAllSources() {
    std::lock_guard<std::mutex> lock(g_submitSourceMutex);
    return g_submitAttemptsBySource;
}

void MetricsRegistry::SetChainHeight(uint64_t height) {
    g_chainHeight.store(height);
}

// Mining metrics implementations (with label support)
void MetricsRegistry::IncrementMiningBlocksFound(const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningBlocksFound[key]++;  // operator[] default-constructs to 0 if not found
}

void MetricsRegistry::IncrementMiningSharesSubmitted(const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningSharesSubmitted[key]++;
}

void MetricsRegistry::IncrementMiningSharesAccepted(const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningSharesAccepted[key]++;
}

void MetricsRegistry::IncrementMiningSharesRejected(const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningSharesRejected[key]++;
}

void MetricsRegistry::SetMiningThreads(int threads, const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningThreads[key] = threads;
}

void MetricsRegistry::SetMiningHashrate(double hashrate_hps, const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningHashrateHps[key] = hashrate_hps;
}

void MetricsRegistry::SetMiningJobHeight(uint64_t height, const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningJobHeight[key] = height;
}

void MetricsRegistry::SetMiningCurrentBits(uint32_t bits, const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningCurrentBits[key] = bits;
}

void MetricsRegistry::SetMiningUptime(double uptime_seconds, const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningUptimeSeconds[key] = uptime_seconds;
}

void MetricsRegistry::ObserveMiningSolutionLatency(double latency_seconds, const LabelMap& labels) {
    std::string key = GetLabelKey(labels);
    std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
    g_miningSolutionLatencyCount[key]++;
    g_miningSolutionLatencySum[key] += latency_seconds;
}

void MetricsRegistry::SetWebSocketClients(int clients) {
    g_websocketClients.store(clients);
}

void MetricsRegistry::IncrementWebSocketMessages(const std::string& topic) {
    std::lock_guard<std::mutex> lock(g_websocketMessagesMutex);
    g_websocketMessages[topic].fetch_add(1);
}

void MetricsRegistry::IncrementWebSocketDropped() {
    g_websocketDropped.fetch_add(1);
}

void MetricsRegistry::ObserveWebSocketLatency(double latency_ms) {
    g_websocketLatencyCount.fetch_add(1);
    double current_sum = g_websocketLatencySum.load();
    double new_sum = current_sum + latency_ms;
    g_websocketLatencySum.store(new_sum);
}

std::string MetricsRegistry::ExportMetrics() {
    std::ostringstream metrics;
    
    // Submit attempts
    metrics << "# HELP din_mining_submit_attempts_total Total block submission attempts\n";
    metrics << "# TYPE din_mining_submit_attempts_total counter\n";
    metrics << "din_mining_submit_attempts_total " << g_submitAttempts.load() << "\n";
    
    // Blocks accepted
    metrics << "# HELP din_blocks_accepted_total Number of blocks accepted by the network\n";
    metrics << "# TYPE din_blocks_accepted_total counter\n";
    metrics << "din_blocks_accepted_total " << g_blocksAccepted.load() << "\n";
    
    // Blocks rejected (with reasons)
    metrics << "# HELP din_blocks_rejected_total Number of blocks rejected by the network\n";
    metrics << "# TYPE din_blocks_rejected_total counter\n";
    {
        std::lock_guard<std::mutex> lock(g_rejectedMutex);
        if (g_blocksRejected.empty()) {
            metrics << "din_blocks_rejected_total{reason=\"none\"} 0\n";
        } else {
            for (const auto& [reason, count] : g_blocksRejected) {
                metrics << "din_blocks_rejected_total{reason=\"" << reason << "\"} " << count.load() << "\n";
            }
        }
    }
    
    // Blockchain height
    metrics << "# HELP din_blockchain_height Current blockchain height\n";
    metrics << "# TYPE din_blockchain_height gauge\n";
    metrics << "din_blockchain_height " << g_chainHeight.load() << "\n";
    
    // Mining metrics (with label support)
    {
        std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
        
        // Blocks found
        metrics << "# HELP din_mining_blocks_found_total Blocks found by this node\n";
        metrics << "# TYPE din_mining_blocks_found_total counter\n";
        if (g_miningBlocksFound.empty()) {
            metrics << "din_mining_blocks_found_total 0\n";
        } else {
            for (const auto& [label_key, count] : g_miningBlocksFound) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_blocks_found_total" << label_str << " " << count << "\n";
            }
        }
        
        // Shares submitted
        metrics << "# HELP din_mining_shares_submitted_total Shares submitted\n";
        metrics << "# TYPE din_mining_shares_submitted_total counter\n";
        if (g_miningSharesSubmitted.empty()) {
            metrics << "din_mining_shares_submitted_total 0\n";
        } else {
            for (const auto& [label_key, count] : g_miningSharesSubmitted) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_shares_submitted_total" << label_str << " " << count << "\n";
            }
        }
        
        // Shares accepted
        metrics << "# HELP din_mining_shares_accepted_total Shares accepted\n";
        metrics << "# TYPE din_mining_shares_accepted_total counter\n";
        if (g_miningSharesAccepted.empty()) {
            metrics << "din_mining_shares_accepted_total 0\n";
        } else {
            for (const auto& [label_key, count] : g_miningSharesAccepted) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_shares_accepted_total" << label_str << " " << count << "\n";
            }
        }
        
        // Shares rejected
        metrics << "# HELP din_mining_shares_rejected_total Shares rejected\n";
        metrics << "# TYPE din_mining_shares_rejected_total counter\n";
        if (g_miningSharesRejected.empty()) {
            metrics << "din_mining_shares_rejected_total 0\n";
        } else {
            for (const auto& [label_key, count] : g_miningSharesRejected) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_shares_rejected_total" << label_str << " " << count << "\n";
            }
        }
        
        // Threads
        metrics << "# HELP din_mining_threads Active mining threads\n";
        metrics << "# TYPE din_mining_threads gauge\n";
        if (g_miningThreads.empty()) {
            metrics << "din_mining_threads 0\n";
        } else {
            for (const auto& [label_key, threads] : g_miningThreads) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_threads" << label_str << " " << threads << "\n";
            }
        }
        
        // Hashrate
        metrics << "# HELP din_mining_hashrate_hps Instant hashrate\n";
        metrics << "# TYPE din_mining_hashrate_hps gauge\n";
        if (g_miningHashrateHps.empty()) {
            metrics << "din_mining_hashrate_hps 0\n";
        } else {
            for (const auto& [label_key, hashrate] : g_miningHashrateHps) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_hashrate_hps" << label_str << " " << hashrate << "\n";
            }
        }
        
        // Job height
        metrics << "# HELP din_mining_job_height Current job height\n";
        metrics << "# TYPE din_mining_job_height gauge\n";
        if (g_miningJobHeight.empty()) {
            metrics << "din_mining_job_height 0\n";
        } else {
            for (const auto& [label_key, height] : g_miningJobHeight) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_job_height" << label_str << " " << height << "\n";
            }
        }
        
        // Current bits
        metrics << "# HELP din_mining_current_bits Current job difficulty (compact bits)\n";
        metrics << "# TYPE din_mining_current_bits gauge\n";
        if (g_miningCurrentBits.empty()) {
            metrics << "din_mining_current_bits 0\n";
        } else {
            for (const auto& [label_key, bits] : g_miningCurrentBits) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_current_bits" << label_str << " " << bits << "\n";
            }
        }
        
        // Uptime
        metrics << "# HELP din_mining_uptime_seconds Miner uptime\n";
        metrics << "# TYPE din_mining_uptime_seconds gauge\n";
        if (g_miningUptimeSeconds.empty()) {
            metrics << "din_mining_uptime_seconds 0\n";
        } else {
            for (const auto& [label_key, uptime] : g_miningUptimeSeconds) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                metrics << "din_mining_uptime_seconds" << label_str << " " << uptime << "\n";
            }
        }
        
        // Solution latency
        metrics << "# HELP din_mining_solution_latency_seconds_average Average solution latency\n";
        metrics << "# TYPE din_mining_solution_latency_seconds_average gauge\n";
        metrics << "# HELP din_mining_solution_latency_seconds_count Total solution count\n";
        metrics << "# TYPE din_mining_solution_latency_seconds_count counter\n";
        if (g_miningSolutionLatencyCount.empty()) {
            metrics << "din_mining_solution_latency_seconds_average 0\n";
            metrics << "din_mining_solution_latency_seconds_count 0\n";
        } else {
            for (const auto& [label_key, count] : g_miningSolutionLatencyCount) {
                std::string label_str = ParseLabelKeyToPrometheus(label_key);
                uint64_t latency_count = count;
                double latency_sum = g_miningSolutionLatencySum[label_key];
                double avg_latency = (latency_count > 0) ? latency_sum / latency_count : 0.0;
                metrics << "din_mining_solution_latency_seconds_average" << label_str << " " << avg_latency << "\n";
                metrics << "din_mining_solution_latency_seconds_count" << label_str << " " << latency_count << "\n";
            }
        }
    }

    // WebSocket metrics
    metrics << "# HELP din_ws_clients Current number of WebSocket clients\n";
    metrics << "# TYPE din_ws_clients gauge\n";
    metrics << "din_ws_clients " << g_websocketClients << "\n";

    std::lock_guard<std::mutex> wsMsgLock(g_websocketMessagesMutex);
    for (const auto& [topic, count] : g_websocketMessages) {
        metrics << "# HELP din_ws_messages_total Total WebSocket messages by topic\n";
        metrics << "# TYPE din_ws_messages_total counter\n";
        metrics << "din_ws_messages_total{topic=\"" << topic << "\"} " << count << "\n";
    }

    metrics << "# HELP din_ws_dropped_total Total dropped WebSocket messages\n";
    metrics << "# TYPE din_ws_dropped_total counter\n";
    metrics << "din_ws_dropped_total " << g_websocketDropped << "\n";

    if (g_websocketLatencyCount > 0) {
        metrics << "# HELP din_ws_broadcast_latency_ms WebSocket broadcast latency\n";
        metrics << "# TYPE din_ws_broadcast_latency_ms histogram\n";
        metrics << "din_ws_broadcast_latency_ms_count " << g_websocketLatencyCount << "\n";
        metrics << "din_ws_broadcast_latency_ms_sum " << g_websocketLatencySum << "\n";
    }

    // Build info metric (constant gauge with version info)
    metrics << "# HELP din_build_info Daemon build information\n";
    metrics << "# TYPE din_build_info gauge\n";
    metrics << "din_build_info{version=\"0.1.0\",git=\"unknown\",target=\"daemon\"} 1\n";

    return metrics.str();
}

// Week 5: Export metrics in JSON format
std::string MetricsRegistry::ExportMetricsJSON() {
    std::ostringstream json;
    json << "{\n";
    
    // Blockchain metrics
    json << "  \"blockchain\": {\n";
    json << "    \"height\": " << g_chainHeight.load() << ",\n";
    json << "    \"blocks_accepted\": " << g_blocksAccepted.load() << ",\n";
    json << "    \"submit_attempts\": " << g_submitAttempts.load() << ",\n";
    json << "    \"blocks_rejected\": {\n";
    {
        std::lock_guard<std::mutex> lock(g_rejectedMutex);
        bool first = true;
        for (const auto& [reason, count] : g_blocksRejected) {
            if (!first) json << ",\n";
            json << "      \"" << reason << "\": " << count.load();
            first = false;
        }
        if (g_blocksRejected.empty()) {
            json << "      \"none\": 0";
        }
    }
    json << "\n    }\n";
    json << "  },\n";
    
    // Mining metrics (with label support)
    json << "  \"mining\": {\n";
    {
        std::lock_guard<std::mutex> lock(g_miningMetricsMutex);
        
        // Blocks found (grouped by labels)
        json << "    \"blocks_found\": {\n";
        bool first_bf = true;
        for (const auto& [label_key, count] : g_miningBlocksFound) {
            if (!first_bf) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << count;
            first_bf = false;
        }
        if (g_miningBlocksFound.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Shares submitted
        json << "    \"shares_submitted\": {\n";
        bool first_ss = true;
        for (const auto& [label_key, count] : g_miningSharesSubmitted) {
            if (!first_ss) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << count;
            first_ss = false;
        }
        if (g_miningSharesSubmitted.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Shares accepted
        json << "    \"shares_accepted\": {\n";
        bool first_sa = true;
        for (const auto& [label_key, count] : g_miningSharesAccepted) {
            if (!first_sa) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << count;
            first_sa = false;
        }
        if (g_miningSharesAccepted.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Shares rejected
        json << "    \"shares_rejected\": {\n";
        bool first_sr = true;
        for (const auto& [label_key, count] : g_miningSharesRejected) {
            if (!first_sr) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << count;
            first_sr = false;
        }
        if (g_miningSharesRejected.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Threads
        json << "    \"threads\": {\n";
        bool first_th = true;
        for (const auto& [label_key, threads] : g_miningThreads) {
            if (!first_th) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << threads;
            first_th = false;
        }
        if (g_miningThreads.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Hashrate
        json << "    \"hashrate_hps\": {\n";
        bool first_hr = true;
        for (const auto& [label_key, hashrate] : g_miningHashrateHps) {
            if (!first_hr) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << hashrate;
            first_hr = false;
        }
        if (g_miningHashrateHps.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Job height
        json << "    \"job_height\": {\n";
        bool first_jh = true;
        for (const auto& [label_key, height] : g_miningJobHeight) {
            if (!first_jh) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << height;
            first_jh = false;
        }
        if (g_miningJobHeight.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Current bits
        json << "    \"current_bits\": {\n";
        bool first_cb = true;
        for (const auto& [label_key, bits] : g_miningCurrentBits) {
            if (!first_cb) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << bits;
            first_cb = false;
        }
        if (g_miningCurrentBits.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Uptime
        json << "    \"uptime_seconds\": {\n";
        bool first_ut = true;
        for (const auto& [label_key, uptime] : g_miningUptimeSeconds) {
            if (!first_ut) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": " << uptime;
            first_ut = false;
        }
        if (g_miningUptimeSeconds.empty()) {
            json << "      {}: 0";
        }
        json << "\n    },\n";
        
        // Solution latency
        json << "    \"solution_latency\": {\n";
        bool first_sl = true;
        for (const auto& [label_key, count] : g_miningSolutionLatencyCount) {
            if (!first_sl) json << ",\n";
            LabelMap labels = ParseLabelKeyToMap(label_key);
            uint64_t latency_count = count;
            double latency_sum = g_miningSolutionLatencySum[label_key];
            double avg_latency = (latency_count > 0) ? latency_sum / latency_count : 0.0;
            json << "      " << MetricsRegistry::FormatLabelsJSON(labels) << ": {\n";
            json << "        \"count\": " << latency_count << ",\n";
            json << "        \"sum_seconds\": " << latency_sum << ",\n";
            json << "        \"average_seconds\": " << avg_latency << "\n";
            json << "      }";
            first_sl = false;
        }
        if (g_miningSolutionLatencyCount.empty()) {
            json << "      {}: {\n";
            json << "        \"count\": 0,\n";
            json << "        \"sum_seconds\": 0,\n";
            json << "        \"average_seconds\": 0\n";
            json << "      }";
        }
        json << "\n    }\n";
    }
    json << "  },\n";
    
    // WebSocket metrics
    json << "  \"websocket\": {\n";
    json << "    \"clients\": " << g_websocketClients.load() << ",\n";
    json << "    \"dropped\": " << g_websocketDropped.load() << ",\n";
    json << "    \"messages_by_topic\": {\n";
    {
        std::lock_guard<std::mutex> lock(g_websocketMessagesMutex);
        bool first = true;
        for (const auto& [topic, count] : g_websocketMessages) {
            if (!first) json << ",\n";
            json << "      \"" << topic << "\": " << count.load();
            first = false;
        }
        if (g_websocketMessages.empty()) {
            json << "      \"none\": 0";
        }
    }
    json << "\n    },\n";
    uint64_t ws_latency_count = g_websocketLatencyCount.load();
    double ws_latency_sum = g_websocketLatencySum.load();
    json << "    \"broadcast_latency\": {\n";
    json << "      \"count\": " << ws_latency_count << ",\n";
    json << "      \"sum_ms\": " << ws_latency_sum << ",\n";
    json << "      \"average_ms\": " << (ws_latency_count > 0 ? ws_latency_sum / ws_latency_count : 0.0) << "\n";
    json << "    }\n";
    json << "  },\n";
    
    // Build info
    json << "  \"build\": {\n";
    json << "    \"version\": \"0.1.0\",\n";
    json << "    \"git_hash\": \"unknown\",\n";
    json << "    \"target\": \"daemon\"\n";
    json << "  }\n";
    
    json << "}\n";
    return json.str();
}

} // namespace metrics
} // namespace dinero
