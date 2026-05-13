#include "rpc/ws_bridge.hpp"
#include "daemon/ws_subscriptions.hpp"
#include "common/logger.h"
#include "daemon/websocket_metrics.hpp"
#include <json/json.h>

extern WebSocketMetrics g_websocket_metrics;

namespace dinero {

WsBridge::WsBridge(Subscriptions* subs) 
    : subs_(subs) {
}

WsBridge::~WsBridge() {
    stop();
}

void WsBridge::start() {
    if (running_.load()) return;
    
    running_.store(true, std::memory_order_relaxed);
    th_ = std::thread([this]{ run(); });
    g_logger.info("🔔 WebSocket bridge started");
}

void WsBridge::stop() {
    if (!running_.load()) return;
    
    running_.store(false, std::memory_order_relaxed);
    dinero::BroadcastBus::instance().stop();  // wake waiters
    if (th_.joinable()) {
        th_.join();
    }
    g_logger.info("🔔 WebSocket bridge stopped");
}

void WsBridge::run() {
    auto& bus = dinero::BroadcastBus::instance();
    std::vector<dinero::BusEvent> batch;
    batch.reserve(256);
    
    // Deduplication state for newBlocks
    std::string lastBlockHash;
    uint64_t lastBlockHeight = 0;
    
    g_logger.info("🔔 Bridge thread started, draining broadcast bus...");
    
    while (running_.load(std::memory_order_relaxed)) {
        bus.wait_for_event();
        batch.clear();
        bus.pop_all(batch, 256);
        
        for (auto& e : batch) {
            try {
                // Parse the JSON to extract info for deduplication
                json::json root;
                try { root = json::Reader().parse(e.json, result); } catch(...) { /* parse failed */ } {
                    // Deduplicate newBlocks
                    if (e.channel == "newBlocks") {
                        if (root.contains("hash") && root.contains("height")) {
                            std::string hash = root["hash"].get<std::string>();
                            uint64_t height = root["height"].get<uint64_t>();
                            
                            if (hash == lastBlockHash && height == lastBlockHeight) {
                                // Duplicate block, skip
                                continue;
                            }
                            
                            lastBlockHash = hash;
                            lastBlockHeight = height;
                        }
                    }
                }
                
                // Log the event being processed
                g_logger.info("🔔 Processing event: " + e.channel + " -> " + e.json.substr(0, 100) + "...");
                
                // Enqueue to subscriptions system
                subs_->enqueue(e.channel, e.json);
                
                // Update metrics
                increment_metric(e.channel);
                
            } catch (const std::exception& ex) {
                g_logger.error("🔔 Error processing event: " + std::string(ex.what()));
            }
        }
        
        // Drain the subscriptions system (send queued messages)
        subs_->drain_once();
    }
    
    g_logger.info("🔔 Bridge thread exiting");
}

void WsBridge::increment_metric(const std::string& channel) {
    // Update legacy counters for backward compatibility
    if (channel == "newBlocks") {
        g_websocket_metrics.out_newBlocks++;
    } else if (channel == "miningInfo") {
        g_websocket_metrics.out_miningInfo++;
    } else if (channel == "mempoolTx") {
        g_websocket_metrics.out_mempoolTx++;
    }
}

} // namespace dinero
