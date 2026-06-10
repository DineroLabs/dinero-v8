/**
 * Mempool RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates all mempool RPC methods from legacy globals to DaemonContext.
 *
 * OLD PATTERN (legacy):
 *   extern std::unique_ptr<Mempool> g_mempool;
 *   size_t count = dinero::legacy::g_mempool()->size();
 *
 * NEW PATTERN (context-aware):
 *   auto mempool = ctx.daemon->mempool;
 *   size_t count = mempool->size();
 *
 * Benefits:
 * - No dependency on global variables
 * - Testable with mock mempool services
 * - Clear dependency tracking
 * - Thread-safe service access
 */
#include <cmath>

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/chainstate_service.h"  // Phase 38: For ChainDB access
#include "daemon/interfaces/tx_ingress.h"        // Step 5: ITxIngress interface
#include "daemon/interfaces/origin.h"            // Step 5: TxOrigin enum
#include "daemon/mempool.h"
#include "mempool/fee_estimator.h"  // v0.13.0.3: Fee estimation
#include "mempool/mempool.h"         // Phase 38: For MempoolAcceptResult
#include "wallet/transaction.h"      // Phase 38: For Transaction::Deserialize
#include "common/logger.h"
#include "mempool/tx_orphan_pool.h"  // Transaction orphan pool
#include "common/json_adapter.h"
#include "primitives/uint256.h"    // Phase M.0: uint256 type
#include <memory>
#include <sstream>
#include <iomanip>
#include <set>                      // Phase M.1.B: For ancestor/descendant tracking
#include <ctime>                    // Phase 38: For std::time

using dinero::uint256;  // Phase M.0: Make uint256 available without namespace prefix
using dinero::Transaction;  // Phase 38: Make Transaction available
using dinero::TransactionSerializer;  // Phase 38: For deserialization

namespace {
bool SignalsRBF(const Transaction& tx) {
    // BIP125 opt-in signaling: any input with sequence < 0xfffffffe.
    for (const auto& input : tx.vin) {
        if (input.sequence < 0xfffffffeU) {
            return true;
        }
    }
    return false;
}
}  // namespace

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE MEMPOOL RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * mempool.getinfo - Get mempool information
 *
 * OLD: dinero::legacy::g_mempool()->getStats()
 * NEW: ctx.daemon->mempool->mempool().getStats()
 */
din::Json rpc_context_mempool_getinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        auto& mempool = mempool_service->mempool();
        auto stats = mempool.getStats();

        result["size"] = static_cast<int>(stats.tx_count);
        result["bytes"] = static_cast<int>(stats.total_size);
        result["usage"] = static_cast<int>(stats.total_size);
        result["total_fee"] = static_cast<double>(stats.total_fees) / 1e8;  // Convert una to DIN
        result["maxmempool"] = 300000000;  // 300MB default
        result["mempoolminfee"] = 0.00001;  // 1 una/byte default
        result["minrelaytxfee"] = 0.00001;  // 1 una/byte default
        result["unbroadcastcount"] = 0;
        result["stale_tx_count"] = static_cast<int>(stats.stale_tx_count);
        result["last_connected_height"] = static_cast<int>(stats.last_connected_height);
        result["stale_evicted_total"] = static_cast<din::Json::UInt64>(stats.stale_evicted_total);
        result["refresh_attempted_total"] = static_cast<din::Json::UInt64>(stats.refresh_attempted_total);
        result["refresh_succeeded_total"] = static_cast<din::Json::UInt64>(stats.refresh_succeeded_total);
        result["refresh_dropped_budget_total"] = static_cast<din::Json::UInt64>(stats.refresh_dropped_budget_total);

        if (stats.tx_count > 0) {
            result["avg_fee_rate"] = stats.avg_fee_rate;
            result["median_fee_rate"] = stats.median_fee_rate;
            result["min_fee_rate"] = static_cast<int>(stats.min_fee_rate);
            result["max_fee_rate"] = static_cast<int>(stats.max_fee_rate);
            result["oldest_tx_age_seconds"] = static_cast<int>(stats.oldest_tx_age.count());
        }

        result["rpc_schema"] = "din.mempool.v1";

        dinero::g_logger.info("[Mempool RPC] getinfo: " + std::to_string(stats.tx_count) + " transactions");

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get mempool info: ") + e.what();
    }

    return result;
}

/**
 * mempool.getrawmempool - Get all transaction IDs in mempool
 *
 * OLD: dinero::legacy::g_mempool()->getTransactionIds()
 * NEW: ctx.daemon->mempool->mempool().getTransactionIds()
 */
din::Json rpc_context_mempool_getrawmempool(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        bool verbose = false;
        if (!params.empty() && params[0].isBool()) {
            verbose = params[0].asBool();
        }

        auto& mempool = mempool_service->mempool();

        if (verbose) {
            // Return detailed information about each transaction
            din::Json tx_map;
            auto txids = mempool.getTransactionIds();

            for (const auto& txid : txids) {
                auto entry_opt = mempool.getMempoolEntry(txid);
                if (entry_opt) {
                    const auto& entry = entry_opt.value();
                    din::Json tx_info;
                    tx_info["size"] = static_cast<int>(entry.tx_size);
                    tx_info["fee"] = static_cast<double>(entry.fee) / 100000000.0;  // Convert una to DIN
                    tx_info["modifiedfee"] = static_cast<double>(entry.fee) / 100000000.0;

                    // Convert time point to Unix timestamp
                    auto time_since_epoch = entry.time.time_since_epoch();
                    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
                    tx_info["time"] = static_cast<int>(seconds.count());

                    tx_info["height"] = static_cast<int>(entry.height);

                    // Add dependencies
                    din::Json depends_array = din::arr();
                    for (const auto& dep : entry.depends) {
                        depends_array.append(dep.GetHex());  // Phase M.0: RPC boundary conversion
                    }
                    tx_info["depends"] = depends_array;

                    tx_map[txid.GetHex()] = tx_info;  // Phase M.0: RPC boundary conversion
                }
            }

            result = tx_map;
        } else {
            // Return simple array of transaction IDs
            auto txids = mempool.getTransactionIds();
            din::Json tx_array = din::arr();

            for (const auto& txid : txids) {
                tx_array.append(txid.GetHex());  // Phase M.0: RPC boundary conversion
            }

            result = tx_array;
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get raw mempool: ") + e.what();
    }

    return result;
}

/**
 * mempool.gettransaction - Get specific transaction from mempool
 *
 * OLD: dinero::legacy::g_mempool()->getTransaction(txid)
 * NEW: ctx.daemon->mempool->mempool().getTransaction(txid)
 */
din::Json rpc_context_mempool_gettransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: mempool.gettransaction <txid>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        std::string txid_hex = params[0].as<std::string>();
        uint256 txid = uint256::FromHexUnsafe(txid_hex);  // Phase M.0: RPC boundary conversion
        auto& mempool = mempool_service->mempool();

        if (!mempool.hasTransaction(txid)) {
            result["error"] = "Transaction not in mempool";
            return result;
        }

        auto tx_ptr = mempool.getTransaction(txid);
        if (!tx_ptr) {
            result["error"] = "Failed to get transaction";
            return result;
        }

        auto serialized = tx_ptr->Serialize();
        std::string hex_str = dinero::jj::toHex(serialized);

        result["txid"] = txid.GetHex();  // Phase M.0: RPC boundary conversion
        result["hex"] = hex_str;
        result["size"] = static_cast<int>(serialized.size());
        result["vsize"] = static_cast<int>(serialized.size());
        result["weight"] = static_cast<int>(serialized.size() * 4);

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get transaction: ") + e.what();
    }

    return result;
}

/**
 * mempool.getmempoolentry - Get mempool data for a single transaction
 *
 * Phase M.1.B: Returns detailed information about a transaction in the mempool.
 *
 * Params: [txid]
 * Returns: Object with transaction details or error
 */
din::Json rpc_context_mempool_getmempoolentry(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        // Phase M.1.B: RPC boundary - convert hex string to uint256
        if (params.empty() || !params[0].is<std::string>()) {
            result["error"] = "Missing txid parameter";
            return result;
        }

        std::string txid_hex = params[0].as<std::string>();
        uint256 txid = uint256::FromHexUnsafe(txid_hex);

        auto& mempool = mempool_service->mempool();

        if (!mempool.hasTransaction(txid)) {
            result["error"] = "Transaction not in mempool";
            return result;
        }

        auto entry_opt = mempool.getMempoolEntry(txid);
        if (!entry_opt) {
            result["error"] = "Failed to get mempool entry";
            return result;
        }

        const auto& entry = entry_opt.value();

        // Serialize entry fields (Phase M.1.B: Core uses uint256, RPC outputs hex strings)
        result["vsize"] = static_cast<int>(entry.tx_size);
        result["size"] = static_cast<int>(entry.tx_size);
        result["weight"] = static_cast<int>(entry.tx_size * 4);  // Non-segwit weight
        result["effectivevsize"] = static_cast<Json::UInt64>(entry.effective_vsize);
        result["fee"] = static_cast<double>(entry.fee) / 100000000.0;  // Convert una to DIN
        result["modifiedfee"] = static_cast<double>(entry.fee) / 100000000.0;

        // Convert time point to Unix timestamp
        auto time_since_epoch = entry.time.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
        result["time"] = static_cast<int>(seconds.count());

        result["height"] = static_cast<int>(entry.height);

        // Ancestry information
        result["descendantcount"] = 1;  // At minimum, the tx itself
        result["descendantsize"] = static_cast<int>(entry.tx_size);
        result["descendantfees"] = static_cast<double>(entry.fee) / 100000000.0;

        result["ancestorcount"] = static_cast<int>(entry.depends.size() + 1);  // Parents + self
        result["ancestorsize"] = static_cast<int>(entry.ancestor_size > 0 ? entry.ancestor_size : entry.tx_size);
        result["ancestorfees"] = static_cast<double>(entry.ancestor_fee > 0 ? entry.ancestor_fee : entry.fee) / 100000000.0;

        // Fee rates
        result["fees"] = din::Json(Json::objectValue);
        result["fees"]["base"] = static_cast<double>(entry.fee) / 100000000.0;
        result["fees"]["modified"] = static_cast<double>(entry.fee) / 100000000.0;
        result["fees"]["ancestor"] = entry.ancestor_feerate;
        result["fees"]["descendant"] = entry.fee_rate;
        result["fees"]["effective"] = entry.adjusted_fee_rate;
        result["fees"]["ancestor_effective"] = entry.ancestor_adjusted_feerate;

        result["privacy"] = din::Json(Json::objectValue);
        result["privacy"]["has_confidential_outputs"] = entry.is_confidential;
        result["privacy"]["proof_bytes"] = static_cast<Json::UInt64>(entry.total_proof_bytes);

        // Dependencies (Phase M.1.B: Convert uint256 to hex at RPC boundary)
        din::Json depends_array = din::arr();
        for (const auto& dep : entry.depends) {
            depends_array.append(dep.GetHex());
        }
        result["depends"] = depends_array;

        // Spentby - need to scan mempool for descendants
        din::Json spentby_array = din::arr();
        auto all_txids = mempool.getTransactionIds();
        for (const auto& other_txid : all_txids) {
            if (other_txid == txid) continue;  // Skip self
            auto other_entry_opt = mempool.getMempoolEntry(other_txid);
            if (other_entry_opt) {
                const auto& other_entry = other_entry_opt.value();
                // Check if other_entry depends on this txid
                for (const auto& dep : other_entry.depends) {
                    if (dep == txid) {
                        spentby_array.append(other_txid.GetHex());
                        break;
                    }
                }
            }
        }
        result["spentby"] = spentby_array;

        // BIP125 replaceability derived from transaction sequence signaling.
        result["bip125-replaceable"] = SignalsRBF(entry.tx);

        dinero::g_logger.debug("[Mempool RPC] getmempoolentry: " + txid_hex);

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get mempool entry: ") + e.what();
    }

    return result;
}

/**
 * mempool.getmempoolancestors - Get all in-mempool ancestors of a transaction
 *
 * Phase M.1.B: Returns all ancestor transactions (recursive dependencies).
 *
 * Params: [txid, verbose=false]
 * Returns: Array of txids (if verbose=false) or object with full details (if verbose=true)
 */
din::Json rpc_context_mempool_getmempoolancestors(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        // Phase M.1.B: RPC boundary - convert hex string to uint256
        if (params.empty() || !params[0].is<std::string>()) {
            result["error"] = "Missing txid parameter";
            return result;
        }

        std::string txid_hex = params[0].as<std::string>();
        uint256 txid = uint256::FromHexUnsafe(txid_hex);

        bool verbose = false;
        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }

        auto& mempool = mempool_service->mempool();

        if (!mempool.hasTransaction(txid)) {
            result["error"] = "Transaction not in mempool";
            return result;
        }

        // Recursively collect all ancestors (Phase M.1.B: Core uses uint256)
        std::set<uint256> ancestors;
        std::vector<uint256> to_process;
        to_process.push_back(txid);

        while (!to_process.empty()) {
            uint256 current = to_process.back();
            to_process.pop_back();

            auto entry_opt = mempool.getMempoolEntry(current);
            if (!entry_opt) continue;

            const auto& entry = entry_opt.value();
            for (const auto& parent_txid : entry.depends) {
                // Only add if not already visited and exists in mempool
                if (ancestors.find(parent_txid) == ancestors.end() && mempool.hasTransaction(parent_txid)) {
                    ancestors.insert(parent_txid);
                    to_process.push_back(parent_txid);
                }
            }
        }

        // Phase M.1.B: RPC boundary - convert uint256 to hex at output
        if (verbose) {
            din::Json verbose_result = din::Json(Json::objectValue);
            for (const auto& ancestor_txid : ancestors) {
                auto entry_opt = mempool.getMempoolEntry(ancestor_txid);
                if (entry_opt) {
                    const auto& entry = entry_opt.value();
                    din::Json tx_info;
                    tx_info["size"] = static_cast<int>(entry.tx_size);
                    tx_info["fee"] = static_cast<double>(entry.fee) / 100000000.0;
                    tx_info["modifiedfee"] = static_cast<double>(entry.fee) / 100000000.0;

                    auto time_since_epoch = entry.time.time_since_epoch();
                    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
                    tx_info["time"] = static_cast<int>(seconds.count());

                    tx_info["height"] = static_cast<int>(entry.height);

                    din::Json depends_array = din::arr();
                    for (const auto& dep : entry.depends) {
                        depends_array.append(dep.GetHex());
                    }
                    tx_info["depends"] = depends_array;

                    verbose_result[ancestor_txid.GetHex()] = tx_info;
                }
            }
            result = verbose_result;
        } else {
            din::Json txid_array = din::arr();
            for (const auto& ancestor_txid : ancestors) {
                txid_array.append(ancestor_txid.GetHex());
            }
            result = txid_array;
        }

        dinero::g_logger.debug("[Mempool RPC] getmempoolancestors: " + txid_hex +
                               " (found " + std::to_string(ancestors.size()) + " ancestors)");

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get ancestors: ") + e.what();
    }

    return result;
}

/**
 * mempool.getmempooldescendants - Get all in-mempool descendants of a transaction
 *
 * Phase M.1.B: Returns all descendant transactions (recursive dependents).
 *
 * Params: [txid, verbose=false]
 * Returns: Array of txids (if verbose=false) or object with full details (if verbose=true)
 */
din::Json rpc_context_mempool_getmempooldescendants(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        // Phase M.1.B: RPC boundary - convert hex string to uint256
        if (params.empty() || !params[0].is<std::string>()) {
            result["error"] = "Missing txid parameter";
            return result;
        }

        std::string txid_hex = params[0].as<std::string>();
        uint256 txid = uint256::FromHexUnsafe(txid_hex);

        bool verbose = false;
        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }

        auto& mempool = mempool_service->mempool();

        if (!mempool.hasTransaction(txid)) {
            result["error"] = "Transaction not in mempool";
            return result;
        }

        // Recursively collect all descendants (Phase M.1.B: Core uses uint256)
        std::set<uint256> descendants;
        std::vector<uint256> to_process;
        to_process.push_back(txid);

        while (!to_process.empty()) {
            uint256 current = to_process.back();
            to_process.pop_back();

            // Find all transactions that depend on current
            auto all_txids = mempool.getTransactionIds();
            for (const auto& candidate_txid : all_txids) {
                if (descendants.find(candidate_txid) != descendants.end()) {
                    continue;  // Already found
                }

                auto entry_opt = mempool.getMempoolEntry(candidate_txid);
                if (!entry_opt) continue;

                const auto& entry = entry_opt.value();
                // Check if this transaction depends on current
                for (const auto& dep : entry.depends) {
                    if (dep == current) {
                        // This is a descendant
                        descendants.insert(candidate_txid);
                        to_process.push_back(candidate_txid);
                        break;
                    }
                }
            }
        }

        // Phase M.1.B: RPC boundary - convert uint256 to hex at output
        if (verbose) {
            din::Json verbose_result = din::Json(Json::objectValue);
            for (const auto& descendant_txid : descendants) {
                auto entry_opt = mempool.getMempoolEntry(descendant_txid);
                if (entry_opt) {
                    const auto& entry = entry_opt.value();
                    din::Json tx_info;
                    tx_info["size"] = static_cast<int>(entry.tx_size);
                    tx_info["fee"] = static_cast<double>(entry.fee) / 100000000.0;
                    tx_info["modifiedfee"] = static_cast<double>(entry.fee) / 100000000.0;

                    auto time_since_epoch = entry.time.time_since_epoch();
                    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
                    tx_info["time"] = static_cast<int>(seconds.count());

                    tx_info["height"] = static_cast<int>(entry.height);

                    din::Json depends_array = din::arr();
                    for (const auto& dep : entry.depends) {
                        depends_array.append(dep.GetHex());
                    }
                    tx_info["depends"] = depends_array;

                    verbose_result[descendant_txid.GetHex()] = tx_info;
                }
            }
            result = verbose_result;
        } else {
            din::Json txid_array = din::arr();
            for (const auto& descendant_txid : descendants) {
                txid_array.append(descendant_txid.GetHex());
            }
            result = txid_array;
        }

        dinero::g_logger.debug("[Mempool RPC] getmempooldescendants: " + txid_hex +
                               " (found " + std::to_string(descendants.size()) + " descendants)");

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get descendants: ") + e.what();
    }

    return result;
}

/**
 * mempool.stats - Get comprehensive mempool statistics
 *
 * NEW method - provides detailed stats about mempool state
 */
din::Json rpc_context_mempool_stats(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        auto& mempool = mempool_service->mempool();
        auto stats = mempool.getStats();

        result["tx_count"] = static_cast<int>(stats.tx_count);
        result["total_size_bytes"] = static_cast<int>(stats.total_size);
        result["total_fees_din"] = static_cast<double>(stats.total_fees) / 1e8;
        result["avg_fee_rate_una_byte"] = stats.avg_fee_rate;
        result["min_fee_rate"] = static_cast<int>(stats.min_fee_rate);
        result["max_fee_rate"] = static_cast<int>(stats.max_fee_rate);
        result["oldest_tx_age_seconds"] = static_cast<int>(stats.oldest_tx_age.count());
        result["stale_tx_count"] = static_cast<int>(stats.stale_tx_count);
        result["last_connected_height"] = static_cast<int>(stats.last_connected_height);
        result["stale_evicted_total"] = static_cast<din::Json::UInt64>(stats.stale_evicted_total);
        result["refresh_attempted_total"] = static_cast<din::Json::UInt64>(stats.refresh_attempted_total);
        result["refresh_succeeded_total"] = static_cast<din::Json::UInt64>(stats.refresh_succeeded_total);
        result["refresh_dropped_budget_total"] = static_cast<din::Json::UInt64>(stats.refresh_dropped_budget_total);
        result["rpc_schema"] = "din.mempool.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get mempool stats: ") + e.what();
    }

    return result;
}

/**
 * mempool.clear - Clear all transactions from mempool
 *
 * WARNING: This is a dangerous operation - use only for testing!
 */
din::Json rpc_context_mempool_clear(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        auto& mempool = mempool_service->mempool();
        size_t count = mempool.size();
        mempool.clear();

        result["success"] = true;
        result["transactions_removed"] = static_cast<int>(count);
        result["warning"] = "Mempool cleared - all unconfirmed transactions removed";

        dinero::g_logger.warning("[Mempool RPC] Mempool cleared: " + std::to_string(count) + " transactions removed");

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to clear mempool: ") + e.what();
    }

    return result;
}

/**
 * mempool.getbyfee - Get transactions sorted by fee rate
 *
 * NEW method - useful for block template creation and fee analysis
 */
din::Json rpc_context_mempool_getbyfee(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        size_t max_count = 100;
        if (!params.empty() && params[0].is<int>()) {
            max_count = params[0].as<int>();
        }

        auto& mempool = mempool_service->mempool();
        auto txs = mempool.getTransactionsByFeeRate(max_count);

        din::Json tx_array = din::arr();
        for (const auto& tx : txs) {
            // Phase M.0: Keep identity as uint256, convert to hex only for output
            uint256 txid = tx.GetTxid().AsUint256();  // Phase M.4: Unwrap TxId
            din::Json tx_obj;
            tx_obj["txid"] = txid.GetHex();  // Convert to hex at RPC boundary
            tx_obj["size"] = static_cast<int>(tx.Serialize().size() / 2);

            // Add fee information from mempool entry
            auto fee_opt = mempool.getTransactionFee(txid);
            auto fee_rate_opt = mempool.getTransactionFeeRate(txid);

            if (fee_opt) {
                tx_obj["fee"] = static_cast<double>(fee_opt.value()) / 100000000.0;  // Convert to DIN
            }
            if (fee_rate_opt) {
                tx_obj["feerate"] = fee_rate_opt.value();  // una/byte
            }

            tx_array.append(tx_obj);
        }

        result["transactions"] = tx_array;
        result["count"] = static_cast<int>(txs.size());

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get transactions by fee: ") + e.what();
    }

    return result;
}

/**
 * mempool.estimatefee - Estimate fee for target confirmation
 *
 * Week 7: Improved fee estimation based on mempool state
 */
din::Json rpc_context_mempool_estimatefee(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    int target_blocks = 6;  // Default
    if (!params.empty() && params[0].is<int>()) {
        target_blocks = params[0].as<int>();
    }

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    try {
        auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
        if (!mempool_service) {
            result["error"] = "Failed to cast mempool service";
            return result;
        }

        auto& mempool = mempool_service->mempool();
        auto stats = mempool.getStats();

        // Week 7: Calculate fee rate based on mempool statistics
        double feerate_una_vb = 1.0;  // Default minimum fee (1 una/vbyte)
        double confidence = 0.5;     // Low confidence for simple estimator

        if (stats.tx_count > 0 && stats.avg_fee_rate > 0) {
            // Use average fee rate from mempool as base estimate
            feerate_una_vb = stats.avg_fee_rate;
            confidence = std::min(0.9, 0.5 + (stats.tx_count / 100.0));  // Higher confidence with more txs

            // Adjust for target blocks (higher fee for faster confirmation)
            if (target_blocks <= 1) {
                feerate_una_vb = stats.max_fee_rate;  // Use max fee for immediate confirmation
                confidence = 0.95;
            } else if (target_blocks <= 3) {
                feerate_una_vb = std::max(feerate_una_vb, stats.avg_fee_rate * 1.5);
            }
        }

        result["feerate"] = feerate_una_vb / 100000.0;  // Convert una/vbyte to DIN/kB
        result["feerate_una_vb"] = feerate_una_vb;
        result["blocks"] = target_blocks;
        result["confidence"] = confidence;
        result["mempool_size"] = static_cast<int>(stats.tx_count);
        result["note"] = "Fee estimation based on mempool statistics";
        result["rpc_schema"] = "din.mempool.v1";

        dinero::g_logger.debug("[Mempool RPC] estimatefee: " + std::to_string(feerate_una_vb) +
                              " una/vbyte for " + std::to_string(target_blocks) + " blocks");

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to estimate fee: ") + e.what();
    }

    return result;
}

/**
 * mempool.estimatesmartfee - Smart fee estimation (Bitcoin Core compatible)
 *
 * Returns fee estimate with confidence and reasoning.
 * Uses advanced fee estimation with EWMA buckets when available.
 *
 * Params:
 *   [0] conf_target: int - Confirmation target in blocks (1-1008)
 *   [1] estimate_mode: string - "ECONOMICAL" or "CONSERVATIVE" (optional)
 *
 * Returns: {feerate, blocks, errors[]}
 */
din::Json rpc_context_mempool_estimatesmartfee(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Parse confirmation target
    int conf_target = 6;  // Default
    if (!params.empty() && params[0].is<int>()) {
        conf_target = params[0].as<int>();
        if (conf_target < 1) conf_target = 1;
        if (conf_target > 1008) conf_target = 1008;
    }

    // Parse estimate mode
    std::string estimate_mode = "CONSERVATIVE";
    if (params.size() >= 2 && params[1].is<std::string>()) {
        estimate_mode = params[1].as<std::string>();
    }
    bool conservative = (estimate_mode != "ECONOMICAL");

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["errors"] = din::arr();
        result["errors"].append("Mempool service not available");
        return result;
    }

    try {
        auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
        if (!mempool_service) {
            result["errors"] = din::arr();
            result["errors"].append("Failed to cast mempool service");
            return result;
        }

        // v0.13.0.3: Use new FeeEstimator
        auto& mempool = mempool_service->mempool();
        auto& fee_estimator = mempool.getFeeEstimator();

        double feerate_din_kb = 0.0;
        int actual_blocks = conf_target;
        din::Json errors = din::arr();

        // Try to get estimate from FeeEstimator
        auto estimate = fee_estimator.estimateFee(static_cast<uint32_t>(conf_target));

        if (estimate.has_value()) {
            // Got estimate from historical data
            double fee_rate_una_byte = estimate.value();

            // Apply conservative multiplier if needed
            if (conservative) {
                fee_rate_una_byte *= 1.1;  // 10% buffer
            }

            // Convert una/byte to DIN/kB
            // 1 una/byte = 1000 una/kB = 0.00001 DIN/kB (at 1e8 una = 1 DIN)
            feerate_din_kb = fee_rate_una_byte * 1000.0 / 1e8;
        } else {
            errors.append("Insufficient data for reliable estimate");
            // Fall back to mempool-based estimate
        }

        // Fallback: use mempool statistics
        if (feerate_din_kb <= 0.0) {
            auto stats = mempool.getStats();

            uint64_t fee_rate_una = 1000;  // Minimum 1 una/byte = 1000 una/kB

            if (stats.tx_count > 0 && stats.avg_fee_rate > 0) {
                // Use percentile-based estimation
                if (conf_target <= 1) {
                    fee_rate_una = static_cast<uint64_t>(stats.max_fee_rate * 1000);  // Use max for next block
                } else if (conf_target <= 3) {
                    fee_rate_una = static_cast<uint64_t>(stats.avg_fee_rate * 1.5 * 1000);
                } else if (conf_target <= 6) {
                    fee_rate_una = static_cast<uint64_t>(stats.avg_fee_rate * 1000);
                } else {
                    fee_rate_una = static_cast<uint64_t>(stats.avg_fee_rate * 0.75 * 1000);
                }

                if (conservative) {
                    fee_rate_una = static_cast<uint64_t>(fee_rate_una * 1.1);
                }
            }

            feerate_din_kb = static_cast<double>(fee_rate_una) / 1e8;
        }

        // Build result (Bitcoin Core compatible format)
        result["feerate"] = feerate_din_kb;
        result["blocks"] = actual_blocks;

        if (errors.size() > 0) {
            result["errors"] = errors;
        }

        // Extended info
        result["conf_target"] = conf_target;
        result["estimate_mode"] = estimate_mode;

        dinero::g_logger.debug("[Mempool RPC] estimatesmartfee: " +
            std::to_string(feerate_din_kb) + " DIN/kB for " +
            std::to_string(conf_target) + " blocks (" + estimate_mode + ")");

    } catch (const std::exception& e) {
        result["errors"] = din::arr();
        result["errors"].append(std::string("Fee estimation failed: ") + e.what());
    }

    return result;
}

/**
 * mempool.getfeehistogram - Get fee rate histogram of mempool
 *
 * Returns distribution of fee rates in the mempool for visualization.
 */
din::Json rpc_context_mempool_getfeehistogram(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::arr();

    if (!ctx.daemon || !ctx.daemon->mempool) {
        din::Json error;
        error["error"] = "Mempool service not available";
        return error;
    }

    try {
        auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
        if (!mempool_service) {
            din::Json error;
            error["error"] = "Failed to cast mempool service";
            return error;
        }

        auto& mempool = mempool_service->mempool();

        // Get fee histogram from mempool
        // Format: [[fee_rate, cumulative_vsize], ...]
        // Each entry represents: "at this fee rate, this many vbytes are waiting"

        // Fee rate buckets (una/vB)
        std::vector<uint64_t> buckets = {1, 2, 3, 5, 10, 20, 50, 100, 200, 500, 1000};
        std::map<uint64_t, uint64_t> bucket_sizes;  // fee_rate -> total vsize

        for (auto& bucket : buckets) {
            bucket_sizes[bucket] = 0;
        }

        // Iterate mempool transactions (simplified - real impl would use mempool iterator)
        auto stats = mempool.getStats();

        // For now, estimate distribution based on avg fee rate
        uint64_t total_size = stats.total_size;
        double avg_fee = stats.avg_fee_rate;

        // Simple model: normal distribution around average
        for (auto& bucket : buckets) {
            double bucket_fee = static_cast<double>(bucket);
            double distance = std::abs(bucket_fee - avg_fee) / std::max(avg_fee, 1.0);
            double proportion = std::exp(-distance * 2.0);  // Exponential decay
            bucket_sizes[bucket] = static_cast<uint64_t>(total_size * proportion * 0.2);
        }

        // Build result array
        for (auto it = buckets.rbegin(); it != buckets.rend(); ++it) {
            din::Json entry = din::arr();
            entry.append(static_cast<double>(*it));
            entry.append(static_cast<double>(bucket_sizes[*it]));
            result.append(entry);
        }

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("Failed to get fee histogram: ") + e.what();
        return error;
    }

    return result;
}

/**
 * mempool.sendrawtransaction - Submit raw transaction to mempool
 *
 * Phase 38: Real implementation using Mempool::submitTransaction
 * This is the REAL mempool insertion - validates and adds to mempool
 */
din::Json rpc_context_mempool_sendrawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    // spec Fatal §3: broadcasting depends on mempool/chainstate trust.
    // Gate before any chainstate or mempool work.
    if (ctx.daemon && ctx.daemon->chainstate) {
        auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (cs && cs->IsInSafeMode()) {
            result["error"] = "disabled while node is in safe mode: " + cs->GetSafeModeReason();
            result["safe_mode"] = true;
            return result;
        }
    }

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: mempool.sendrawtransaction \"rawtx_hex\" [maxfeerate]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    // Phase 39: ChainDB access via ChainstateService (ChainManager deleted)
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate || !chainstate->GetChainDB()) {
        result["error"] = "Chain database not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    try {
        std::string raw_tx_hex = params[0].as<std::string>();

        // Step 1: Deserialize transaction
        Transaction tx;
        if (!TransactionSerializer::Deserialize(tx, raw_tx_hex)) {
            result["error"] = "TX decode failed";
            return result;
        }

        uint256 txid = tx.GetTxid().AsUint256();  // Phase M.4: Unwrap TxId

        // Step 2: Submit to mempool using canonical ingress
        // Step 5: Prefer ITxIngress interface, fall back to direct mempool
        dinero::TxAcceptResult submit_result;

        if (ctx.daemon->tx_ingress) {
            // Check if already in mempool via interface
            if (ctx.daemon->tx_ingress->HasTransaction(txid)) {
                result["error"] = din::Json(Json::objectValue);
                result["error"]["code"] = -27;  // RPC_VERIFY_ALREADY_IN_CHAIN
                result["error"]["message"] = "Transaction already in mempool";
                return result;
            }

            // Submit via canonical interface (TxOrigin::RPC auto-relays)
            submit_result = ctx.daemon->tx_ingress->Submit(tx, dinero::TxOrigin::RPC);
        } else {
            // Fallback to direct mempool (legacy path)
            auto& mempool = mempool_service->mempool();
            submit_result = mempool.submitTransaction(tx, "rpc:sendrawtransaction", true);  // relay=true
        }

        // Step 3: Handle result with structured rejection info
        if (submit_result.accepted()) {
            // Success - return txid
            result["result"] = txid.GetHex();
            dinero::g_logger.info("[mempool.sendrawtransaction] Accepted tx: " + txid.GetHex());
            return result;
        } else {
            // Rejection - return JSON-RPC error with structured info
            // Map TxRejectCode to Bitcoin Core RPC error codes
            int error_code = -26;  // RPC_VERIFY_REJECTED (default policy failure)
            switch (submit_result.code) {
                case dinero::TxRejectCode::ALREADY_IN_MEMPOOL:
                case dinero::TxRejectCode::ALREADY_IN_CHAIN:
                    error_code = -27;  // RPC_VERIFY_ALREADY_IN_CHAIN
                    break;
                case dinero::TxRejectCode::INVALID_TX:
                case dinero::TxRejectCode::SCRIPT_VERIFY_FAILED:
                    error_code = -25;  // RPC_VERIFY_ERROR
                    break;
                case dinero::TxRejectCode::INSUFFICIENT_FEE:
                case dinero::TxRejectCode::MEMPOOL_FULL:
                    error_code = -26;  // RPC_VERIFY_REJECTED
                    break;
                default:
                    error_code = -26;  // Default policy rejection
                    break;
            }

            result["error"] = din::Json(Json::objectValue);
            result["error"]["code"] = error_code;
            result["error"]["message"] = std::string(dinero::TxRejectCodeToString(submit_result.code)) +
                                        ": " + submit_result.message;
            dinero::g_logger.warning("[mempool.sendrawtransaction] Rejected tx: " + txid.GetHex() +
                                    " - " + submit_result.message);
            return result;
        }

    } catch (const std::exception& e) {
        result["error"] = din::Json(Json::objectValue);
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("exception: ") + e.what();
        return result;
    }
}

/**
 * mempool.testmempoolaccept - Test whether transaction would be accepted
 *
 * Tests whether raw transactions would be accepted by the mempool without actually adding them.
 * Returns detailed rejection reasons if validation fails.
 *
 * Phase 38: Real implementation using Mempool::acceptTransaction with TEST_ONLY mode
 */
din::Json rpc_context_mempool_testmempoolaccept(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::arr();

    if (params.empty() || !params[0].is<std::string>()) {
        din::Json error;
        error["error"] = "Usage: mempool.testmempoolaccept \"rawtx_hex\"";
        return error;
    }

    if (!ctx.daemon || !ctx.daemon->mempool) {
        din::Json error;
        error["error"] = "Mempool service not available";
        return error;
    }

    // Phase 39: ChainDB access via ChainstateService (ChainManager deleted)
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate || !chainstate->GetChainDB()) {
        din::Json error;
        error["error"] = "Chain database not available";
        return error;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        din::Json error;
        error["error"] = "Failed to cast mempool service";
        return error;
    }

    try {
        std::string raw_tx_hex = params[0].as<std::string>();

        // Step 1: Deserialize transaction
        Transaction tx;
        if (!TransactionSerializer::Deserialize(tx, raw_tx_hex)) {
            din::Json entry;
            entry["txid"] = "";
            entry["allowed"] = false;
            entry["reject-reason"] = "transaction decode failed";
            result.append(entry);
            return result;
        }

        uint256 txid = tx.GetTxid().AsUint256();  // Phase M.4: Unwrap TxId

        // Step 2: Test mempool acceptance (TEST_ONLY mode - no actual insertion)
        auto& mempool = mempool_service->mempool();

        // Phase G.3: Use structured submitTransactionTestOnly API
        auto test_result = mempool.submitTransactionTestOnly(tx, "rpc:testmempoolaccept");

        // Step 3: Format Bitcoin-compatible response
        din::Json entry;
        entry["txid"] = txid.GetHex();

        if (test_result.accepted()) {
            entry["allowed"] = true;
        } else {
            entry["allowed"] = false;
            entry["reject-reason"] = std::string(dinero::TxRejectCodeToString(test_result.code)) +
                                    ": " + test_result.message;
        }

        result.append(entry);
        return result;

    } catch (const std::exception& e) {
        din::Json entry;
        entry["txid"] = "";
        entry["allowed"] = false;
        entry["reject-reason"] = std::string("exception: ") + e.what();
        result.append(entry);
        return result;
    }
}

/**
 * mempool.getorphans - Report orphan pool status and contents
 */
din::Json rpc_context_mempool_getorphans(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;

    if (!ctx.daemon) {
        result["error"] = "Daemon context not available";
        return result;
    }

    auto* pool = ctx.daemon->orphan_pool;
    if (!pool) {
        result["orphans"] = din::arr();
        result["count"] = 0;
        result["tracking_enabled"] = false;
        result["note"] = "Orphan pool not initialized";
        return result;
    }

    result["tracking_enabled"] = true;
    result["count"] = static_cast<int>(pool->size());

    // List orphan transaction IDs
    auto txids = pool->getOrphanTxIds();
    din::Json orphan_list = din::arr();
    for (const auto& txid : txids) {
        orphan_list.append(txid.GetHex());
    }
    result["orphans"] = orphan_list;

    // Per-peer breakdown
    auto peer_counts = pool->getPeerOrphanCounts();
    din::Json peers = din::obj();
    for (const auto& [peer_id, count] : peer_counts) {
        peers[peer_id] = static_cast<int>(count);
    }
    result["per_peer"] = peers;

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

extern RpcRegistry g_rpcRegistry;

void registerMempoolMethodsContext() {
    // Core mempool info methods (fully implemented)
    g_rpcRegistry.registerHandler("mempool.getinfo",
                                 rpc_context_mempool_getinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Register legacy aliases for backward compatibility
    g_rpcRegistry.registerAlias("getmempoolinfo", "mempool.getinfo");

    g_rpcRegistry.registerHandler("mempool.getrawmempool",
                                 rpc_context_mempool_getrawmempool,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getrawmempool", "mempool.getrawmempool");

    g_rpcRegistry.registerHandler("mempool.gettransaction",
                                 rpc_context_mempool_gettransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Phase M.1.B: Mempool entry and ancestry RPCs
    g_rpcRegistry.registerHandler("mempool.getmempoolentry",
                                 rpc_context_mempool_getmempoolentry,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getmempoolentry", "mempool.getmempoolentry");

    g_rpcRegistry.registerHandler("mempool.getmempoolancestors",
                                 rpc_context_mempool_getmempoolancestors,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getmempoolancestors", "mempool.getmempoolancestors");

    g_rpcRegistry.registerHandler("mempool.getmempooldescendants",
                                 rpc_context_mempool_getmempooldescendants,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getmempooldescendants", "mempool.getmempooldescendants");

    g_rpcRegistry.registerHandler("mempool.stats",
                                 rpc_context_mempool_stats,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Mempool management methods
    g_rpcRegistry.registerHandler("mempool.clear",
                                 rpc_context_mempool_clear,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mempool.getbyfee",
                                 rpc_context_mempool_getbyfee,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Fee estimation
    g_rpcRegistry.registerHandler("mempool.estimatefee",
                                 rpc_context_mempool_estimatefee,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mempool.estimatesmartfee",
                                 rpc_context_mempool_estimatesmartfee,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Also register Bitcoin Core compatible aliases
    g_rpcRegistry.registerHandler("estimatesmartfee",
                                 rpc_context_mempool_estimatesmartfee,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mempool.getfeehistogram",
                                 rpc_context_mempool_getfeehistogram,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Transaction submission and policy probes
    g_rpcRegistry.registerHandler("mempool.sendrawtransaction",
                                 rpc_context_mempool_sendrawtransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("sendrawtransaction", "mempool.sendrawtransaction");

    g_rpcRegistry.registerHandler("mempool.testmempoolaccept",
                                 rpc_context_mempool_testmempoolaccept,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("testmempoolaccept", "mempool.testmempoolaccept");

    g_rpcRegistry.registerHandler("mempool.getorphans",
                                 rpc_context_mempool_getorphans,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] ✅ 13 mempool context-aware handlers registered (Phase M.1.B: +3)");
}
