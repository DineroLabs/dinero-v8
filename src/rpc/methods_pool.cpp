/**
 * Pool RPC Methods - Mining Pool Accounting API
 *
 * Provides RPC interface for mining pool operations:
 * - Pool statistics and configuration
 * - Worker management and stats
 * - Block and payout tracking
 * - Share statistics
 */

#include "rpc/rpc_method_builder.h"
#include "pool/pool_db.h"
#include "pool/pool_manager.h"
#include "pool/pool_types.h"
#include "pool/payout_calculator.h"
#include "common/logger.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <sstream>
#include <iomanip>
#include <mutex>

namespace din {
namespace rpc {

// Global pool database pointer (configured by daemon startup).
std::shared_ptr<dinero::pool::PoolDB> g_pool_db = nullptr;
std::shared_ptr<dinero::pool::PoolManager> g_pool_manager = nullptr;

namespace {

std::mutex g_pool_rpc_mutex;
bool g_pool_rpc_enabled = false;
std::string g_pool_rpc_disabled_reason =
    "Pool accounting is disabled (set --pool.accounting.enable=1 on a pool-capable sync profile)";

din::Json makePoolError(int code, const std::string& message) {
    din::Json error = din::obj();
    error["error"]["code"] = code;
    error["error"]["message"] = message;
    return error;
}

struct PoolRuntimeSnapshot {
    std::shared_ptr<dinero::pool::PoolDB> db;
    std::shared_ptr<dinero::pool::PoolManager> manager;
    bool enabled = false;
    std::string disabled_reason;
};

PoolRuntimeSnapshot snapshotPoolRuntime() {
    std::lock_guard<std::mutex> lock(g_pool_rpc_mutex);
    return PoolRuntimeSnapshot{
        g_pool_db,
        g_pool_manager,
        g_pool_rpc_enabled,
        g_pool_rpc_disabled_reason
    };
}

bool resolvePoolDb(std::shared_ptr<dinero::pool::PoolDB>& db_out, din::Json& error_out) {
    const auto snapshot = snapshotPoolRuntime();
    if (!snapshot.enabled) {
        error_out = makePoolError(-32021, snapshot.disabled_reason);
        return false;
    }

    if (!snapshot.db || !snapshot.db->isOpen()) {
        error_out = makePoolError(-32022, "Pool accounting database is not initialized");
        return false;
    }

    db_out = snapshot.db;
    return true;
}

bool resolvePoolManager(std::shared_ptr<dinero::pool::PoolManager>& manager_out, din::Json& error_out) {
    const auto snapshot = snapshotPoolRuntime();
    if (!snapshot.enabled) {
        error_out = makePoolError(-32021, snapshot.disabled_reason);
        return false;
    }

    if (!snapshot.manager || !snapshot.manager->isRunning()) {
        error_out = makePoolError(-32023, "Pool manager is not initialized");
        return false;
    }

    manager_out = snapshot.manager;
    return true;
}

const din::Json* getParam(const din::Json& params, size_t positional_index, const char* named_key) {
    if (params.isArray() && positional_index < params.size()) {
        return &params[static_cast<din::Json::ArrayIndex>(positional_index)];
    }
    if (params.isObject() && named_key && params.isMember(named_key)) {
        return &params[named_key];
    }
    return nullptr;
}

bool readRequiredStringParam(const din::Json& params,
                             size_t positional_index,
                             const char* named_key,
                             std::string& out,
                             std::string& error) {
    const din::Json* value = getParam(params, positional_index, named_key);
    if (!value || !value->isString() || value->asString().empty()) {
        error = std::string("Missing required parameter: ") + named_key;
        return false;
    }
    out = value->asString();
    return true;
}

bool readDoubleParam(const din::Json& params,
                     size_t positional_index,
                     const char* named_key,
                     double default_value,
                     double& out) {
    const din::Json* value = getParam(params, positional_index, named_key);
    if (!value) {
        out = default_value;
        return true;
    }
    if (!value->isNumeric()) {
        return false;
    }
    out = value->asDouble();
    return true;
}

bool readBoolParam(const din::Json& params,
                   size_t positional_index,
                   const char* named_key,
                   bool default_value,
                   bool& out) {
    const din::Json* value = getParam(params, positional_index, named_key);
    if (!value) {
        out = default_value;
        return true;
    }
    if (value->isBool()) {
        out = value->asBool();
        return true;
    }
    if (value->isIntegral()) {
        const auto v = value->asInt64();
        if (v == 0 || v == 1) {
            out = (v == 1);
            return true;
        }
        return false;
    }
    if (value->isString()) {
        std::string s = value->asString();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (s == "true" || s == "1") {
            out = true;
            return true;
        }
        if (s == "false" || s == "0") {
            out = false;
            return true;
        }
    }
    return false;
}

bool readUint32Param(const din::Json& params,
                     size_t positional_index,
                     const char* named_key,
                     uint32_t default_value,
                     uint32_t& out) {
    const din::Json* value = getParam(params, positional_index, named_key);
    if (!value) {
        out = default_value;
        return true;
    }
    if (!value->isIntegral()) {
        return false;
    }
    const auto n = value->asInt64();
    if (n < 0 || n > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out = static_cast<uint32_t>(n);
    return true;
}

bool readUint64Param(const din::Json& params,
                     size_t positional_index,
                     const char* named_key,
                     uint64_t default_value,
                     uint64_t& out) {
    const din::Json* value = getParam(params, positional_index, named_key);
    if (!value) {
        out = default_value;
        return true;
    }
    if (!value->isIntegral()) {
        return false;
    }
    const auto n = value->asInt64();
    if (n < 0) {
        return false;
    }
    out = static_cast<uint64_t>(n);
    return true;
}

bool isHex(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool isWorkerIdSafe(const std::string& worker_id) {
    if (worker_id.empty() || worker_id.size() > 128) {
        return false;
    }
    return std::all_of(worker_id.begin(), worker_id.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
    });
}

bool isTaprootAddress(const std::string& address) {
    if (address.size() < 8 || address.size() > 96) {
        return false;
    }
    return address.rfind("din1p", 0) == 0 ||
           address.rfind("tdin1p", 0) == 0 ||
           address.rfind("rdin1p", 0) == 0;
}

}  // namespace

void configurePoolRpc(std::shared_ptr<dinero::pool::PoolDB> pool_db,
                      std::shared_ptr<dinero::pool::PoolManager> pool_manager,
                      bool enabled,
                      const std::string& disabled_reason) {
    std::lock_guard<std::mutex> lock(g_pool_rpc_mutex);
    g_pool_db = std::move(pool_db);
    g_pool_manager = std::move(pool_manager);
    g_pool_rpc_enabled = enabled;

    if (!enabled) {
        g_pool_rpc_disabled_reason = disabled_reason.empty()
            ? "Pool accounting is disabled"
            : disabled_reason;
        return;
    }

    if (!g_pool_db || !g_pool_db->isOpen()) {
        g_pool_rpc_enabled = false;
        g_pool_rpc_disabled_reason = "Pool accounting enabled but database is unavailable";
        return;
    }
    if (!g_pool_manager || !g_pool_manager->isRunning()) {
        g_pool_rpc_enabled = false;
        g_pool_rpc_disabled_reason = "Pool accounting enabled but manager is unavailable";
        return;
    }

    g_pool_rpc_disabled_reason.clear();
}

// Helper: Convert una to DIN string
static std::string unaToString(uint64_t una) {
    double din = static_cast<double>(una) / 100000000.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8) << din;
    return oss.str();
}

// Helper: Format timestamp
static std::string formatTimestamp(int64_t timestamp) {
    if (timestamp == 0) return "never";
    std::time_t t = static_cast<std::time_t>(timestamp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

void registerPoolMethods() {
    RPC_METHOD("pool.status", "pool")
        .description("Get pool accounting status and runtime gate state")
        .params({})
        .result("object", "Pool feature status")
        .handler([](const ExecutionContext&, const din::Json&) {
            din::Json result = din::obj();
            const auto snapshot = snapshotPoolRuntime();

            result["enabled"] = snapshot.enabled;
            result["database_open"] = snapshot.db && snapshot.db->isOpen();
            result["ready"] = snapshot.enabled && snapshot.db && snapshot.db->isOpen();
            if (!snapshot.enabled) {
                result["disabled_reason"] = snapshot.disabled_reason;
            }
            return result;
        })
        .examples({
            "pool.status"
        });

    RPC_METHOD("pool.authorizeworker", "pool")
        .description("Authorize/register a worker for pool accounting")
        .param("worker_id", "string", "Worker identifier (for example user.rig1)", true)
        .param("wallet_address", "string", "Worker payout address", true)
        .result("object", "Authorization result")
        .handler([](const ExecutionContext&, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolManager> pool_manager;
            if (!resolvePoolManager(pool_manager, result)) {
                return result;
            }

            std::string worker_id;
            std::string wallet_address;
            std::string parse_error;
            if (!readRequiredStringParam(params, 0, "worker_id", worker_id, parse_error) ||
                !readRequiredStringParam(params, 1, "wallet_address", wallet_address, parse_error)) {
                return makePoolError(-32602, parse_error);
            }
            if (!isWorkerIdSafe(worker_id)) {
                return makePoolError(-32602, "Invalid worker_id: use [A-Za-z0-9._-], max 128 chars");
            }
            if (!isTaprootAddress(wallet_address)) {
                return makePoolError(-32602, "Invalid wallet_address: taproot address required (din1p...)");
            }

            if (!pool_manager->onWorkerAuthorize(worker_id, wallet_address)) {
                return makePoolError(-32030, "Failed to authorize worker");
            }

            result["success"] = true;
            result["worker_id"] = worker_id;
            result["wallet_address"] = wallet_address;
            return result;
        })
        .examples({
            "pool.authorizeworker \"miner1.rig1\" \"din1qexample...\"",
            "pool.authorizeworker {\"worker_id\":\"miner1.rig1\",\"wallet_address\":\"din1qexample...\"}"
        });

    RPC_METHOD("pool.submitshare", "pool")
        .description("Submit share accounting result for a worker")
        .param("worker_id", "string", "Worker identifier", true)
        .param("job_id", "string", "Stratum job identifier", true)
        .param("difficulty", "number", "Share difficulty", true)
        .param("is_valid", "boolean", "Whether share is valid (default: true)", false)
        .param("is_stale", "boolean", "Whether share is stale (default: false)", false)
        .param("is_block", "boolean", "Whether share found a block (default: false)", false)
        .param("share_uid", "string", "Optional unique share identifier for dedupe", false)
        .param("block_hash", "string", "Found block hash (required if is_block=true)", false)
        .param("block_height", "number", "Found block height (required if is_block=true)", false)
        .param("block_reward", "number", "Found block reward in una (required if is_block=true)", false)
        .result("object", "Share submission result")
        .handler([](const ExecutionContext&, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolManager> pool_manager;
            if (!resolvePoolManager(pool_manager, result)) {
                return result;
            }

            std::string worker_id;
            std::string job_id;
            std::string parse_error;
            if (!readRequiredStringParam(params, 0, "worker_id", worker_id, parse_error) ||
                !readRequiredStringParam(params, 1, "job_id", job_id, parse_error)) {
                return makePoolError(-32602, parse_error);
            }
            if (!isWorkerIdSafe(worker_id)) {
                return makePoolError(-32602, "Invalid worker_id: use [A-Za-z0-9._-], max 128 chars");
            }
            if (job_id.size() > 128) {
                return makePoolError(-32602, "Invalid parameter: job_id too long");
            }

            double difficulty = 0.0;
            if (!readDoubleParam(params, 2, "difficulty", 0.0, difficulty) || difficulty <= 0.0) {
                return makePoolError(-32602, "Invalid parameter: difficulty must be > 0");
            }

            bool is_valid = true;
            bool is_stale = false;
            bool is_block = false;
            if (!readBoolParam(params, 3, "is_valid", true, is_valid)) {
                return makePoolError(-32602, "Invalid parameter: is_valid must be boolean");
            }
            if (!readBoolParam(params, 4, "is_stale", false, is_stale)) {
                return makePoolError(-32602, "Invalid parameter: is_stale must be boolean");
            }
            if (!readBoolParam(params, 5, "is_block", false, is_block)) {
                return makePoolError(-32602, "Invalid parameter: is_block must be boolean");
            }

            std::string block_hash;
            uint32_t block_height = 0;
            uint64_t block_reward = 0;
            std::string share_uid;
            if (params.isArray()) {
                if (!is_block) {
                    if (params.size() > 6) {
                        const auto& uid_param = params[6];
                        if (!uid_param.isString()) {
                            return makePoolError(-32602, "Invalid parameter: share_uid must be string");
                        }
                        share_uid = uid_param.asString();
                    }
                } else {
                    if (params.size() < 9) {
                        return makePoolError(-32602, "Missing required parameters for block share");
                    }

                    const auto& hash_param = params[params.size() - 3];
                    const auto& height_param = params[params.size() - 2];
                    const auto& reward_param = params[params.size() - 1];
                    if (!hash_param.isString()) {
                        return makePoolError(-32602, "Invalid parameter: block_hash must be string");
                    }
                    block_hash = hash_param.asString();
                    if (block_hash.size() != 64 || !isHex(block_hash)) {
                        return makePoolError(-32602, "Invalid parameter: block_hash must be 64 hex chars");
                    }
                    if (!height_param.isIntegral() || height_param.asInt64() <= 0 ||
                        height_param.asInt64() > std::numeric_limits<uint32_t>::max()) {
                        return makePoolError(-32602, "Invalid parameter: block_height must be > 0");
                    }
                    if (!reward_param.isIntegral() || reward_param.asInt64() <= 0) {
                        return makePoolError(-32602, "Invalid parameter: block_reward must be > 0");
                    }
                    block_height = static_cast<uint32_t>(height_param.asInt64());
                    block_reward = static_cast<uint64_t>(reward_param.asInt64());

                    if (params.size() >= 10) {
                        const auto& uid_param = params[params.size() - 4];
                        if (!uid_param.isString()) {
                            return makePoolError(-32602, "Invalid parameter: share_uid must be string");
                        }
                        share_uid = uid_param.asString();
                    }
                }
            } else {
                if (const din::Json* uid_param = getParam(params, 6, "share_uid")) {
                    if (!uid_param->isString()) {
                        return makePoolError(-32602, "Invalid parameter: share_uid must be string");
                    }
                    share_uid = uid_param->asString();
                }

                if (is_block) {
                    if (!readRequiredStringParam(params, 7, "block_hash", block_hash, parse_error)) {
                        return makePoolError(-32602, parse_error);
                    }
                    if (block_hash.size() != 64 || !isHex(block_hash)) {
                        return makePoolError(-32602, "Invalid parameter: block_hash must be 64 hex chars");
                    }
                    if (!readUint32Param(params, 8, "block_height", 0, block_height) || block_height == 0) {
                        return makePoolError(-32602, "Invalid parameter: block_height must be > 0");
                    }
                    if (!readUint64Param(params, 9, "block_reward", 0, block_reward) || block_reward == 0) {
                        return makePoolError(-32602, "Invalid parameter: block_reward must be > 0");
                    }
                }
            }

            if (share_uid.size() > 128) {
                return makePoolError(-32602, "Invalid parameter: share_uid too long");
            }

            const auto submit = pool_manager->onShareSubmit(worker_id,
                                                            job_id,
                                                            difficulty,
                                                            is_valid,
                                                            is_stale,
                                                            is_block,
                                                            block_hash,
                                                            block_height,
                                                            block_reward,
                                                            share_uid);

            switch (submit.code) {
                case dinero::pool::PoolManager::ShareSubmitCode::RATE_LIMITED:
                    return makePoolError(-32031, submit.message.empty() ? "Share submission rate-limited" : submit.message);
                case dinero::pool::PoolManager::ShareSubmitCode::UNKNOWN_WORKER:
                    return makePoolError(-32032, submit.message.empty() ? "Worker not authorized" : submit.message);
                case dinero::pool::PoolManager::ShareSubmitCode::REJECTED:
                    return makePoolError(-32033, submit.message.empty() ? "Share submission rejected" : submit.message);
                case dinero::pool::PoolManager::ShareSubmitCode::DUPLICATE:
                    result["success"] = true;
                    result["duplicate"] = true;
                    result["status"] = "duplicate";
                    result["worker_id"] = worker_id;
                    result["job_id"] = job_id;
                    return result;
                case dinero::pool::PoolManager::ShareSubmitCode::ACCEPTED:
                    break;
            }

            result["success"] = true;
            result["worker_id"] = worker_id;
            result["job_id"] = job_id;
            result["difficulty"] = difficulty;
            result["status"] = submit.status == dinero::pool::ShareStatus::VALID ? "valid" :
                               submit.status == dinero::pool::ShareStatus::STALE ? "stale" :
                               submit.status == dinero::pool::ShareStatus::INVALID ? "invalid" :
                               submit.status == dinero::pool::ShareStatus::BLOCK ? "block" :
                               submit.status == dinero::pool::ShareStatus::DUPLICATE ? "duplicate" : "unknown";
            if (is_block) {
                result["block_hash"] = block_hash;
                result["block_height"] = static_cast<int>(block_height);
                result["block_reward_una"] = static_cast<din::Json::UInt64>(block_reward);
            }
            if (!share_uid.empty()) {
                result["share_uid"] = share_uid;
            }
            return result;
        })
        .examples({
            "pool.submitshare \"miner1.rig1\" \"job123\" 1024.0",
            "pool.submitshare {\"worker_id\":\"miner1.rig1\",\"job_id\":\"job123\",\"difficulty\":1024.0,\"is_valid\":true,\"is_stale\":false,\"is_block\":false}"
        });

    RPC_METHOD("pool.disconnectworker", "pool")
        .description("Mark a worker as disconnected")
        .param("worker_id", "string", "Worker identifier", true)
        .result("object", "Disconnect result")
        .handler([](const ExecutionContext&, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolManager> pool_manager;
            if (!resolvePoolManager(pool_manager, result)) {
                return result;
            }

            std::string worker_id;
            std::string parse_error;
            if (!readRequiredStringParam(params, 0, "worker_id", worker_id, parse_error)) {
                return makePoolError(-32602, parse_error);
            }
            if (!isWorkerIdSafe(worker_id)) {
                return makePoolError(-32602, "Invalid worker_id: use [A-Za-z0-9._-], max 128 chars");
            }

            pool_manager->onWorkerDisconnect(worker_id);
            result["success"] = true;
            result["worker_id"] = worker_id;
            return result;
        })
        .examples({
            "pool.disconnectworker \"miner1.rig1\"",
            "pool.disconnectworker {\"worker_id\":\"miner1.rig1\"}"
        });

    // ═══════════════════════════════════════════════════════════════════════════
    // POOL STATISTICS
    // ═══════════════════════════════════════════════════════════════════════════

    RPC_METHOD("pool.stats", "pool")
        .description("Get overall pool statistics")
        .params({})
        .result("object", "Pool statistics including hashrate, workers, blocks, and payouts")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }

            auto stats = pool_db->getPoolStats();
            auto config = pool_db->getConfig();

            // Workers
            result["active_workers"] = static_cast<int>(stats.active_workers);
            result["total_workers"] = static_cast<int>(stats.total_workers);

            // Hashrate
            result["pool_hashrate"] = stats.pool_hashrate;
            result["pool_hashrate_formatted"] = [&]() {
                double hr = stats.pool_hashrate;
                if (hr >= 1e15) return std::to_string(hr / 1e15) + " PH/s";
                if (hr >= 1e12) return std::to_string(hr / 1e12) + " TH/s";
                if (hr >= 1e9) return std::to_string(hr / 1e9) + " GH/s";
                if (hr >= 1e6) return std::to_string(hr / 1e6) + " MH/s";
                if (hr >= 1e3) return std::to_string(hr / 1e3) + " KH/s";
                return std::to_string(hr) + " H/s";
            }();

            // Shares
            result["total_shares"] = static_cast<int64_t>(stats.total_shares);
            result["shares_per_second"] = static_cast<int64_t>(stats.shares_per_second);

            // Blocks
            result["blocks_found"] = static_cast<int64_t>(stats.blocks_found);
            result["blocks_orphaned"] = static_cast<int64_t>(stats.blocks_orphaned);
            result["blocks_pending"] = static_cast<int64_t>(stats.blocks_pending);

            // Payouts
            result["total_paid"] = unaToString(stats.total_paid);
            result["total_paid_una"] = static_cast<int64_t>(stats.total_paid);
            result["pending_payouts"] = unaToString(stats.pending_payouts);
            result["pending_payouts_una"] = static_cast<int64_t>(stats.pending_payouts);

            // Current round (PROP mode)
            result["current_round_shares"] = static_cast<int64_t>(stats.round_shares);
            result["current_round_start"] = formatTimestamp(stats.round_start);

            // Luck
            result["luck_1d"] = stats.luck_1d;
            result["luck_7d"] = stats.luck_7d;
            result["luck_30d"] = stats.luck_30d;

            // Config
            result["payout_mode"] = dinero::pool::PayoutModeToString(config.payout_mode);
            result["pool_fee_percent"] = config.pool_fee_percent;
            result["min_payout"] = unaToString(config.min_payout);
            result["min_auto_payout"] = unaToString(config.min_auto_payout);
            result["max_payout_retries"] = static_cast<int>(config.max_payout_retries);

            return result;
        })
        .examples({
            "pool.stats"
        });

    // ═══════════════════════════════════════════════════════════════════════════
    // POOL CONFIGURATION
    // ═══════════════════════════════════════════════════════════════════════════

    RPC_METHOD("pool.getconfig", "pool")
        .description("Get pool configuration")
        .params({})
        .result("object", "Pool configuration including payout mode, fees, and thresholds")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }

            auto config = pool_db->getConfig();

            result["payout_mode"] = dinero::pool::PayoutModeToString(config.payout_mode);
            result["pplns_window"] = static_cast<int64_t>(config.pplns_window);
            result["pps_rate"] = config.pps_rate;
            result["pool_fee_percent"] = config.pool_fee_percent;
            result["pool_fee_address"] = config.pool_fee_address;
            result["min_payout"] = unaToString(config.min_payout);
            result["min_payout_una"] = static_cast<int64_t>(config.min_payout);
            result["min_auto_payout"] = unaToString(config.min_auto_payout);
            result["min_auto_payout_una"] = static_cast<int64_t>(config.min_auto_payout);
            result["max_payout_retries"] = static_cast<int>(config.max_payout_retries);
            result["required_confirmations"] = static_cast<int>(config.required_confirmations);
            result["new_round_on_block"] = config.new_round_on_block;

            return result;
        })
        .examples({
            "pool.getconfig"
        });

    RPC_METHOD("pool.setconfig", "pool")
        .description("Update pool configuration")
        .param("payout_mode", "string", "Payout mode: PROP, PPLNS, PPS, or SOLO", false)
        .param("pool_fee_percent", "number", "Pool fee percentage (0-100)", false)
        .param("min_payout", "number", "Minimum payout threshold in DIN", false)
        .param("min_auto_payout", "number", "Auto-payout threshold in DIN", false)
        .param("required_confirmations", "number", "Block confirmations required before payout", false)
        .param("new_round_on_block", "boolean", "Start a new accounting round when a block is found", false)
        .param("max_payout_retries", "number", "Retry cap for failed payout attempts", false)
        .param("pplns_window", "number", "PPLNS window size (number of shares)", false)
        .result("object", "Updated configuration")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }

            if (!params.isNull() && !params.isObject()) {
                return makePoolError(-32602, "Invalid parameter: object payload expected");
            }

            auto config = pool_db->getConfig();

            // Update fields if provided (using JsonCpp API)
            if (params.isMember("payout_mode")) {
                config.payout_mode = dinero::pool::StringToPayoutMode(params["payout_mode"].asString());
            }
            if (params.isMember("pool_fee_percent")) {
                config.pool_fee_percent = params["pool_fee_percent"].asDouble();
            }
            if (params.isMember("min_payout")) {
                // Convert DIN to una
                config.min_payout = static_cast<uint64_t>(params["min_payout"].asDouble() * 100000000);
            }
            if (params.isMember("min_auto_payout")) {
                config.min_auto_payout = static_cast<uint64_t>(params["min_auto_payout"].asDouble() * 100000000);
            }
            if (params.isMember("pplns_window")) {
                config.pplns_window = static_cast<uint64_t>(params["pplns_window"].asInt64());
            }
            if (params.isMember("required_confirmations")) {
                config.required_confirmations = static_cast<uint32_t>(params["required_confirmations"].asUInt());
            }
            if (params.isMember("new_round_on_block")) {
                config.new_round_on_block = params["new_round_on_block"].asBool();
            }
            if (params.isMember("max_payout_retries")) {
                config.max_payout_retries = static_cast<uint32_t>(params["max_payout_retries"].asUInt());
            }

            if (pool_db->updateConfig(config)) {
                result["success"] = true;
                result["message"] = "Configuration updated";
            } else {
                result["success"] = false;
                result["error"] = "Failed to update configuration";
            }

            return result;
        })
        .examples({
            "pool.setconfig {\"payout_mode\": \"PPLNS\", \"pool_fee_percent\": 1.0}"
        });

    // ═══════════════════════════════════════════════════════════════════════════
    // WORKER OPERATIONS
    // ═══════════════════════════════════════════════════════════════════════════

    RPC_METHOD("pool.workers", "pool")
        .description("Get list of active workers")
        .param("all", "boolean", "Include inactive workers (default: false)", false)
        .result("array", "List of worker statistics")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }
            result = din::arr();

            bool include_all = false;
            if (params.isMember("all")) {
                include_all = params["all"].asBool();
            }

            std::vector<dinero::pool::WorkerStats> workers;
            if (include_all) {
                workers = pool_db->getWorkersWithPendingBalance(0);
            } else {
                workers = pool_db->getActiveWorkers(900);  // 15 minutes
            }

            for (const auto& w : workers) {
                din::Json worker = din::obj();
                worker["worker_id"] = w.worker_id;
                worker["wallet_address"] = w.wallet_address;
                worker["shares_valid"] = static_cast<int64_t>(w.shares_valid);
                worker["shares_stale"] = static_cast<int64_t>(w.shares_stale);
                worker["shares_invalid"] = static_cast<int64_t>(w.shares_invalid);
                worker["blocks_found"] = static_cast<int64_t>(w.blocks_found);
                worker["current_difficulty"] = w.current_difficulty;
                worker["hashrate_1m"] = w.hashrate_1m;
                worker["hashrate_15m"] = w.hashrate_15m;
                worker["hashrate_1h"] = w.hashrate_1h;
                worker["hashrate_24h"] = w.hashrate_24h;
                worker["pending_payout"] = unaToString(w.pending_payout);
                worker["pending_payout_una"] = static_cast<int64_t>(w.pending_payout);
                worker["total_paid"] = unaToString(w.total_paid);
                worker["total_paid_una"] = static_cast<int64_t>(w.total_paid);
                worker["last_share"] = formatTimestamp(w.last_share);
                worker["first_seen"] = formatTimestamp(w.first_seen);

                result.append(worker);
            }

            return result;
        })
        .examples({
            "pool.workers",
            "pool.workers {\"all\": true}"
        });

    RPC_METHOD("pool.worker", "pool")
        .description("Get detailed stats for a specific worker")
        .param("worker_id", "string", "Worker identifier", true)
        .result("object", "Detailed worker statistics")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }

            std::string worker_id = params[0].asString();
            auto worker = pool_db->getWorker(worker_id);

            if (!worker) {
                result["error"] = "Worker not found";
                return result;
            }

            result["worker_id"] = worker->worker_id;
            result["wallet_address"] = worker->wallet_address;
            result["shares_valid"] = static_cast<int64_t>(worker->shares_valid);
            result["shares_stale"] = static_cast<int64_t>(worker->shares_stale);
            result["shares_invalid"] = static_cast<int64_t>(worker->shares_invalid);
            result["blocks_found"] = static_cast<int64_t>(worker->blocks_found);
            result["current_difficulty"] = worker->current_difficulty;
            result["total_difficulty"] = worker->total_difficulty;
            result["hashrate_1m"] = worker->hashrate_1m;
            result["hashrate_15m"] = worker->hashrate_15m;
            result["hashrate_1h"] = worker->hashrate_1h;
            result["hashrate_24h"] = worker->hashrate_24h;
            result["total_earned"] = unaToString(worker->total_earned);
            result["total_earned_una"] = static_cast<int64_t>(worker->total_earned);
            result["pending_payout"] = unaToString(worker->pending_payout);
            result["pending_payout_una"] = static_cast<int64_t>(worker->pending_payout);
            result["total_paid"] = unaToString(worker->total_paid);
            result["total_paid_una"] = static_cast<int64_t>(worker->total_paid);
            result["first_seen"] = formatTimestamp(worker->first_seen);
            result["last_seen"] = formatTimestamp(worker->last_seen);
            result["last_share"] = formatTimestamp(worker->last_share);

            // Get recent shares
            auto shares = pool_db->getWorkerShares(worker_id, 10);
            din::Json recent_shares = din::arr();
            for (const auto& s : shares) {
                din::Json share = din::obj();
                share["share_id"] = static_cast<int64_t>(s.share_id);
                share["difficulty"] = s.difficulty_real;
                share["status"] = [&]() {
                    switch (s.status) {
                        case dinero::pool::ShareStatus::VALID: return "valid";
                        case dinero::pool::ShareStatus::STALE: return "stale";
                        case dinero::pool::ShareStatus::DUPLICATE: return "duplicate";
                        case dinero::pool::ShareStatus::INVALID: return "invalid";
                        case dinero::pool::ShareStatus::BLOCK: return "block";
                        default: return "unknown";
                    }
                }();
                share["submitted_at"] = formatTimestamp(s.submitted_at);
                recent_shares.append(share);
            }
            result["recent_shares"] = recent_shares;

            // Get recent payouts
            auto payouts = pool_db->getWorkerPayouts(worker_id, 10);
            din::Json recent_payouts = din::arr();
            for (const auto& p : payouts) {
                din::Json payout = din::obj();
                payout["payout_id"] = static_cast<int64_t>(p.payout_id);
                payout["amount"] = unaToString(p.amount);
                payout["amount_una"] = static_cast<int64_t>(p.amount);
                payout["status"] = [&]() {
                    switch (p.status) {
                        case dinero::pool::PayoutStatus::PENDING: return "pending";
                        case dinero::pool::PayoutStatus::CONFIRMED: return "confirmed";
                        case dinero::pool::PayoutStatus::PAID: return "paid";
                        case dinero::pool::PayoutStatus::FAILED: return "failed";
                        default: return "unknown";
                    }
                }();
                payout["txid"] = p.txid;
                payout["calculated_at"] = formatTimestamp(p.calculated_at);
                payout["paid_at"] = formatTimestamp(p.paid_at);
                recent_payouts.append(payout);
            }
            result["recent_payouts"] = recent_payouts;

            return result;
        })
        .examples({
            "pool.worker \"miner1.rig1\""
        });

    // ═══════════════════════════════════════════════════════════════════════════
    // BLOCK OPERATIONS
    // ═══════════════════════════════════════════════════════════════════════════

    RPC_METHOD("pool.blocks", "pool")
        .description("Get list of blocks found by pool")
        .param("limit", "number", "Maximum number of blocks to return (default: 50)", false)
        .result("array", "List of blocks")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }
            result = din::arr();

            uint32_t limit = 50;
            if (params.size() > 0 && params[0].isInt()) {
                limit = static_cast<uint32_t>(params[0].asInt());
            }

            auto blocks = pool_db->getRecentBlocks(limit);

            for (const auto& b : blocks) {
                din::Json block = din::obj();
                block["block_id"] = static_cast<int64_t>(b.block_id);
                block["block_hash"] = b.block_hash;
                block["height"] = static_cast<int>(b.height);
                block["finder_worker"] = b.finder_worker;
                block["finder_address"] = b.finder_address;
                block["reward"] = unaToString(b.reward);
                block["reward_una"] = static_cast<int64_t>(b.reward);
                block["fees"] = unaToString(b.fees);
                block["total_reward"] = unaToString(b.total_reward);
                block["pool_fee_amount"] = unaToString(b.pool_fee_amount);
                block["distributable"] = unaToString(b.distributable);
                block["round_shares"] = static_cast<int64_t>(b.round_shares);
                block["confirmations"] = static_cast<int>(b.confirmations);
                block["required_confirmations"] = static_cast<int>(b.required_confirmations);
                block["orphaned"] = b.orphaned;
                block["payouts_calculated"] = b.payouts_calculated;
                block["payouts_sent"] = b.payouts_sent;
                block["found_at"] = formatTimestamp(b.found_at);
                block["confirmed_at"] = formatTimestamp(b.confirmed_at);

                result.append(block);
            }

            return result;
        })
        .examples({
            "pool.blocks",
            "pool.blocks 100"
        });

    RPC_METHOD("pool.block", "pool")
        .description("Get detailed info for a specific block")
        .param("block_id", "number", "Block ID", true)
        .result("object", "Detailed block information including payouts")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }

            uint64_t block_id = static_cast<uint64_t>(params[0].asInt64());
            auto block = pool_db->getBlock(block_id);

            if (!block) {
                result["error"] = "Block not found";
                return result;
            }

            result["block_id"] = static_cast<int64_t>(block->block_id);
            result["block_hash"] = block->block_hash;
            result["height"] = static_cast<int>(block->height);
            result["finder_worker"] = block->finder_worker;
            result["finder_address"] = block->finder_address;
            result["reward"] = unaToString(block->reward);
            result["fees"] = unaToString(block->fees);
            result["total_reward"] = unaToString(block->total_reward);
            result["pool_fee_percent"] = block->pool_fee_percent;
            result["pool_fee_amount"] = unaToString(block->pool_fee_amount);
            result["distributable"] = unaToString(block->distributable);
            result["round_shares"] = static_cast<int64_t>(block->round_shares);
            result["round_difficulty"] = block->round_difficulty;
            result["confirmations"] = static_cast<int>(block->confirmations);
            result["required_confirmations"] = static_cast<int>(block->required_confirmations);
            result["orphaned"] = block->orphaned;
            result["payouts_calculated"] = block->payouts_calculated;
            result["payouts_sent"] = block->payouts_sent;
            result["found_at"] = formatTimestamp(block->found_at);
            result["confirmed_at"] = formatTimestamp(block->confirmed_at);

            // Get payouts for this block
            auto payouts = pool_db->getPayoutsForBlock(block_id);
            din::Json payout_list = din::arr();
            for (const auto& p : payouts) {
                din::Json payout = din::obj();
                payout["worker_id"] = p.worker_id;
                payout["wallet_address"] = p.wallet_address;
                payout["amount"] = unaToString(p.amount);
                payout["share_percent"] = p.share_percent;
                payout["status"] = [&]() {
                    switch (p.status) {
                        case dinero::pool::PayoutStatus::PENDING: return "pending";
                        case dinero::pool::PayoutStatus::CONFIRMED: return "confirmed";
                        case dinero::pool::PayoutStatus::PAID: return "paid";
                        case dinero::pool::PayoutStatus::FAILED: return "failed";
                        default: return "unknown";
                    }
                }();
                payout["txid"] = p.txid;
                payout_list.append(payout);
            }
            result["payouts"] = payout_list;

            return result;
        })
        .examples({
            "pool.block 1"
        });

    // ═══════════════════════════════════════════════════════════════════════════
    // PAYOUT OPERATIONS
    // ═══════════════════════════════════════════════════════════════════════════

    RPC_METHOD("pool.payouts", "pool")
        .description("Get pending payouts")
        .param("status", "string", "Filter by status: pending, confirmed, paid, failed (default: all)", false)
        .result("array", "List of payouts")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }
            result = din::arr();

            auto payouts = pool_db->getPendingPayouts();

            for (const auto& p : payouts) {
                din::Json payout = din::obj();
                payout["payout_id"] = static_cast<int64_t>(p.payout_id);
                payout["block_id"] = static_cast<int64_t>(p.block_id);
                payout["worker_id"] = p.worker_id;
                payout["wallet_address"] = p.wallet_address;
                payout["amount"] = unaToString(p.amount);
                payout["amount_una"] = static_cast<int64_t>(p.amount);
                payout["share_percent"] = p.share_percent;
                payout["status"] = [&]() {
                    switch (p.status) {
                        case dinero::pool::PayoutStatus::PENDING: return "pending";
                        case dinero::pool::PayoutStatus::CONFIRMED: return "confirmed";
                        case dinero::pool::PayoutStatus::PAID: return "paid";
                        case dinero::pool::PayoutStatus::FAILED: return "failed";
                        default: return "unknown";
                    }
                }();
                payout["txid"] = p.txid;
                payout["error_message"] = p.error_message;
                payout["calculated_at"] = formatTimestamp(p.calculated_at);
                payout["paid_at"] = formatTimestamp(p.paid_at);

                result.append(payout);
            }

            return result;
        })
        .examples({
            "pool.payouts"
        });

    RPC_METHOD("pool.processpayouts", "pool")
        .description("Process pending payouts (admin only)")
        .params({})
        .result("object", "Number of payouts processed")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();
            std::shared_ptr<dinero::pool::PoolDB> pool_db;
            if (!resolvePoolDb(pool_db, result)) {
                return result;
            }

            auto config = pool_db->getConfig();
            dinero::pool::PayoutCalculator calculator(*pool_db, config);

            // First, process any confirmed blocks that need payout calculation
            uint32_t blocks_processed = calculator.processConfirmedBlocks();

            result["blocks_processed"] = static_cast<int>(blocks_processed);
            result["message"] = "Payouts calculated for " + std::to_string(blocks_processed) + " blocks";

            // Note: Actual payment sending would be done by PayoutProcessor
            // with a wallet integration callback

            return result;
        })
        .examples({
            "pool.processpayouts"
        });

    dinero::g_logger.info("Registered 14 pool accounting RPC methods");
}

} // namespace rpc
} // namespace din
