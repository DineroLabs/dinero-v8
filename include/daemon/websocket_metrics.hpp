#pragma once
#include <atomic>
#include <cstdint>
#include <string>

struct WebSocketMetrics {
    // Connection lifecycle
    std::atomic<uint64_t> ws_connections_current{0};
    std::atomic<uint64_t> ws_connections_accepted_total{0};
    std::atomic<uint64_t> ws_connections_closed_total{0};
    
    // Authentication and errors
    std::atomic<uint64_t> ws_auth_fail{0};
    std::atomic<uint64_t> ws_ping_timeouts{0};
    
    // Backpressure and drops
    std::atomic<uint64_t> ws_backpressure_drops{0};
    std::atomic<uint64_t> ws_conn_dropped_backpressure{0};
    std::atomic<uint64_t> ws_dropped_events_total{0};
    
    // Outbound flow control / accounting
    std::atomic<uint64_t> ws_sent_bytes_total{0};
    
    // Per-channel sent counters (new structure from patch)
    std::atomic<uint64_t> ws_out_total_newBlocks{0};
    std::atomic<uint64_t> ws_out_total_miningInfo{0};
    std::atomic<uint64_t> ws_out_total_mempoolTx{0};
    
    // Legacy counters (keep for backward compatibility)
    std::atomic<uint64_t> out_newBlocks{0};
    std::atomic<uint64_t> out_miningInfo{0};
    std::atomic<uint64_t> out_mempoolTx{0};
    
    // Performance metrics
    std::atomic<uint64_t> ws_events_sent_total{0};
    
    // Per-channel drop tracking
    std::atomic<uint64_t> dropped_miningInfo{0};
    std::atomic<uint64_t> dropped_newBlocks{0};
    std::atomic<uint64_t> dropped_mempoolTx{0};
    
    std::string to_json() const {
        return std::string("{") +
            "\"ws_connections_current\":" + std::to_string(ws_connections_current.load()) + "," +
            "\"ws_connections_accepted_total\":" + std::to_string(ws_connections_accepted_total.load()) + "," +
            "\"ws_connections_closed_total\":" + std::to_string(ws_connections_closed_total.load()) + "," +
            "\"ws_auth_fail\":" + std::to_string(ws_auth_fail.load()) + "," +
            "\"ws_ping_timeouts\":" + std::to_string(ws_ping_timeouts.load()) + "," +
            "\"ws_backpressure_drops\":" + std::to_string(ws_backpressure_drops.load()) + "," +
            "\"ws_conn_dropped_backpressure\":" + std::to_string(ws_conn_dropped_backpressure.load()) + "," +
            "\"ws_dropped_events_total\":" + std::to_string(ws_dropped_events_total.load()) + "," +
            "\"ws_sent_bytes_total\":" + std::to_string(ws_sent_bytes_total.load()) + "," +
            "\"ws_events_sent_total\":" + std::to_string(ws_events_sent_total.load()) + "," +
            "\"ws_out_total\":{" +
                "\"newBlocks\":" + std::to_string(ws_out_total_newBlocks.load()) + "," +
                "\"miningInfo\":" + std::to_string(ws_out_total_miningInfo.load()) + "," +
                "\"mempoolTx\":" + std::to_string(ws_out_total_mempoolTx.load()) +
            "}," +
            "\"ws_dropped_by_channel\":{" +
                "\"miningInfo\":" + std::to_string(dropped_miningInfo.load()) + "," +
                "\"newBlocks\":" + std::to_string(dropped_newBlocks.load()) + "," +
                "\"mempoolTx\":" + std::to_string(dropped_mempoolTx.load()) +
            "}" +
            "}";
    }
};

extern WebSocketMetrics g_websocket_metrics;
