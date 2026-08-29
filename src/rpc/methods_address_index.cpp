/**
 * Address-Indexed Query RPCs & Protocol-Level Checkpoints
 *
 * Implements address-based queries for light wallets (DineroDPI).
 * These RPCs query by arbitrary address, not wallet-owned addresses,
 * enabling phones with different mnemonics to query seed node data.
 *
 *   - blockchain.getaddressbalance: Confirmed balance + explicit mempool delta
 *   - blockchain.getaddressmempool: Pending mempool transactions
 *   - blockchain.getaddresshistory: Confirmed transaction history
 *   - blockchain.getcheckpoints:    Full 128-byte headers every 1000 blocks
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "storage/chain_db.h"
#include "daemon/bech32_decode.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "common/logger.h"
#include "wallet/recipient_descriptor.h"
#include "primitives/block.h"
#include "rpc/methods_utreexo.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace din {
using ExecutionContext = ::ExecutionContext;
using dinero::uint256;

namespace {
using BatchClock = std::chrono::steady_clock;

constexpr size_t MAX_BATCH_ADDRESSES = 100;
constexpr int MAX_BATCH_HISTORY = 50;
constexpr auto BATCH_SCAN_TIMEOUT = std::chrono::seconds(40);
constexpr auto BATCH_WAIT_TIMEOUT = std::chrono::seconds(45);
constexpr auto BATCH_SCAN_COOLDOWN = std::chrono::seconds(2);
constexpr auto BATCH_CACHE_TTL = std::chrono::seconds(2);
constexpr size_t MAX_CONCURRENT_BATCH_SCANS = 2;
constexpr size_t MAX_BATCH_CACHE_ENTRIES = 64;

struct BatchCacheEntry {
    Json result;
    BatchClock::time_point stored_at{};
};

struct BatchFlight {
    std::condition_variable changed;
    bool running = true;
    Json result;
};

struct BatchDiscoveryState {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<BatchFlight>> flights;
    std::unordered_map<std::string, BatchCacheEntry> cache;
    std::unordered_map<std::string, BatchClock::time_point> last_scan_started;
    size_t active_scans = 0;
    uint64_t requests = 0;
    uint64_t cache_hits = 0;
    uint64_t waiters = 0;
    uint64_t scans = 0;
    uint64_t rejected = 0;
    uint64_t failures = 0;
    uint64_t total_scan_ms = 0;
};

BatchDiscoveryState g_batch_discovery;

void addBatchMetadata(Json& result,
                      bool cache_hit,
                      uint64_t scan_ms,
                      size_t address_count,
                      const BatchDiscoveryState& state) {
    Json meta;
    meta["cache_hit"] = cache_hit;
    meta["scan_ms"] = static_cast<::Json::Value::UInt64>(scan_ms);
    meta["address_count"] = static_cast<::Json::Value::UInt64>(address_count);
    meta["requests"] = static_cast<::Json::Value::UInt64>(state.requests);
    meta["cache_hits"] = static_cast<::Json::Value::UInt64>(state.cache_hits);
    meta["singleflight_waiters"] = static_cast<::Json::Value::UInt64>(state.waiters);
    meta["scans"] = static_cast<::Json::Value::UInt64>(state.scans);
    meta["rejected"] = static_cast<::Json::Value::UInt64>(state.rejected);
    meta["failures"] = static_cast<::Json::Value::UInt64>(state.failures);
    meta["total_scan_ms"] = static_cast<::Json::Value::UInt64>(state.total_scan_ms);
    meta["active_scans"] = static_cast<::Json::Value::UInt64>(state.active_scans);
    meta["max_concurrent_scans"] =
        static_cast<::Json::Value::UInt64>(MAX_CONCURRENT_BATCH_SCANS);
    result["batch_meta"] = meta;
}

void pruneBatchState(BatchDiscoveryState& state, BatchClock::time_point now) {
    for (auto it = state.cache.begin(); it != state.cache.end();) {
        if (now - it->second.stored_at > BATCH_CACHE_TTL) it = state.cache.erase(it);
        else ++it;
    }
    for (auto it = state.last_scan_started.begin(); it != state.last_scan_started.end();) {
        if (now - it->second >= BATCH_SCAN_COOLDOWN) it = state.last_scan_started.erase(it);
        else ++it;
    }
    while (state.cache.size() > MAX_BATCH_CACHE_ENTRIES) state.cache.erase(state.cache.begin());
}
} // namespace

// ─── Helpers ────────────────────────────────────────────────────────────

static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t b : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

static std::string formatDIN(uint64_t una) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8) << static_cast<double>(una) / 100000000.0;
    return oss.str();
}

static uint64_t absUna(int64_t una) {
    if (una >= 0) {
        return static_cast<uint64_t>(una);
    }
    return static_cast<uint64_t>(-(una + 1)) + 1;
}

static uint64_t applySignedDelta(uint64_t base, int64_t delta) {
    if (delta >= 0) {
        return base + static_cast<uint64_t>(delta);
    }

    const uint64_t delta_abs = absUna(delta);
    if (delta_abs >= base) {
        return 0;
    }
    return base - delta_abs;
}

static std::string formatSignedDIN(int64_t una) {
    std::ostringstream oss;
    if (una < 0) {
        oss << "-";
    }
    oss << std::fixed << std::setprecision(8)
        << static_cast<double>(absUna(una)) / 100000000.0;
    return oss.str();
}

static std::string classifyPrivacyFlow(bool has_conf_inputs, bool has_conf_outputs) {
    if (has_conf_inputs && has_conf_outputs) {
        return "ct_transfer";
    }
    if (has_conf_inputs) {
        return "unshield";
    }
    if (has_conf_outputs) {
        return "shield";
    }
    return "transparent";
}

static std::string classifyAddressPrivacyFlow(bool has_input,
                                              bool has_output,
                                              bool matched_conf_input,
                                              bool matched_conf_output,
                                              bool tx_has_conf_inputs,
                                              bool tx_has_conf_outputs) {
    if (has_output) {
        if (matched_conf_output) {
            return tx_has_conf_inputs ? "ct_transfer" : "shield";
        }
        return tx_has_conf_inputs ? "unshield" : "transparent";
    }
    if (has_input) {
        if (matched_conf_input) {
            return tx_has_conf_outputs ? "ct_transfer" : "unshield";
        }
        return tx_has_conf_outputs ? "shield" : "transparent";
    }
    return classifyPrivacyFlow(tx_has_conf_inputs, tx_has_conf_outputs);
}

static void setHistoryAmountFields(Json& entry, bool amount_hidden, uint64_t amount_una) {
    entry["amount_hidden"] = amount_hidden;
    entry["display_amount"] = amount_hidden ? "confidential" : formatDIN(amount_una);
    if (!amount_hidden) {
        entry["amount"] = static_cast<::Json::Value::Int64>(amount_una);
        entry["amount_din"] = formatDIN(amount_una);
    }
}

/**
 * Decode bech32m address to scriptPubKey hex string.
 * Reuses the same pattern as scantxoutset (methods_utxoset.cpp:220-242).
 * On failure, populates error JSON and returns false.
 */
static bool decodeAddressToScriptHex(const std::string& address,
                                      std::string& script_hex,
                                      Json& error) {
    auto recipient = dinero::DecodePaymentTarget(address);
    if (!recipient.isValid()) {
        error["code"] = -5;
        error["message"] = "Invalid address: failed to decode payment target";
        return false;
    }
    script_hex = bytesToHex(recipient.script_pubkey);
    return true;
}

static std::optional<std::pair<std::string, uint64_t>> getAuthoritativeCoin(
    const std::shared_ptr<dinero::ChainstateService>& chainstate,
    dinero::ChainDB* chain_db,
    const dinero::TxOutPoint& outpoint) {
    if (chainstate->IsAssumeUTXOActive()) {
        auto coin = chainstate->GetActiveUTXO(dinero::OutPoint{outpoint.txid, outpoint.vout});
        if (!coin) return std::nullopt;
        return std::make_pair(bytesToHex(coin->scriptPubKey), coin->value.GetUna());
    }
    auto coin = chain_db->getCoin(outpoint.txid.AsUint256(), outpoint.vout);
    if (coin.status() != dinero::Status::Ok) return std::nullopt;
    return std::make_pair(coin.value().script_pubkey, coin.value().amount);
}

// ─── RPC: getaddressbalance ─────────────────────────────────────────────

/**
 * blockchain.getaddressbalance - Get confirmed balance with explicit mempool delta for an address
 *
 * Params: [0] address (string, required)
 * Returns:
 *   - confirmed / balance / spendable: confirmed-chain state only
 *   - unconfirmed: signed mempool delta for this address
 *   - estimated_balance: confirmed + mempool delta (advisory only)
 *   - *_din: DIN-formatted string mirrors of the above
 */
Json rpc_getaddressbalance(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
            result["error"]["code"] = -8;
            result["error"]["message"] = "Missing required parameter: address (string)";
            return result;
        }

        std::string address = params[0].asString();
        std::string target_script;
        Json decode_err;
        if (!decodeAddressToScriptHex(address, target_script, decode_err)) {
            result["error"] = decode_err;
            return result;
        }

        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Daemon/chainstate not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not initialized";
            return result;
        }

        // Sum confirmed UTXOs
        uint64_t confirmed = 0;
        bool scan_ok = true;
        const bool assumed_utxo = chainstate->IsAssumeUTXOActive();
        if (assumed_utxo) {
            scan_ok = chainstate->ForEachActiveUTXO(
                [&](const dinero::OutPoint&, const dinero::consensus::UTXOEntry& coin) {
                    if (bytesToHex(coin.scriptPubKey) == target_script) confirmed += coin.value.GetUna();
                    return true;
                });
        } else {
            scan_ok = chain_db->forEachUTXO(
                [&](const uint256&, uint32_t, const dinero::Coin& coin) -> bool {
                    if (coin.script_pubkey == target_script) confirmed += coin.amount;
                    return true;
                }) == dinero::Status::Ok;
        }
        if (!scan_ok) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to scan UTXO set";
            return result;
        }

        // Sum unconfirmed from mempool
        int64_t unconfirmed = 0;
        if (ctx.daemon->mempool && ctx.daemon->mempool->isInitialized()) {
            auto mempool_txs = ctx.daemon->mempool->mempool().getTransactionsForAddress(address);
            for (const auto& tx : mempool_txs) {
                // Sum outputs to this address
                for (const auto& output : tx.vout) {
                    if (bytesToHex(output.scriptPubKey) == target_script) {
                        unconfirmed += static_cast<int64_t>(output.value.GetUna());
                    }
                }
                // Subtract inputs spending from this address. Prefer mempool
                // parents first so descendant spends cancel unconfirmed
                // receives correctly before the package confirms.
                for (const auto& input : tx.vin) {
                    bool matched_input = false;

                    if (auto parent_tx = ctx.daemon->mempool->mempool().getTransaction(input.prevout.txid.AsUint256());
                        parent_tx && input.prevout.vout < parent_tx->vout.size()) {
                        const auto& spent_output = parent_tx->vout[input.prevout.vout];
                        if (bytesToHex(spent_output.scriptPubKey) == target_script) {
                            unconfirmed -= static_cast<int64_t>(spent_output.value.GetUna());
                            matched_input = true;
                        }
                    }

                    if (!matched_input) {
                        auto coin = getAuthoritativeCoin(chainstate, chain_db, input.prevout);
                        if (coin && coin->first == target_script) {
                            unconfirmed -= static_cast<int64_t>(coin->second);
                        }
                    }
                }
            }
        }

        const uint64_t estimated_balance = applySignedDelta(confirmed, unconfirmed);

        result["address"] = address;
        result["confirmed"] = static_cast<::Json::Value::Int64>(confirmed);
        result["unconfirmed"] = static_cast<::Json::Value::Int64>(unconfirmed);
        result["balance"] = static_cast<::Json::Value::Int64>(confirmed);
        result["spendable"] = static_cast<::Json::Value::Int64>(confirmed);
        result["estimated_balance"] = static_cast<::Json::Value::Int64>(estimated_balance);
        result["confirmed_din"] = formatDIN(confirmed);
        result["unconfirmed_din"] = formatSignedDIN(unconfirmed);
        result["balance_din"] = formatDIN(confirmed);
        result["spendable_din"] = formatDIN(confirmed);
        result["estimated_balance_din"] = formatDIN(estimated_balance);

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getaddressbalance] Exception: " + std::string(e.what()));
    }

    return result;
}

// ─── RPC: getaddressmempool ─────────────────────────────────────────────

/**
 * blockchain.getaddressmempool - Get pending mempool transactions for an address
 *
 * Params: [0] address (string, required)
 * Returns: { address, transactions: [...] }
 */
Json rpc_getaddressmempool(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
            result["error"]["code"] = -8;
            result["error"]["message"] = "Missing required parameter: address (string)";
            return result;
        }

        std::string address = params[0].asString();
        std::string target_script;
        Json decode_err;
        if (!decodeAddressToScriptHex(address, target_script, decode_err)) {
            result["error"] = decode_err;
            return result;
        }

        if (!ctx.daemon || !ctx.daemon->mempool || !ctx.daemon->mempool->isInitialized()) {
            result["address"] = address;
            result["transactions"] = Json(::Json::arrayValue);
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not initialized";
            return result;
        }

        auto& mempool = ctx.daemon->mempool->mempool();
        auto mempool_txs = mempool.getTransactionsForAddress(address);

        ::Json::Value txs_arr(::Json::arrayValue);
        for (const auto& tx : mempool_txs) {
            Json entry;
            entry["txid"] = tx.GetTxid().AsUint256().GetHex();

            // Calculate net amount and determine type
            int64_t net_amount = 0;
            bool has_output = false;
            bool has_input = false;

            for (const auto& output : tx.vout) {
                if (bytesToHex(output.scriptPubKey) == target_script) {
                    net_amount += static_cast<int64_t>(output.value.GetUna());
                    has_output = true;
                }
            }

            for (const auto& input : tx.vin) {
                bool matched_input = false;

                auto parent_tx = mempool.getTransaction(input.prevout.txid.AsUint256());
                if (parent_tx && input.prevout.vout < parent_tx->vout.size()) {
                    const auto& spent_output = parent_tx->vout[input.prevout.vout];
                    if (bytesToHex(spent_output.scriptPubKey) == target_script) {
                        net_amount -= static_cast<int64_t>(spent_output.value.GetUna());
                        has_input = true;
                        matched_input = true;
                    }
                }

                if (!matched_input) {
                    auto coin = getAuthoritativeCoin(chainstate, chain_db, input.prevout);
                    if (coin && coin->first == target_script) {
                        net_amount -= static_cast<int64_t>(coin->second);
                        has_input = true;
                    }
                }
            }

            entry["type"] = has_input ? "send" : "receive";
            entry["amount"] = static_cast<::Json::Value::Int64>(absUna(net_amount));
            entry["amount_din"] = formatDIN(absUna(net_amount));
            entry["size"] = static_cast<::Json::Value::UInt64>(tx.GetSize());

            txs_arr.append(entry);
        }

        result["address"] = address;
        result["transactions"] = txs_arr;

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getaddressmempool] Exception: " + std::string(e.what()));
    }

    return result;
}

// ─── RPC: getaddresshistory ─────────────────────────────────────────────

/**
 * blockchain.getaddresshistory - Get confirmed transaction history for an address
 *
 * Params: [0] address (string, required)
 *         [1] count (int, optional, default 50, max 200)
 *         [2] from_height (int, optional, default tip)
 * Returns: { address, from_height, transactions: [...] }
 */
Json rpc_getaddresshistory(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
            result["error"]["code"] = -8;
            result["error"]["message"] = "Missing required parameter: address (string)";
            return result;
        }

        std::string address = params[0].asString();
        std::string target_script;
        Json decode_err;
        if (!decodeAddressToScriptHex(address, target_script, decode_err)) {
            result["error"] = decode_err;
            return result;
        }

        int count = 50;
        if (params.size() > 1 && params[1].isInt()) {
            count = params[1].asInt();
            if (count < 1) count = 1;
            if (count > 200) count = 200;
        }

        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Daemon/chainstate not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not initialized";
            return result;
        }

        auto tip_result = chain_db->getTip();
        if (!tip_result.ok()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to get chain tip";
            return result;
        }

        const bool assumed_utxo = chainstate->IsAssumeUTXOActive();
        int tip_height = assumed_utxo
            ? static_cast<int>(chainstate->getBlockHeight())
            : tip_result.value().height;

        int from_height = tip_height;
        if (params.size() > 2 && params[2].isInt()) {
            from_height = params[2].asInt();
            if (from_height > tip_height) from_height = tip_height;
            if (from_height < 0) from_height = 0;
        }

        ::Json::Value txs_arr(::Json::arrayValue);
        int found = 0;

        // AssumeUTXO proves current spendable state but does not promise that
        // pre-base block bodies are locally available. Report this explicitly
        // instead of returning a deceptively complete empty history.
        for (int h = assumed_utxo ? -1 : from_height; h >= 0 && found < count; --h) {
            auto hash_result = chain_db->getBlockHashByHeight(h);
            if (!hash_result.ok()) continue;

            auto block_result = chainstate->getBlockByHash(hash_result.value());
            if (!block_result.ok()) continue;

            const auto& block = block_result.value();

            for (const auto& tx : block.vtx) {
                bool has_output = false;
                bool has_input = false;
                bool matched_conf_output = false;
                bool matched_conf_input = false;
                bool tx_has_conf_outputs = false;
                bool tx_has_conf_inputs = false;
                int64_t visible_amount = 0;
                std::string matched_commitment;
                uint64_t matched_range_proof_bytes = 0;
                uint64_t matched_nonce_bytes = 0;

                // Check outputs
                for (const auto& output : tx.vout) {
                    tx_has_conf_outputs = tx_has_conf_outputs || output.is_confidential;
                    if (bytesToHex(output.scriptPubKey) == target_script) {
                        has_output = true;
                        if (output.is_confidential) {
                            matched_conf_output = true;
                            matched_commitment = bytesToHex(output.commitment);
                            matched_range_proof_bytes = output.range_proof.size();
                            matched_nonce_bytes = output.nonce.size();
                        } else {
                            visible_amount += static_cast<int64_t>(output.value.GetUna());
                        }
                    }
                }

                // Check inputs (skip coinbase)
                if (!tx.IsCoinbase()) {
                    for (const auto& input : tx.vin) {
                        auto prev_tx_result = chain_db->getTransaction(input.prevout.txid.AsUint256());
                        if (!prev_tx_result.ok()) continue;
                        const auto& prev_tx = prev_tx_result.value();
                        if (input.prevout.vout < prev_tx.vout.size()) {
                            const auto& spent_output = prev_tx.vout[input.prevout.vout];
                            tx_has_conf_inputs = tx_has_conf_inputs || spent_output.is_confidential;
                            if (bytesToHex(spent_output.scriptPubKey) == target_script) {
                                has_input = true;
                                if (spent_output.is_confidential) {
                                    matched_conf_input = true;
                                } else {
                                    visible_amount -= static_cast<int64_t>(spent_output.value.GetUna());
                                }
                            }
                        }
                    }
                }

                if (has_output || has_input) {
                    Json entry;
                    const std::string privacy_flow =
                        classifyAddressPrivacyFlow(
                            has_input,
                            has_output,
                            matched_conf_input,
                            matched_conf_output,
                            tx_has_conf_inputs,
                            tx_has_conf_outputs);
                    const bool amount_hidden = matched_conf_output || matched_conf_input;
                    entry["txid"] = tx.GetTxid().AsUint256().GetHex();
                    entry["height"] = h;
                    entry["confirmations"] = tip_height - h + 1;
                    entry["type"] = has_input ? "send" : "receive";
                    // Coinbase (mining reward) flag — derived here from the block's
                    // own transactions (no txindex needed), so light clients can label
                    // Mined / Immature Mining instead of a plain receive.
                    entry["is_coinbase"] = tx.IsCoinbase();
                    entry["privacy_flow"] = privacy_flow;
                    entry["classification"] = privacy_flow;
                    entry["has_confidential_activity"] = tx_has_conf_inputs || tx_has_conf_outputs;
                    setHistoryAmountFields(
                        entry,
                        amount_hidden,
                        static_cast<uint64_t>(visible_amount >= 0 ? visible_amount : -visible_amount));
                    if (matched_conf_output) {
                        entry["commitment"] = matched_commitment;
                        entry["range_proof_bytes"] = static_cast<::Json::Value::UInt64>(matched_range_proof_bytes);
                        entry["nonce_bytes"] = static_cast<::Json::Value::UInt64>(matched_nonce_bytes);
                    }

                    txs_arr.append(entry);
                    found++;
                    if (found >= count) break;
                }
            }
        }

        result["address"] = address;
        result["from_height"] = from_height;
        result["transactions"] = txs_arr;
        result["history_complete"] = !assumed_utxo;

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getaddresshistory] Exception: " + std::string(e.what()));
    }

    return result;
}

// Internal batch primitive used by wallet.snapshot. It performs one UTXO pass and
// one chain pass for the complete address gap window instead of repeating both
// scans once per address. Batch history is intentionally capped at the newest
// 50 wallet transactions globally (versus 200 for getaddresshistory). A per-
// address cap would require every unused gap address to "fill" before the scan
// could stop, turning ordinary wallet refreshes into full-chain scans.
static Json computeAddressBatch(const ExecutionContext& ctx,
                                const Json& params,
                                BatchClock::time_point deadline) {
    Json result;
    try {
        if (!params.isObject() || !params.isMember("addresses") || !params["addresses"].isArray()) {
            result["error"]["code"] = -8;
            result["error"]["message"] = "addresses array is required";
            return result;
        }
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Daemon/chainstate not available";
            return result;
        }
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not initialized";
            return result;
        }

        int count = params.isMember("history_count") && params["history_count"].isInt()
            ? params["history_count"].asInt() : 50;
        count = std::max(1, std::min(count, MAX_BATCH_HISTORY));

        struct AddressState {
            std::string address;
            std::string script;
            uint64_t confirmed = 0;
            int64_t unconfirmed = 0;
            Json transactions = Json(::Json::arrayValue);
        };
        std::vector<AddressState> states;
        std::unordered_map<std::string, size_t> script_to_index;
        for (const auto& value : params["addresses"]) {
            if (!value.isString() || states.size() >= MAX_BATCH_ADDRESSES) continue;
            std::string script;
            Json decode_error;
            if (!decodeAddressToScriptHex(value.asString(), script, decode_error)) {
                result["error"] = decode_error;
                return result;
            }
            if (script_to_index.find(script) != script_to_index.end()) continue;
            script_to_index.emplace(script, states.size());
            states.push_back({value.asString(), std::move(script)});
        }
        if (states.empty()) {
            result["error"]["code"] = -8;
            result["error"]["message"] = "no valid addresses supplied";
            return result;
        }

        const bool assumed_utxo = chainstate->IsAssumeUTXOActive();
        if (assumed_utxo) {
            // An assumed UTXO set is active before its background replay has
            // populated ChainDB. Read that authoritative in-memory set so a
            // snapshot-bootstrapped bridge cannot report false zero balances.
            const bool scanned = chainstate->ForEachActiveUTXO(
                [&](const dinero::OutPoint&, const dinero::consensus::UTXOEntry& coin) {
                    auto found = script_to_index.find(bytesToHex(coin.scriptPubKey));
                    if (found != script_to_index.end()) {
                        states[found->second].confirmed += coin.value.GetUna();
                    }
                    return true;
                });
            if (!scanned) {
                result["error"]["code"] = -1;
                result["error"]["message"] = "Active AssumeUTXO set is unavailable";
                return result;
            }
        } else {
            auto utxo_status = chain_db->forEachUTXO(
                [&](const uint256&, uint32_t, const dinero::Coin& coin) -> bool {
                    auto found = script_to_index.find(coin.script_pubkey);
                    if (found != script_to_index.end()) states[found->second].confirmed += coin.amount;
                    return true;
                });
            if (utxo_status != dinero::Status::Ok) {
                result["error"]["code"] = -1;
                result["error"]["message"] = "Failed to scan UTXO set";
                return result;
            }
        }
        if (BatchClock::now() > deadline) {
            result["error"]["code"] = -32006;
            result["error"]["message"] = "batch discovery timed out";
            return result;
        }

        if (ctx.daemon->mempool && ctx.daemon->mempool->isInitialized()) {
            for (auto& state : states) {
                auto mempool_txs = ctx.daemon->mempool->mempool().getTransactionsForAddress(state.address);
                for (const auto& tx : mempool_txs) {
                    for (const auto& output : tx.vout) {
                        if (bytesToHex(output.scriptPubKey) == state.script)
                            state.unconfirmed += static_cast<int64_t>(output.value.GetUna());
                    }
                    for (const auto& input : tx.vin) {
                        bool matched_input = false;
                        if (auto parent_tx = ctx.daemon->mempool->mempool().getTransaction(
                                input.prevout.txid.AsUint256());
                            parent_tx && input.prevout.vout < parent_tx->vout.size()) {
                            const auto& output = parent_tx->vout[input.prevout.vout];
                            if (bytesToHex(output.scriptPubKey) == state.script) {
                                state.unconfirmed -= static_cast<int64_t>(output.value.GetUna());
                                matched_input = true;
                            }
                        }
                        if (!matched_input) {
                            auto coin = getAuthoritativeCoin(chainstate, chain_db, input.prevout);
                            if (coin && coin->first == state.script) {
                                state.unconfirmed -= static_cast<int64_t>(coin->second);
                            }
                        }
                    }
                }
            }
        }

        auto tip_result = chain_db->getTip();
        if (!tip_result.ok()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to get chain tip";
            return result;
        }
        const int tip_height = assumed_utxo
            ? static_cast<int>(chainstate->getBlockHeight())
            : tip_result.value().height;
        size_t history_transactions = 0;
        // Snapshot bootstrap guarantees the current UTXO set, not historical
        // block bodies. Do not turn a balance query into a futile 90k-height
        // scan while background history validation is incomplete.
        for (int height = assumed_utxo ? -1 : tip_height;
             height >= 0 && history_transactions < static_cast<size_t>(count); --height) {
            if ((height & 0x3f) == 0 && BatchClock::now() > deadline) {
                result.clear();
                result["error"]["code"] = -32006;
                result["error"]["message"] = "batch discovery timed out";
                return result;
            }
            auto hash_result = chain_db->getBlockHashByHeight(height);
            if (!hash_result.ok()) continue;
            auto block_result = chainstate->getBlockByHash(hash_result.value());
            if (!block_result.ok()) continue;
            for (const auto& tx : block_result.value().vtx) {
                struct Match {
                    bool input = false;
                    bool output = false;
                    bool conf_input = false;
                    bool conf_output = false;
                    bool tx_conf_inputs = false;
                    bool tx_conf_outputs = false;
                    int64_t visible = 0;
                    std::string commitment;
                    uint64_t range_proof_bytes = 0;
                    uint64_t nonce_bytes = 0;
                };
                std::unordered_map<size_t, Match> matches;
                bool tx_has_conf_outputs = false;
                for (const auto& output : tx.vout) {
                    tx_has_conf_outputs = tx_has_conf_outputs || output.is_confidential;
                    auto found = script_to_index.find(bytesToHex(output.scriptPubKey));
                    if (found == script_to_index.end()) continue;
                    auto& match = matches[found->second];
                    match.output = true;
                    if (output.is_confidential) {
                        match.conf_output = true;
                        match.commitment = bytesToHex(output.commitment);
                        match.range_proof_bytes = output.range_proof.size();
                        match.nonce_bytes = output.nonce.size();
                    } else {
                        match.visible += static_cast<int64_t>(output.value.GetUna());
                    }
                }
                bool tx_has_conf_inputs = false;
                if (!tx.IsCoinbase()) {
                    for (const auto& input : tx.vin) {
                        auto previous = chain_db->getTransaction(input.prevout.txid.AsUint256());
                        if (!previous.ok() || input.prevout.vout >= previous.value().vout.size()) continue;
                        const auto& output = previous.value().vout[input.prevout.vout];
                        tx_has_conf_inputs = tx_has_conf_inputs || output.is_confidential;
                        auto found = script_to_index.find(bytesToHex(output.scriptPubKey));
                        if (found == script_to_index.end()) continue;
                        auto& match = matches[found->second];
                        match.input = true;
                        if (output.is_confidential) match.conf_input = true;
                        else match.visible -= static_cast<int64_t>(output.value.GetUna());
                    }
                }
                if (matches.empty()) continue;
                for (auto& [index, match] : matches) {
                    auto& state = states[index];
                    match.tx_conf_inputs = tx_has_conf_inputs;
                    match.tx_conf_outputs = tx_has_conf_outputs;
                    Json entry;
                    entry["txid"] = tx.GetTxid().AsUint256().GetHex();
                    entry["height"] = height;
                    entry["confirmations"] = tip_height - height + 1;
                    entry["type"] = match.input ? "send" : "receive";
                    entry["is_coinbase"] = tx.IsCoinbase();
                    const std::string flow = classifyAddressPrivacyFlow(
                        match.input, match.output, match.conf_input, match.conf_output,
                        match.tx_conf_inputs, match.tx_conf_outputs);
                    entry["privacy_flow"] = flow;
                    entry["classification"] = flow;
                    entry["has_confidential_activity"] = tx_has_conf_inputs || tx_has_conf_outputs;
                    setHistoryAmountFields(entry, match.conf_input || match.conf_output, absUna(match.visible));
                    if (match.conf_output) {
                        entry["commitment"] = match.commitment;
                        entry["range_proof_bytes"] = static_cast<::Json::Value::UInt64>(match.range_proof_bytes);
                        entry["nonce_bytes"] = static_cast<::Json::Value::UInt64>(match.nonce_bytes);
                    }
                    state.transactions.append(entry);
                }
                // Count the chain transaction once even when it touches both a
                // receive and change address. Preserve all of its address-scoped
                // rows so wallet.snapshot can classify the complete event.
                ++history_transactions;
                if (history_transactions >= static_cast<size_t>(count)) break;
            }
        }

        Json address_results(::Json::objectValue);
        for (const auto& state : states) {
            Json item;
            item["confirmed"] = static_cast<::Json::Value::UInt64>(state.confirmed);
            item["unconfirmed"] = static_cast<::Json::Value::Int64>(state.unconfirmed);
            item["estimated_balance"] = static_cast<::Json::Value::UInt64>(
                applySignedDelta(state.confirmed, state.unconfirmed));
            item["transactions"] = state.transactions;
            address_results[state.address] = item;
        }
        result["tip_height"] = tip_height;
        result["history_complete"] = !assumed_utxo;
        result["history_count"] = count;
        result["history_limit"] = MAX_BATCH_HISTORY;
        result["history_scope"] = "wallet_global";
        result["addresses"] = address_results;
        Json proof_context;
        proof_context["tip_height"] = tip_height;
        proof_context["tip_hash"] = ctx.daemon->chainstate->getBestBlockHash();
        const auto commitment = din::rpc_getutreexocommitment(ctx, din::arr());
        proof_context["utreexo_root"] = commitment.isMember("commitment")
            ? commitment["commitment"] : Json("");
        proof_context["available"] = proof_context["tip_hash"].isString() &&
            !proof_context["tip_hash"].asString().empty() &&
            proof_context["utreexo_root"].isString() &&
            !proof_context["utreexo_root"].asString().empty();
        result["proof_context"] = proof_context;
    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
    }
    return result;
}

Json rpc_getaddressbatch(const ExecutionContext& ctx, const Json& params) {
    Json result;
    if (!params.isObject() || !params.isMember("addresses") || !params["addresses"].isArray()) {
        result["error"]["code"] = -8;
        result["error"]["message"] = "addresses array is required";
        return result;
    }
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Daemon/chainstate not available";
        return result;
    }

    std::vector<std::string> addresses;
    addresses.reserve(params["addresses"].size());
    for (const auto& value : params["addresses"]) {
        if (value.isString()) addresses.push_back(value.asString());
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    if (addresses.empty() || addresses.size() > MAX_BATCH_ADDRESSES) {
        result["error"]["code"] = -8;
        result["error"]["message"] = "addresses must contain between 1 and 100 unique strings";
        return result;
    }
    int history_count = params.isMember("history_count") && params["history_count"].isInt()
        ? params["history_count"].asInt() : 50;
    if (history_count < 1 || history_count > MAX_BATCH_HISTORY) {
        result["error"]["code"] = -8;
        result["error"]["message"] = "history_count must be between 1 and 50";
        return result;
    }

    std::ostringstream key_builder;
    key_builder << ctx.daemon->chainstate->getBestBlockHash() << ':' << history_count;
    for (const auto& address : addresses) key_builder << ':' << address;
    const std::string key = key_builder.str();
    const auto now = BatchClock::now();

    std::shared_ptr<BatchFlight> flight;
    {
        std::unique_lock<std::mutex> lock(g_batch_discovery.mutex);
        ++g_batch_discovery.requests;
        pruneBatchState(g_batch_discovery, now);

        if (auto cached = g_batch_discovery.cache.find(key);
            cached != g_batch_discovery.cache.end()) {
            ++g_batch_discovery.cache_hits;
            result = cached->second.result;
            addBatchMetadata(result, true, 0, addresses.size(), g_batch_discovery);
            return result;
        }

        if (auto running = g_batch_discovery.flights.find(key);
            running != g_batch_discovery.flights.end()) {
            flight = running->second;
            ++g_batch_discovery.waiters;
            const bool completed = flight->changed.wait_for(
                lock, BATCH_WAIT_TIMEOUT, [&] { return !flight->running; });
            if (completed && !flight->result.isMember("error")) {
                ++g_batch_discovery.cache_hits;
                result = flight->result;
                addBatchMetadata(result, true, 0, addresses.size(), g_batch_discovery);
                return result;
            }
            ++g_batch_discovery.rejected;
            if (completed) result = flight->result;
            else {
                result["error"]["code"] = -32006;
                result["error"]["message"] = "batch discovery wait timed out";
            }
            addBatchMetadata(result, false, 0, addresses.size(), g_batch_discovery);
            return result;
        }

        if (g_batch_discovery.active_scans >= MAX_CONCURRENT_BATCH_SCANS) {
            ++g_batch_discovery.rejected;
            result["error"]["code"] = -32005;
            result["error"]["message"] = "batch discovery busy";
            addBatchMetadata(result, false, 0, addresses.size(), g_batch_discovery);
            return result;
        }

        if (auto previous = g_batch_discovery.last_scan_started.find(key);
            previous != g_batch_discovery.last_scan_started.end() &&
            now - previous->second < BATCH_SCAN_COOLDOWN) {
            ++g_batch_discovery.rejected;
            result["error"]["code"] = -32005;
            result["error"]["message"] = "batch discovery rate limited; retry shortly";
            addBatchMetadata(result, false, 0, addresses.size(), g_batch_discovery);
            return result;
        }

        flight = std::make_shared<BatchFlight>();
        g_batch_discovery.flights.emplace(key, flight);
        g_batch_discovery.last_scan_started[key] = now;
        ++g_batch_discovery.active_scans;
        ++g_batch_discovery.scans;
    }

    result = computeAddressBatch(ctx, params, now + BATCH_SCAN_TIMEOUT);
    const uint64_t scan_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(BatchClock::now() - now).count());
    {
        std::lock_guard<std::mutex> lock(g_batch_discovery.mutex);
        g_batch_discovery.total_scan_ms += scan_ms;
        if (result.isMember("error")) {
            ++g_batch_discovery.failures;
            dinero::g_logger.warning("[getaddressbatch] scan failed after " +
                std::to_string(scan_ms) + " ms: " + result["error"]["message"].asString());
        } else {
            g_batch_discovery.cache[key] = BatchCacheEntry{result, BatchClock::now()};
        }
        flight->result = result;
        flight->running = false;
        g_batch_discovery.flights.erase(key);
        if (g_batch_discovery.active_scans > 0) --g_batch_discovery.active_scans;
        pruneBatchState(g_batch_discovery, BatchClock::now());
        addBatchMetadata(result, false, scan_ms, addresses.size(), g_batch_discovery);
    }
    flight->changed.notify_all();
    return result;
}


// ─── RPC: reindextx ─────────────────────────────────────────────────────

/**
 * blockchain.reindextx - Build transaction index for all blocks
 *
 * One-time backfill: iterates every block and writes txid -> (block_hash, offset)
 * entries to the tx index. Required for getaddresshistory send detection.
 * Safe to call multiple times (idempotent).
 *
 * Params: none
 * Returns: { indexed_blocks, indexed_txs }
 */
Json rpc_reindextx(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Daemon/chainstate not available";
            return result;
        }

        auto [indexed_blocks, indexed_txs] = ctx.daemon->chainstate->RebuildTxIndex();

        result["indexed_blocks"] = static_cast<::Json::Value::Int64>(indexed_blocks);
        result["indexed_txs"] = static_cast<::Json::Value::Int64>(indexed_txs);

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[reindextx] Exception: " + std::string(e.what()));
    }

    return result;
}

// ─── RPC: getcheckpoints ────────────────────────────────────────────────

/**
 * blockchain.getcheckpoints - Full 128-byte headers at every 1000th block
 *
 * Protocol-level checkpoint service for light client quorum bootstrap.
 * Any client can query multiple daemons and compare raw headers byte-for-byte.
 *
 * Params: [0] from_height (int, optional, default 0)
 * Returns: { checkpoints: [...], tip_height, interval }
 */
Json rpc_getcheckpoints(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Daemon/chainstate not available";
            return result;
        }

        dinero::ChainDB* chain_db = ctx.daemon->chainstate->GetChainDB();
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not initialized";
            return result;
        }

        auto tip_result = chain_db->getTip();
        if (!tip_result.ok()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to get chain tip";
            return result;
        }

        int tip_height = tip_result.value().height;

        int from_height = 0;
        if (params.isArray() && params.size() > 0 && params[0].isInt()) {
            from_height = params[0].asInt();
            if (from_height < 0) from_height = 0;
        }

        // Align from_height to nearest 1000-block boundary
        from_height = (from_height / 1000) * 1000;

        static constexpr int INTERVAL = 1000;

        ::Json::Value checkpoints_arr(::Json::arrayValue);

        for (int h = from_height; h <= tip_height; h += INTERVAL) {
            auto hash_result = chain_db->getBlockHashByHeight(h);
            if (!hash_result.ok()) continue;

            const uint256& hash = hash_result.value();

            auto header_result = chain_db->getHeader(hash);
            if (!header_result.ok()) continue;

            const auto& header = header_result.value();
            auto raw_bytes = header.SerializeForHash();

            // Convert 128 raw bytes to hex
            std::ostringstream raw_hex;
            for (uint8_t b : raw_bytes) {
                raw_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            }

            Json cp;
            cp["height"] = h;
            cp["raw_header"] = raw_hex.str();
            cp["block_hash"] = header.GetHash().GetHex();
            cp["timestamp"] = static_cast<::Json::Value::UInt64>(header.timestamp);
            cp["prev_hash"] = header.prev_block_hash.GetHex();
            cp["utreexo_root"] = header.utreexo_root.GetHex();
            cp["nbits"] = static_cast<::Json::Value::UInt64>(header.difficulty);
            cp["nonce"] = static_cast<::Json::Value::UInt64>(header.nonce);
            cp["version"] = static_cast<int>(header.version);

            auto work_result = chain_db->getBlockWork(hash);
            if (work_result.ok()) {
                cp["chainwork"] = "0x" + work_result.value().GetHex();
            }

            checkpoints_arr.append(cp);
        }

        result["checkpoints"] = checkpoints_arr;
        result["tip_height"] = tip_height;
        result["interval"] = INTERVAL;

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getcheckpoints] Exception: " + std::string(e.what()));
    }

    return result;
}

} // namespace din
