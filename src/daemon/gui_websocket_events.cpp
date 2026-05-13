#include "daemon/ws_globals.h"
#include "common/logger.h"
#include <json/writer.h>
#include <chrono>
#include <iomanip>
#include <sstream>

// Helper function to get current UTC timestamp
[[maybe_unused]] static std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

// Size cap for WebSocket messages (256 KiB)
[[maybe_unused]] static constexpr size_t MAX_WS_MESSAGE_SIZE = 256 * 1024;

namespace dinero_daemon {
namespace gui_events {

// Broadcast new transaction event to GUI subscribers
void BroadcastNewTransaction(const std::string& txid, uint64_t fee, double feerate, uint32_t size) {
#if DIN_WS_BROADCAST
    if (!g_subscriptions) return;

    try {
        uint64_t seq = g_subscriptions->get_next_topic_seq("newTransactions");

        Json::Value tx_event;
        tx_event["type"] = "event";
        tx_event["topic"] = "newTransactions";
        tx_event["seq"] = static_cast<Json::Value::UInt64>(seq);
        tx_event["ts"] = getCurrentTimestamp();
        tx_event["schema"] = "dinero.tx.v1";
        tx_event["source"] = "dinerod";

        Json::Value data;
        data["txid"] = txid;
        data["fee"] = static_cast<Json::Value::UInt64>(fee);
        data["feerate"] = feerate;
        data["size"] = static_cast<Json::Value::UInt>(size);
        tx_event["data"] = data;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string json_str = Json::writeString(builder, tx_event);

        if (json_str.size() > MAX_WS_MESSAGE_SIZE) {
            tx_event["data"] = "[TRIMMED: payload exceeds 256 KiB]";
            tx_event["warn"] = "Data trimmed due to size limit";
            tx_event["original_size"] = static_cast<Json::Value::UInt64>(json_str.size());
            json_str = Json::writeString(builder, tx_event);
        }

        g_subscriptions->enqueue("newTransactions", json_str);
        dinero::g_logger.debug("📡 Broadcast new transaction: " + txid);
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to broadcast newTransactions event: " + std::string(e.what()));
    }
#else
    // Feature disabled - suppress unused parameter warnings
    (void)txid; (void)fee; (void)feerate; (void)size;
#endif
}

// Broadcast network info update to GUI subscribers
void BroadcastNetworkInfo(int peer_count, int inbound_count, int outbound_count) {
#if DIN_WS_BROADCAST
    if (!g_subscriptions) return;

    try {
        uint64_t seq = g_subscriptions->get_next_topic_seq("networkInfo");

        Json::Value net_event;
        net_event["type"] = "event";
        net_event["topic"] = "networkInfo";
        net_event["seq"] = static_cast<Json::Value::UInt64>(seq);
        net_event["ts"] = getCurrentTimestamp();
        net_event["schema"] = "dinero-coin.com.v1";
        net_event["source"] = "dinerod";

        Json::Value data;
        data["connections"] = peer_count;
        data["inbound"] = inbound_count;
        data["outbound"] = outbound_count;
        net_event["data"] = data;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string json_str = Json::writeString(builder, net_event);

        g_subscriptions->enqueue("networkInfo", json_str);
        dinero::g_logger.debug("📡 Broadcast network info: " + std::to_string(peer_count) + " peers");
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to broadcast networkInfo event: " + std::string(e.what()));
    }
#else
    // Feature disabled - suppress unused parameter warnings
    (void)peer_count; (void)inbound_count; (void)outbound_count;
#endif
}

// Broadcast mempool stats update to GUI subscribers
void BroadcastMempoolUpdate(uint32_t tx_count, uint64_t total_bytes, double avg_fee) {
#if DIN_WS_BROADCAST
    if (!g_subscriptions) return;

    try {
        uint64_t seq = g_subscriptions->get_next_topic_seq("mempool");

        Json::Value mempool_event;
        mempool_event["type"] = "event";
        mempool_event["topic"] = "mempool";
        mempool_event["seq"] = static_cast<Json::Value::UInt64>(seq);
        mempool_event["ts"] = getCurrentTimestamp();
        mempool_event["schema"] = "dinero.mempool.v1";
        mempool_event["source"] = "dinerod";

        Json::Value data;
        data["size"] = tx_count;
        data["bytes"] = static_cast<Json::Value::UInt64>(total_bytes);
        data["avgfee"] = avg_fee;
        mempool_event["data"] = data;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string json_str = Json::writeString(builder, mempool_event);

        g_subscriptions->enqueue("mempool", json_str);
        dinero::g_logger.debug("📡 Broadcast mempool update: " + std::to_string(tx_count) + " txs");
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to broadcast mempool event: " + std::string(e.what()));
    }
#else
    // Feature disabled - suppress unused parameter warnings
    (void)tx_count; (void)total_bytes; (void)avg_fee;
#endif
}

// Broadcast sync progress update to GUI subscribers
void BroadcastSyncProgress(bool ibd, double progress, int eta_s) {
#if DIN_WS_BROADCAST
    if (!g_subscriptions) return;

    try {
        // Throttle: only emit on significant change or state transition
        static bool last_ibd = true;
        static int last_pct = -1;
        int pct = static_cast<int>(progress * 100.0 + 0.5);

        // Emit if: state changed (ibd->synced), progress changed >=1%, or first call
        if (last_ibd != ibd || pct != last_pct || last_pct < 0) {
            last_ibd = ibd;
            last_pct = pct;
        } else {
            return; // Skip redundant updates
        }

        uint64_t seq = g_subscriptions->get_next_topic_seq("syncProgress");

        Json::Value sync_event;
        sync_event["type"] = "event";
        sync_event["topic"] = "syncProgress";
        sync_event["seq"] = static_cast<Json::Value::UInt64>(seq);
        sync_event["ts"] = getCurrentTimestamp();
        sync_event["schema"] = "dinero.sync.v2"; // Bumped schema version
        sync_event["source"] = "dinerod";

        Json::Value data;
        data["ibd"] = ibd;                           // true = syncing, false = synced
        data["progress"] = progress;                 // 0.0-1.0 fraction
        data["progressPct"] = pct;                   // 0-100 percentage (convenience)
        data["eta"] = eta_s;                         // estimated seconds remaining
        sync_event["data"] = data;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string json_str = Json::writeString(builder, sync_event);

        g_subscriptions->enqueue("syncProgress", json_str);

        if (ibd) {
            dinero::g_logger.debug("📡 Sync progress: " + std::to_string(pct) + "% (ETA: " +
                                  (eta_s > 0 ? std::to_string(eta_s) + "s" : "unknown") + ")");
        } else {
            dinero::g_logger.debug("📡 Sync complete: Node fully synced");
        }
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to broadcast syncProgress event: " + std::string(e.what()));
    }
#else
    // Feature disabled - suppress unused parameter warnings
    (void)ibd; (void)progress; (void)eta_s;
#endif
}

} // namespace gui_events
} // namespace dinero_daemon
