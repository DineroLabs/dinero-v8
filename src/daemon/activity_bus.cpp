#include "daemon/activity_bus.hpp"
#include "daemon/ws_globals.h"
#include <mutex>
#include <deque>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <json/writer.h>

namespace dinero_daemon {

// Thread-safe activity storage
static std::mutex activity_mutex;
static std::deque<Json::Value> activity_ring;
static constexpr size_t MAX_EVENTS = 500;

// Phase 2.1: Per-topic sequence now managed by Subscriptions class
// (Removed global g_activity_seq - using g_subscriptions->get_next_topic_seq instead)

// Size cap for WebSocket messages (256 KiB)
static constexpr size_t MAX_WS_MESSAGE_SIZE = 256 * 1024;

// Helper function to get current UTC timestamp in ISO format
static std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

void ActivityBus::publish(Json::Value j) {
    // Add timestamp
    j["ts"] = getCurrentTimestamp();

    // Store in ring buffer
    {
        std::lock_guard<std::mutex> lock(activity_mutex);
        activity_ring.push_back(j);
        if (activity_ring.size() > MAX_EVENTS) {
            activity_ring.pop_front();
        }
    }

    // Publish to WebSocket clients (with null-safety check)
    if (!g_subscriptions) {
        return;  // WebSocket server not initialized - skip publishing
    }

    // Phase 2.1: Get next sequence from centralized topic sequence manager
    uint64_t topic_seq = g_subscriptions->get_next_topic_seq("activity");

    // Build event with metadata
    Json::Value event;
    event["type"] = "event";
    event["topic"] = "activity";
    event["seq"] = static_cast<Json::Value::UInt64>(topic_seq);
    event["ts"] = j["ts"];  // Use same timestamp as activity
    event["schema"] = "dinero.activity.v1";
    event["source"] = "dinerod";
    event["data"] = j;

    // Serialize
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string json_str = Json::writeString(builder, event);

    // Size cap: trim data if > 256 KiB
    if (json_str.size() > MAX_WS_MESSAGE_SIZE) {
        // Trim the data field and add warning
        event["data"] = "[TRIMMED: payload exceeds 256 KiB]";
        event["warn"] = "Data payload trimmed due to size limit";
        event["original_size"] = static_cast<Json::Value::UInt64>(json_str.size());
        json_str = Json::writeString(builder, event);
    }

    // Broadcast to all activity subscribers
    g_subscriptions->enqueue("activity", json_str);
}

Json::Value ActivityBus::getActivity(int limit) {
    std::lock_guard<std::mutex> lock(activity_mutex);
    
    Json::Value result(Json::arrayValue);
    int start = std::max<int>(0, static_cast<int>(activity_ring.size()) - limit);
    
    for (int i = start; i < static_cast<int>(activity_ring.size()); ++i) {
        result.append(activity_ring[i]);
    }
    
    return result;
}

void ActivityBus::clear() {
    std::lock_guard<std::mutex> lock(activity_mutex);
    activity_ring.clear();
}

size_t ActivityBus::getCount() {
    std::lock_guard<std::mutex> lock(activity_mutex);
    return activity_ring.size();
}

} // namespace dinero_daemon
