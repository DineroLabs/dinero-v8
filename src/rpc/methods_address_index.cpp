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
#include <sstream>
#include <iomanip>

namespace din {
using ExecutionContext = ::ExecutionContext;
using dinero::uint256;

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
        auto status = chain_db->forEachUTXO(
            [&](const uint256& txid, uint32_t vout, const dinero::Coin& coin) -> bool {
                if (coin.script_pubkey == target_script) {
                    confirmed += coin.amount;
                }
                return true;
            });

        if (status != dinero::Status::Ok) {
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
                        auto coin_result = chain_db->getCoin(input.prevout.txid.AsUint256(), input.prevout.vout);
                        if (coin_result.status() == dinero::Status::Ok &&
                            coin_result.value().script_pubkey == target_script) {
                            unconfirmed -= static_cast<int64_t>(coin_result.value().amount);
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

        dinero::ChainDB* chain_db = ctx.daemon->chainstate ? ctx.daemon->chainstate->GetChainDB() : nullptr;
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
                    auto coin_result = chain_db->getCoin(input.prevout.txid.AsUint256(), input.prevout.vout);
                    if (coin_result.status() == dinero::Status::Ok &&
                        coin_result.value().script_pubkey == target_script) {
                        net_amount -= static_cast<int64_t>(coin_result.value().amount);
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

        int tip_height = tip_result.value().height;

        int from_height = tip_height;
        if (params.size() > 2 && params[2].isInt()) {
            from_height = params[2].asInt();
            if (from_height > tip_height) from_height = tip_height;
            if (from_height < 0) from_height = 0;
        }

        ::Json::Value txs_arr(::Json::arrayValue);
        int found = 0;

        // Scan blocks from from_height down to genesis
        for (int h = from_height; h >= 0 && found < count; --h) {
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

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getaddresshistory] Exception: " + std::string(e.what()));
    }

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
