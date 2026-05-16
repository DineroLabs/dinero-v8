#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/peer_scoring_service.h"
#include "common/logger.h"
#include "din_json.h"

namespace dinero {
namespace rpc {

using din::Json;
using ::ExecutionContext;

static P2PService* GetP2PService() {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->p2p) {
        return nullptr;
    }
    return daemon_ctx->p2p.get();
}

/**
 * listbanned - List all banned peers
 *
 * Returns a list of banned peers with their ban expiration times.
 */
static Json listbanned_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    auto* p2p = GetP2PService();
    if (!daemon_ctx || (!daemon_ctx->peer_scoring && !p2p)) {
        throw std::runtime_error("Peer ban services not available");
    }

    Json result = din::arr();

    if (p2p) {
        for (const auto& ban : p2p->ListBannedPeers()) {
            Json entry = din::obj();
            entry["address"] = ban.target;
            entry["peer_id"] = ban.target;
            entry["ban_created"] = static_cast<Json::Int64>(ban.ban_created);
            entry["banned_until"] = static_cast<Json::Int64>(ban.banned_until);
            entry["ban_until"] = static_cast<Json::Int64>(ban.banned_until);
            entry["ban_duration"] = static_cast<Json::Int64>(ban.banned_until - ban.ban_created);
            entry["source"] = "manual";
            result.append(entry);
        }
        return result;
    }

    auto banned_peers = daemon_ctx->peer_scoring->getBannedPeers();

    for (const auto& peer_id : banned_peers) {
        auto peer_score = daemon_ctx->peer_scoring->getPeerScore(peer_id);

        Json entry = din::obj();
        entry["peer_id"] = peer_id;
        entry["score"] = peer_score.score;
        entry["lifetime_score"] = peer_score.lifetime_score;
        entry["misbehavior_count"] = static_cast<Json::UInt>(peer_score.misbehavior_count);
        entry["is_banned"] = peer_score.is_banned;

        auto ban_until_time_t = std::chrono::system_clock::to_time_t(peer_score.ban_until);
        entry["ban_until"] = static_cast<Json::Int64>(ban_until_time_t);

        result.append(entry);
    }

    return result;
}

/**
 * setban "ip" "add|remove" [bantime] [absolute]
 *
 * Manually ban or unban a peer.
 */
static Json setban_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    auto* p2p = GetP2PService();
    if (!daemon_ctx || (!daemon_ctx->peer_scoring && !p2p)) {
        throw std::runtime_error("Peer ban services not available");
    }

    if (params.size() < 2) {
        throw std::invalid_argument("setban requires at least 2 parameters: address command [bantime] [absolute]");
    }

    std::string address = params[0].asString();
    std::string command = params[1].asString();

    if (command == "add") {
        int64_t bantime = 86400; // Default: 24 hours
        bool absolute = false;

        if (params.size() >= 3) {
            bantime = params[2].asInt64();
        }
        if (params.size() >= 4) {
            absolute = params[3].asBool();
        }

        std::chrono::seconds duration;
        if (absolute) {
            // bantime is absolute unix timestamp
            auto ban_until = std::chrono::system_clock::from_time_t(bantime);
            auto now = std::chrono::system_clock::now();
            duration = std::chrono::duration_cast<std::chrono::seconds>(ban_until - now);

            if (duration.count() <= 0) {
                throw std::invalid_argument("Ban time must be in the future");
            }
        } else {
            // bantime is relative duration in seconds
            duration = std::chrono::seconds(bantime);
        }

        if (duration.count() <= 0) {
            throw std::invalid_argument("Ban duration must be positive");
        }

        if (p2p && !p2p->BanPeer(address, duration)) {
            throw std::runtime_error("Failed to ban peer address");
        }
        if (daemon_ctx->peer_scoring) {
            daemon_ctx->peer_scoring->banPeer(address, duration);
        }

        Json result = din::obj();
        result["success"] = true;
        result["message"] = "Peer banned successfully";
        result["address"] = address;
        result["duration_seconds"] = static_cast<Json::Int64>(duration.count());
        return result;

    } else if (command == "remove") {
        if (p2p) {
            p2p->UnbanPeer(address);
        }
        if (daemon_ctx->peer_scoring) {
            daemon_ctx->peer_scoring->unbanPeer(address);
        }

        Json result = din::obj();
        result["success"] = true;
        result["message"] = "Peer unbanned successfully";
        result["address"] = address;
        return result;

    } else {
        throw std::invalid_argument("Invalid command. Use 'add' or 'remove'");
    }
}

/**
 * clearbanned - Clear all banned peers
 *
 * Removes all peer bans immediately.
 */
static Json clearbanned_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    auto* p2p = GetP2PService();
    if (!daemon_ctx || (!daemon_ctx->peer_scoring && !p2p)) {
        throw std::runtime_error("Peer ban services not available");
    }

    if (p2p) {
        p2p->ClearBannedPeers();
    }
    if (daemon_ctx->peer_scoring) {
        daemon_ctx->peer_scoring->clearAllBans();
    }

    Json result = din::obj();
    result["success"] = true;
    result["message"] = "All bans cleared successfully";
    return result;
}

/**
 * getpeerscores [peer_id]
 *
 * Get peer scoring information.
 * If peer_id is provided, returns details for that peer.
 * Otherwise returns overall statistics.
 */
static Json getpeerscores_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->peer_scoring) {
        throw std::runtime_error("Peer scoring service not available");
    }

    if (params.size() >= 1) {
        // Get specific peer score
        std::string peer_id = params[0].asString();
        auto peer_score = daemon_ctx->peer_scoring->getPeerScore(peer_id);

        Json result = din::obj();
        result["peer_id"] = peer_score.peer_id;
        result["score"] = peer_score.score;
        result["lifetime_score"] = peer_score.lifetime_score;
        result["misbehavior_count"] = static_cast<Json::UInt>(peer_score.misbehavior_count);
        result["is_banned"] = peer_score.is_banned;

        auto first_seen_time_t = std::chrono::system_clock::to_time_t(peer_score.first_seen);
        auto last_misbehavior_time_t = std::chrono::system_clock::to_time_t(peer_score.last_misbehavior);
        auto ban_until_time_t = std::chrono::system_clock::to_time_t(peer_score.ban_until);

        result["first_seen"] = static_cast<Json::Int64>(first_seen_time_t);
        result["last_misbehavior"] = static_cast<Json::Int64>(last_misbehavior_time_t);
        result["ban_until"] = static_cast<Json::Int64>(ban_until_time_t);

        // Add misbehavior history
        Json history = din::arr();
        for (const auto& entry : peer_score.history) {
            Json h = din::obj();
            h["type"] = static_cast<int>(entry.first);
            auto timestamp = std::chrono::system_clock::to_time_t(entry.second);
            h["timestamp"] = static_cast<Json::Int64>(timestamp);
            history.append(h);
        }
        result["history"] = history;

        return result;

    } else {
        // Get overall statistics
        auto stats = daemon_ctx->peer_scoring->getStats();

        Json result = din::obj();
        result["total_peers"] = static_cast<Json::UInt64>(stats.total_peers);
        result["banned_peers"] = static_cast<Json::UInt64>(stats.banned_peers);
        result["misbehaving_peers"] = static_cast<Json::UInt64>(stats.misbehaving_peers);
        result["avg_score"] = stats.avg_score;
        result["total_misbehaviors"] = static_cast<Json::UInt64>(stats.total_misbehaviors);

        auto oldest_ban_time_t = std::chrono::system_clock::to_time_t(stats.oldest_ban);
        result["oldest_ban"] = static_cast<Json::Int64>(oldest_ban_time_t);

        return result;
    }
}

/**
 * Register peer scoring methods in the RPC registry
 */
void register_peer_scoring_methods() {
    g_rpcRegistry.registerHandler("listbanned",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return listbanned_impl(ctx, params);
        },
        "network");

    g_rpcRegistry.registerHandler("setban",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return setban_impl(ctx, params);
        },
        "network");

    g_rpcRegistry.registerHandler("clearbanned",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return clearbanned_impl(ctx, params);
        },
        "network");

    g_rpcRegistry.registerHandler("getpeerscores",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return getpeerscores_impl(ctx, params);
        },
        "network");
}

} // namespace rpc
} // namespace dinero
