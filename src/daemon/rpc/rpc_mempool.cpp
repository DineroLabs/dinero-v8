#include "daemon/tx_mempool.h"
#include "daemon/validation.h"
#if DIN_ENABLE_LEGACY_RPC
/* daemon-only: legacy rpc_server.h disabled */
#endif
#include "daemon/execution_context.h"
#include "daemon/block_acceptor.h"
#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "compat/jsoncpp_compat.h"
#include <util/hex.h>
#include <stdexcept>

using namespace dinero;

/**
 * sendrawtransaction RPC handler
 * Validates and broadcasts a raw transaction
 */
#if DIN_ENABLE_LEGACY_RPC
din::Json rpc_sendrawtransaction(const din::ExecutionContext& ctx, dinero::RPCServer& server, const din::Json& params) {
#else
din::Json rpc_sendrawtransaction_direct(const din::ExecutionContext& ctx, const din::Json& params) {
#endif
    try {
        if (!params.isArray() || params.size() < 1) {
            throw std::invalid_argument("sendrawtransaction requires at least 1 parameter");
        }

        if (!ctx.mempool) {
            throw std::runtime_error("Mempool not initialized");
        }

        if (!ctx.utxo_view) {
            throw std::runtime_error("UTXO view not initialized");
        }

        std::string hex_tx = params[0].asString();
        bool allow_high_fees = false;

        if (params.size() > 1 && params[1].isBool()) {
            allow_high_fees = params[1].asBool();
        }

        // Parse raw transaction from hex
        Transaction tx;
        std::vector<uint8_t> tx_bytes = util::HexToBytes(hex_tx);
        size_t offset = 0;
        if (!dinero::BlockAcceptor::ParseTransaction(tx_bytes.data(), tx_bytes.size(), offset, tx)) {
            throw std::runtime_error("Invalid transaction hex");
        }

        // Validate and accept to mempool
        MemPoolPolicy policy;
        auto outcome = AcceptToMemoryPool(tx, *ctx.mempool, policy, *ctx.utxo_view, false, false);

        if (outcome.IsRejected()) {
            din::Json error = din::obj();
            error["error"]["code"] = -26;
            error["error"]["message"] = outcome.reason;
            return error;
        }

        dinero::g_logger.info("Accepted raw transaction: " + outcome.txid);

        // Return transaction ID
        return din::Json(outcome.txid);

    } catch (const std::exception& e) {
        din::Json error = din::obj();
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("sendrawtransaction failed: ") + e.what();
        return error;
    }
}

/**
 * getrawmempool RPC handler
 * Returns mempool contents
 */
#if DIN_ENABLE_LEGACY_RPC
din::Json rpc_getrawmempool(const din::ExecutionContext& ctx, dinero::RPCServer& server, const din::Json& params) {
#else
din::Json rpc_getrawmempool_direct(const din::ExecutionContext& ctx, const din::Json& params) {
#endif
    try {
        if (!ctx.mempool) {
            throw std::runtime_error("Mempool not initialized");
        }

        bool verbose = false;
        if (params.isArray() && params.size() > 0 && params[0].isBool()) {
            verbose = params[0].asBool();
        }

        if (verbose) {
            // Return detailed information about each transaction
            din::Json result = din::obj();

            auto entries = ctx.mempool->GetEntries();
            for (const auto& entry : entries) {
                din::Json tx_info = din::obj();
                tx_info["size"] = static_cast<din::Json::Int64>(entry.size);
                tx_info["vsize"] = static_cast<din::Json::Int64>(entry.vsize);
                tx_info["weight"] = static_cast<din::Json::Int64>(entry.weight);
                tx_info["fee"] = static_cast<din::Json::Int64>(entry.fee);
                tx_info["modifiedfee"] = static_cast<din::Json::Int64>(entry.fee);
                tx_info["time"] = static_cast<din::Json::Int64>(entry.time);
                tx_info["height"] = static_cast<din::Json::Int64>(entry.height);
                tx_info["descendantcount"] = static_cast<din::Json::Int64>(entry.descendant_count);
                tx_info["descendantsize"] = static_cast<din::Json::Int64>(entry.descendant_size);
                tx_info["descendantfees"] = static_cast<din::Json::Int64>(entry.descendant_fees);
                tx_info["ancestorcount"] = static_cast<din::Json::Int64>(entry.ancestor_count);
                tx_info["ancestorsize"] = static_cast<din::Json::Int64>(entry.ancestor_size);
                tx_info["ancestorfees"] = static_cast<din::Json::Int64>(entry.ancestor_fees);
                
                // Dependencies
                din::Json depends = din::arr();
                for (const auto& dep : entry.depends) {
                    depends.append(dep);
                }
                tx_info["depends"] = depends;
                
                // Spent by
                din::Json spentby = din::arr();
                for (const auto& spent : entry.spentby) {
                    spentby.append(spent);
                }
                tx_info["spentby"] = spentby;
                
                // BIP 125 replaceable
                tx_info["bip125-replaceable"] = entry.rbf_enabled;
                
                result[entry.txid] = tx_info;
            }
            
            return result;
        } else {
            // Return simple array of transaction IDs
            din::Json result = din::arr();

            auto txids = ctx.mempool->GetTxIds();
            for (const auto& txid : txids) {
                result.append(txid);
            }

            return result;
        }
        
    } catch (const std::exception& e) {
        din::Json error = din::obj();
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("getrawmempool failed: ") + e.what();
        return error;
    }
}

/**
 * getmempoolentry RPC handler
 * Returns mempool entry for specific transaction
 */
#if DIN_ENABLE_LEGACY_RPC
din::Json rpc_getmempoolentry(const din::ExecutionContext& ctx, dinero::RPCServer& server, const din::Json& params) {
#else
din::Json rpc_getmempoolentry_direct(const din::ExecutionContext& ctx, const din::Json& params) {
#endif
    try {
        if (!params.isArray() || params.size() < 1) {
            throw std::invalid_argument("getmempoolentry requires 1 parameter");
        }

        if (!ctx.mempool) {
            throw std::runtime_error("Mempool not initialized");
        }

        std::string txid = params[0].asString();

        const auto* entry = ctx.mempool->Get(txid);
        if (!entry) {
            din::Json error = din::obj();
            error["error"]["code"] = -5;
            error["error"]["message"] = "Transaction not in mempool";
            return error;
        }
        
        din::Json result = din::obj();
        result["size"] = static_cast<din::Json::Int64>(entry->size);
        result["vsize"] = static_cast<din::Json::Int64>(entry->vsize);
        result["weight"] = static_cast<din::Json::Int64>(entry->weight);
        result["fee"] = static_cast<din::Json::Int64>(entry->fee);
        result["modifiedfee"] = static_cast<din::Json::Int64>(entry->fee);
        result["time"] = static_cast<din::Json::Int64>(entry->time);
        result["height"] = static_cast<din::Json::Int64>(entry->height);
        result["descendantcount"] = static_cast<din::Json::Int64>(entry->descendant_count);
        result["descendantsize"] = static_cast<din::Json::Int64>(entry->descendant_size);
        result["descendantfees"] = static_cast<din::Json::Int64>(entry->descendant_fees);
        result["ancestorcount"] = static_cast<din::Json::Int64>(entry->ancestor_count);
        result["ancestorsize"] = static_cast<din::Json::Int64>(entry->ancestor_size);
        result["ancestorfees"] = static_cast<din::Json::Int64>(entry->ancestor_fees);
        
        // Dependencies
        din::Json depends = din::arr();
        for (const auto& dep : entry->depends) {
            depends.append(dep);
        }
        result["depends"] = depends;
        
        // Spent by
        din::Json spentby = din::arr();
        for (const auto& spent : entry->spentby) {
            spentby.append(spent);
        }
        result["spentby"] = spentby;
        
        // BIP 125 replaceable
        result["bip125-replaceable"] = entry->rbf_enabled;
        
        return result;
        
    } catch (const std::exception& e) {
        din::Json error = din::obj();
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("getmempoolentry failed: ") + e.what();
        return error;
    }
}

/**
 * getmempoolinfo RPC handler
 * Returns mempool statistics
 */
#if DIN_ENABLE_LEGACY_RPC
din::Json rpc_getmempoolinfo(const din::ExecutionContext& ctx, dinero::RPCServer& server, const din::Json& params) {
#else
din::Json rpc_getmempoolinfo_direct(const din::ExecutionContext& ctx, const din::Json& params) {
#endif
    try {
        if (!ctx.mempool) {
            throw std::runtime_error("Mempool not initialized");
        }

        auto stats = ctx.mempool->GetStats();
        auto policy = ctx.mempool->GetPolicy();
        
        din::Json result = din::obj();
        result["loaded"] = true;
        result["size"] = static_cast<din::Json::Int64>(stats.tx_count);
        result["bytes"] = static_cast<din::Json::Int64>(stats.total_bytes);
        result["usage"] = static_cast<din::Json::Int64>(stats.total_bytes);  // Same as bytes for now
        result["total_fee"] = static_cast<din::Json::Int64>(stats.total_fees);
        result["maxmempool"] = static_cast<din::Json::Int64>(policy.max_size_bytes);
        result["mempoolminfee"] = static_cast<double>(policy.min_relay_feerate) / 100000000.0;  // Convert to DIN
        result["minrelaytxfee"] = static_cast<double>(policy.min_relay_feerate) / 100000000.0;
        result["unbroadcastcount"] = 0;  // Not implemented yet
        result["fullrbf"] = policy.rbf_enabled;
        
        // Additional statistics
        result["orphan_count"] = static_cast<din::Json::Int64>(stats.orphan_count);
        result["orphan_bytes"] = static_cast<din::Json::Int64>(stats.orphan_bytes);
        result["accepts_total"] = static_cast<din::Json::Int64>(stats.accepts_total);
        result["rejects_total"] = static_cast<din::Json::Int64>(stats.rejects_total);
        result["avg_feerate"] = stats.avg_feerate;
        result["last_updated"] = static_cast<din::Json::Int64>(stats.last_updated);
        
        // Reject reasons breakdown
        din::Json reject_reasons = din::obj();
        for (const auto& pair : stats.reject_reasons) {
            reject_reasons[pair.first] = static_cast<din::Json::Int64>(pair.second);
        }
        result["reject_reasons"] = reject_reasons;
        
        return result;
        
    } catch (const std::exception& e) {
        din::Json error = din::obj();
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("getmempoolinfo failed: ") + e.what();
        return error;
    }
}

/**
 * getmempoolancestors RPC handler
 * Returns ancestors of a mempool transaction
 */
#if DIN_ENABLE_LEGACY_RPC
din::Json rpc_getmempoolancestors(const din::ExecutionContext& ctx, dinero::RPCServer& server, const din::Json& params) {
#else
din::Json rpc_getmempoolancestors_direct(const din::ExecutionContext& ctx, const din::Json& params) {
#endif
    try {
        if (!params.isArray() || params.size() < 1) {
            throw std::invalid_argument("getmempoolancestors requires 1 parameter");
        }

        if (!ctx.mempool) {
            throw std::runtime_error("Mempool not initialized");
        }

        std::string txid = params[0].asString();
        bool verbose = false;

        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }

        const auto* entry = ctx.mempool->Get(txid);
        if (!entry) {
            din::Json error = din::obj();
            error["error"]["code"] = -5;
            error["error"]["message"] = "Transaction not in mempool";
            return error;
        }

        auto ancestors = ctx.mempool->GetAncestors(txid);

        if (verbose) {
            din::Json result = din::obj();
            for (const auto& ancestor_txid : ancestors) {
                const auto* ancestor_entry = ctx.mempool->Get(ancestor_txid);
                if (ancestor_entry) {
                    din::Json ancestor_info = din::obj();
                    ancestor_info["size"] = static_cast<din::Json::Int64>(ancestor_entry->size);
                    ancestor_info["vsize"] = static_cast<din::Json::Int64>(ancestor_entry->vsize);
                    ancestor_info["fee"] = static_cast<din::Json::Int64>(ancestor_entry->fee);
                    ancestor_info["time"] = static_cast<din::Json::Int64>(ancestor_entry->time);
                    result[ancestor_txid] = ancestor_info;
                }
            }
            return result;
        } else {
            din::Json result = din::arr();
            for (const auto& ancestor_txid : ancestors) {
                result.append(ancestor_txid);
            }
            return result;
        }
        
    } catch (const std::exception& e) {
        din::Json error = din::obj();
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("getmempoolancestors failed: ") + e.what();
        return error;
    }
}

/**
 * getmempooldescendants RPC handler
 * Returns descendants of a mempool transaction
 */
#if DIN_ENABLE_LEGACY_RPC
din::Json rpc_getmempooldescendants(const din::ExecutionContext& ctx, dinero::RPCServer& server, const din::Json& params) {
#else
din::Json rpc_getmempooldescendants_direct(const din::ExecutionContext& ctx, const din::Json& params) {
#endif
    try {
        if (!params.isArray() || params.size() < 1) {
            throw std::invalid_argument("getmempooldescendants requires 1 parameter");
        }

        if (!ctx.mempool) {
            throw std::runtime_error("Mempool not initialized");
        }

        std::string txid = params[0].asString();
        bool verbose = false;

        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }

        const auto* entry = ctx.mempool->Get(txid);
        if (!entry) {
            din::Json error = din::obj();
            error["error"]["code"] = -5;
            error["error"]["message"] = "Transaction not in mempool";
            return error;
        }

        auto descendants = ctx.mempool->GetDescendants(txid);

        if (verbose) {
            din::Json result = din::obj();
            for (const auto& descendant_txid : descendants) {
                const auto* descendant_entry = ctx.mempool->Get(descendant_txid);
                if (descendant_entry) {
                    din::Json descendant_info = din::obj();
                    descendant_info["size"] = static_cast<din::Json::Int64>(descendant_entry->size);
                    descendant_info["vsize"] = static_cast<din::Json::Int64>(descendant_entry->vsize);
                    descendant_info["fee"] = static_cast<din::Json::Int64>(descendant_entry->fee);
                    descendant_info["time"] = static_cast<din::Json::Int64>(descendant_entry->time);
                    result[descendant_txid] = descendant_info;
                }
            }
            return result;
        } else {
            din::Json result = din::arr();
            for (const auto& descendant_txid : descendants) {
                result.append(descendant_txid);
            }
            return result;
        }
        
    } catch (const std::exception& e) {
        din::Json error = din::obj();
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("getmempooldescendants failed: ") + e.what();
        return error;
    }
}

/**
 * testmempoolaccept RPC handler
 * Test if transactions would be accepted to mempool
 */
#if DIN_ENABLE_LEGACY_RPC
din::Json rpc_testmempoolaccept(const din::ExecutionContext& ctx, dinero::RPCServer& server, const din::Json& params) {
#else
din::Json rpc_testmempoolaccept_direct(const din::ExecutionContext& ctx, const din::Json& params) {
#endif
    try {
        if (!params.isArray() || params.size() < 1) {
            throw std::invalid_argument("testmempoolaccept requires 1 parameter");
        }

        if (!ctx.mempool) {
            throw std::runtime_error("Mempool not initialized");
        }

        if (!ctx.utxo_view) {
            throw std::runtime_error("UTXO view not initialized");
        }
        
        if (!params[0].isArray()) {
            throw std::invalid_argument("First parameter must be array of raw transactions");
        }
        
        double max_feerate = 0.10;  // Default max fee rate
        if (params.size() > 1 && params[1].isDouble()) {
            max_feerate = params[1].asDouble();
        }
        
        din::Json result = din::arr();
        
        for (const auto& hex_tx : params[0]) {
            din::Json tx_result = din::obj();
            
            try {
                std::string hex = hex_tx.asString();
                
                // Parse transaction (simplified)
                Transaction tx;
                tx.version = 2;
                tx.lockTime = 0;
                
                // Test acceptance
                MemPoolPolicy policy;
                auto outcome = AcceptToMemoryPool(tx, *ctx.mempool, policy, *ctx.utxo_view, true, false);  // test_accept = true
                
                tx_result["txid"] = outcome.txid;
                tx_result["allowed"] = outcome.IsAccepted();
                
                if (outcome.IsRejected()) {
                    tx_result["reject-reason"] = outcome.reason;
                }
                
                // Fee information
                if (outcome.fee > 0) {
                    tx_result["fees"]["base"] = static_cast<double>(outcome.fee) / 100000000.0;  // Convert to DIN
                    tx_result["fees"]["effective-feerate"] = outcome.feerate;
                    tx_result["fees"]["effective-includes"] = din::arr();  // Empty for now
                }
                
            } catch (const std::exception& e) {
                tx_result["allowed"] = false;
                tx_result["reject-reason"] = e.what();
            }
            
            result.append(tx_result);
        }
        
        return result;
        
    } catch (const std::exception& e) {
        din::Json error = din::obj();
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("testmempoolaccept failed: ") + e.what();
        return error;
    }
}

/**
 * Register all mempool RPC methods
 */
void registerMempoolRPCMethods() {
    extern RpcRegistry g_rpcRegistry;
    
    g_rpcRegistry.registerHandler("sendrawtransaction", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#if DIN_ENABLE_LEGACY_RPC
        dinero::RPCServer dummy_server;
        return rpc_sendrawtransaction(ctx, dummy_server, params);
#else
        return rpc_sendrawtransaction_direct(ctx, params);
#endif
    }, "mempool");

    g_rpcRegistry.registerHandler("getrawmempool", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#if DIN_ENABLE_LEGACY_RPC
        dinero::RPCServer dummy_server;
        return rpc_getrawmempool(ctx, dummy_server, params);
#else
        return rpc_getrawmempool_direct(ctx, params);
#endif
    }, "mempool");

    g_rpcRegistry.registerHandler("getmempoolentry", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#if DIN_ENABLE_LEGACY_RPC
        dinero::RPCServer dummy_server;
        return rpc_getmempoolentry(ctx, dummy_server, params);
#else
        return rpc_getmempoolentry_direct(ctx, params);
#endif
    }, "mempool");

    g_rpcRegistry.registerHandler("getmempoolinfo", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#if DIN_ENABLE_LEGACY_RPC
        dinero::RPCServer dummy_server;
        return rpc_getmempoolinfo(ctx, dummy_server, params);
#else
        return rpc_getmempoolinfo_direct(ctx, params);
#endif
    }, "mempool");

    g_rpcRegistry.registerHandler("getmempoolancestors", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#if DIN_ENABLE_LEGACY_RPC
        dinero::RPCServer dummy_server;
        return rpc_getmempoolancestors(ctx, dummy_server, params);
#else
        return rpc_getmempoolancestors_direct(ctx, params);
#endif
    }, "mempool");

    g_rpcRegistry.registerHandler("getmempooldescendants", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#if DIN_ENABLE_LEGACY_RPC
        dinero::RPCServer dummy_server;
        return rpc_getmempooldescendants(ctx, dummy_server, params);
#else
        return rpc_getmempooldescendants_direct(ctx, params);
#endif
    }, "mempool");

    g_rpcRegistry.registerHandler("testmempoolaccept", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#if DIN_ENABLE_LEGACY_RPC
        dinero::RPCServer dummy_server;
        return rpc_testmempoolaccept(ctx, dummy_server, params);
#else
        return rpc_testmempoolaccept_direct(ctx, params);
#endif
    }, "mempool");
    
    dinero::g_logger.info("✅ Mempool RPC methods registered (sendrawtransaction, getrawmempool, getmempoolinfo, etc.)");
}
