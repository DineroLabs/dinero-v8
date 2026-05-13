/**
 * Contract RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates all contract/escrow RPC methods from legacy globals to DaemonContext.
 *
 * OLD PATTERN (legacy):
 *   extern ChainDB* g_chain_db_direct;
 *   uint32_t height = dinero::storage::GetChainHeight(dinero::legacy::g_chain_db_direct());
 *
 * NEW PATTERN (context-aware):
 *   auto chainstate = ctx.daemon->chainstate;
 *   uint32_t height = chainstate->getBlockHeight();
 *
 * Benefits:
 * - No dependency on global chain DB
 * - Testable with mock services
 * - Clear dependency tracking
 */

#include "din_json.h"
#include <iostream>
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include "contracts/escrow_contract.h"
#include "contracts/contract_registry.h"
#include "contracts/daemon_mediator.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include <memory>

// ═══════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════

using namespace dinero::contracts;

// Forward declaration for canonical tx broadcast path.
din::Json rpc_context_mempool_sendrawtransaction(const ExecutionContext& ctx, const din::Json& params);

// Parse JSON parameters (handles CLI string-in-array format)
static din::Json parseContractParams(const din::Json& params) {
    if (params.isObject()) {
        return params;
    }

    if (params.isArray() && params.size() > 0) {
        const din::Json& first = params[0];
        if (first.isObject()) {
            return first;
        }
        if (first.isString()) {
            std::string json_str = first.asString();
            if (json_str.empty() || json_str.find_first_not_of(" \t\n\r") == std::string::npos) {
                return din::Json(Json::objectValue);
            }

            Json::CharReaderBuilder builder;
            Json::CharReader* reader = builder.newCharReader();
            std::string errors;
            din::Json parsed;

            bool success = reader->parse(
                json_str.c_str(),
                json_str.c_str() + json_str.length(),
                &parsed,
                &errors
            );
            delete reader;

            if (!success) {
                throw std::runtime_error("Failed to parse JSON parameters: " + errors);
            }

            return parsed;
        }
    }

    if (params.isNull() || (params.isArray() && params.size() == 0)) {
        return din::Json(Json::objectValue);
    }

    throw std::runtime_error("Invalid parameter format (expected object or JSON string)");
}

// Extract a positional or named string parameter.
static bool extractStringParam(
    const din::Json& params,
    Json::ArrayIndex pos,
    const char* name,
    std::string& out
) {
    if (params.isObject()) {
        if (!params.isMember(name) || !params[name].isString()) {
            return false;
        }
        out = params[name].asString();
        return true;
    }

    if (params.isArray() && params.size() > pos && params[pos].isString()) {
        out = params[pos].asString();
        return true;
    }

    return false;
}

static din::Json broadcastSignedEscrowTx(const ExecutionContext& ctx, const din::Json& params, const char* usage) {
    din::Json result;
    std::string tx_hex;

    if (!extractStringParam(params, 0, "signed_tx", tx_hex) &&
        !extractStringParam(params, 0, "tx_hex", tx_hex)) {
        result["error"] = usage;
        return result;
    }

    din::Json relay_params = din::arr();
    relay_params.append(tx_hex);

    din::Json relay_result = rpc_context_mempool_sendrawtransaction(ctx, relay_params);
    if (relay_result.isMember("error")) {
        return relay_result;
    }

    result["success"] = true;
    result["txid"] = relay_result.isMember("result") ? relay_result["result"] : Json::nullValue;
    return result;
}

// Convert EscrowContract to JSON
static din::Json contractToJson(const EscrowContract& contract) {
    din::Json result;

    result["contract_id"] = contract.contract_id;
    result["p2sh_address"] = contract.p2sh_address;
    result["amount"] = contract.amount;
    result["refund_time"] = contract.refund_time;
    result["status"] = contract.status;
    result["confirmations"] = contract.confirmations;
    result["lock_txid"] = contract.lock_txid;
    result["created_at"] = static_cast<Json::UInt64>(contract.created_at);

    din::Json keys;
    keys["buyer"] = contract.keys.buyer_pubkey;
    keys["seller"] = contract.keys.seller_pubkey;
    keys["mediator"] = contract.keys.mediator_pubkey;
    result["keys"] = keys;

    result["redeem_script"] = contract.redeem_script;
    result["script_hash"] = contract.script_hash;

    return result;
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE CONTRACT RPC HANDLERS
// ═════════════════════════════════════════════════════════════

/**
 * contract.createescrow - Create new escrow contract
 *
 * OLD: Uses dinero::legacy::g_chain_db_direct() for block height
 * NEW: ctx.daemon->chainstate->getBlockHeight()
 */
din::Json rpc_context_contract_createescrow(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Parse parameters
        din::Json p = parseContractParams(params);

        // Validate required parameters
        if (!p.isMember("buyer_pubkey")) {
            result["error"] = "Missing 'buyer_pubkey' parameter";
            return result;
        }
        if (!p.isMember("seller_pubkey")) {
            result["error"] = "Missing 'seller_pubkey' parameter";
            return result;
        }
        if (!p.isMember("amount")) {
            result["error"] = "Missing 'amount' parameter";
            return result;
        }

        EscrowKeys keys;
        keys.buyer_pubkey = p["buyer_pubkey"].asString();
        keys.seller_pubkey = p["seller_pubkey"].asString();

        double amount = p["amount"].asDouble();
        uint32_t refund_blocks = p.isMember("refund_blocks") ?
            p["refund_blocks"].asUInt() : 2880;  // Default: ~6 days
        uint32_t seller_window_blocks = p.isMember("seller_window_blocks") ?
            p["seller_window_blocks"].asUInt() : 6;  // Default: 6 blocks

        // Determine contract type
        EscrowType contract_type = EscrowType::TwoOfThreeManual;
        if (p.isMember("type")) {
            std::string type_str = p["type"].asString();
            if (type_str == "2of2" || type_str == "simple") {
                contract_type = EscrowType::TwoOfTwo;
            } else if (type_str == "auto" || type_str == "daemon") {
                contract_type = EscrowType::TwoOfThreeAuto;
            } else if (type_str == "manual" || type_str == "custom") {
                contract_type = EscrowType::TwoOfThreeManual;
            } else {
                result["error"] = "Invalid type (use: 2of2, auto, or manual)";
                return result;
            }
        }

        // Handle mediator based on type
        if (contract_type == EscrowType::TwoOfTwo) {
            keys.mediator_pubkey = "";
        } else if (contract_type == EscrowType::TwoOfThreeAuto) {
            auto daemon_pubkey_opt = DaemonMediator::getMediatorPubKey();
            if (!daemon_pubkey_opt) {
                result["error"] = "Daemon mediator not initialized";
                return result;
            }
            keys.mediator_pubkey = *daemon_pubkey_opt;
        } else {
            if (!p.isMember("mediator_pubkey")) {
                result["error"] = "Missing 'mediator_pubkey' for manual escrow type";
                return result;
            }
            keys.mediator_pubkey = p["mediator_pubkey"].asString();
        }

        // Validate
        if (amount <= 0) {
            result["error"] = "Amount must be positive";
            return result;
        }
        if (keys.buyer_pubkey.empty() || keys.seller_pubkey.empty()) {
            result["error"] = "Buyer and seller public keys must be provided";
            return result;
        }
        if (contract_type != EscrowType::TwoOfTwo && keys.mediator_pubkey.empty()) {
            result["error"] = "Mediator public key required for 2-of-3 escrow";
            return result;
        }

        // Get current block height from context (NOT global!)
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"] = "Chainstate service not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"] = "Failed to cast chainstate service";
            return result;
        }

        uint32_t current_height = chainstate->getBlockHeight();

        // Build contract
        EscrowContract contract = EscrowContractBuilder::buildContract(
            keys, amount, refund_blocks, contract_type, seller_window_blocks, current_height
        );

        // Verify contract
        if (!EscrowContractBuilder::verifyContract(contract)) {
            result["error"] = "Contract verification failed";
            return result;
        }

        // Store contract (singleton registry)
        auto& registry = ContractRegistry::instance();
        if (!registry.storeContract(contract)) {
            result["error"] = "Failed to store contract";
            return result;
        }

        // Return result
        result = contractToJson(contract);
        result["success"] = true;

        std::string type_name;
        switch (contract_type) {
            case EscrowType::TwoOfTwo:
                type_name = "2-of-2 (simple)";
                break;
            case EscrowType::TwoOfThreeAuto:
                type_name = "2-of-3 (daemon mediator)";
                break;
            case EscrowType::TwoOfThreeManual:
                type_name = "2-of-3 (manual mediator)";
                break;
        }

        result["type"] = type_name;
        result["message"] = "Contract created successfully. Send " +
                           std::to_string(amount) + " DIN to " + contract.p2sh_address;

        dinero::g_logger.info("[Contract RPC] Created " + type_name + " escrow: " +
                             contract.contract_id);

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to create contract: ") + e.what();
    }

    return result;
}

/**
 * contract.status - Get contract status
 */
din::Json rpc_context_contract_status(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: contract.status <contract_id>";
        return result;
    }

    try {
        std::string contract_id = params[0].as<std::string>();

        auto& registry = ContractRegistry::instance();
        auto contract_opt = registry.getContract(contract_id);

        if (!contract_opt) {
            result["error"] = "Contract not found";
            return result;
        }

        result = contractToJson(*contract_opt);
        result["success"] = true;

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get contract status: ") + e.what();
    }

    return result;
}

/**
 * contract.list - List all contracts (optionally filtered by address)
 */
din::Json rpc_context_contract_list(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        std::string filter_address = "";
        if (!params.empty() && params[0].is<std::string>()) {
            filter_address = params[0].as<std::string>();
        }

        auto& registry = ContractRegistry::instance();
        auto contracts = registry.listContracts(filter_address);

        din::Json contracts_array = din::arr();
        for (const auto& contract : contracts) {
            contracts_array.append(contractToJson(contract));
        }

        result["contracts"] = contracts_array;
        result["count"] = static_cast<int>(contracts.size());
        result["success"] = true;

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to list contracts: ") + e.what();
    }

    return result;
}

/**
 * contract.release - Build release transaction hex
 */
din::Json rpc_context_contract_release(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    din::Json result;

    std::string contract_id;
    std::string to_address;
    std::string sig_buyer;
    std::string sig_seller;

    if (!extractStringParam(params, 0, "contract_id", contract_id) ||
        !extractStringParam(params, 1, "to_address", to_address) ||
        !extractStringParam(params, 2, "sig_buyer", sig_buyer) ||
        !extractStringParam(params, 3, "sig_seller", sig_seller)) {
        result["error"] = "Usage: contract.release <contract_id> <to_address> <sig_buyer> <sig_seller>";
        return result;
    }

    try {
        auto& registry = ContractRegistry::instance();
        auto contract_opt = registry.getContract(contract_id);

        if (!contract_opt) {
            result["error"] = "Contract not found";
            return result;
        }
        if (contract_opt->lock_txid.empty()) {
            result["error"] = "Contract is not funded (missing lock transaction)";
            return result;
        }

        std::string tx_hex = EscrowContractBuilder::createReleaseTransaction(
            *contract_opt,
            to_address,
            sig_buyer,
            sig_seller
        );
        if (tx_hex.empty()) {
            result["error"] = "Failed to construct release transaction";
            return result;
        }

        result["success"] = true;
        result["contract_id"] = contract_id;
        result["tx_hex"] = tx_hex;
        result["broadcast_required"] = true;
        result["message"] = "Release transaction created. Broadcast with contract.broadcastrelease";

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to release contract: ") + e.what();
    }

    return result;
}

/**
 * contract.refund - Build refund transaction hex
 */
din::Json rpc_context_contract_refund(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    din::Json result;

    std::string contract_id;
    std::string refund_address;
    std::string sig_buyer;

    if (!extractStringParam(params, 0, "contract_id", contract_id) ||
        !extractStringParam(params, 1, "refund_address", refund_address) ||
        !extractStringParam(params, 2, "sig_buyer", sig_buyer)) {
        result["error"] = "Usage: contract.refund <contract_id> <refund_address> <sig_buyer>";
        return result;
    }

    try {
        auto& registry = ContractRegistry::instance();
        auto contract_opt = registry.getContract(contract_id);

        if (!contract_opt) {
            result["error"] = "Contract not found";
            return result;
        }
        if (contract_opt->lock_txid.empty()) {
            result["error"] = "Contract is not funded (missing lock transaction)";
            return result;
        }

        std::string tx_hex = EscrowContractBuilder::createRefundTransaction(
            *contract_opt,
            refund_address,
            sig_buyer
        );
        if (tx_hex.empty()) {
            result["error"] = "Failed to construct refund transaction";
            return result;
        }

        result["success"] = true;
        result["contract_id"] = contract_id;
        result["tx_hex"] = tx_hex;
        result["broadcast_required"] = true;
        result["message"] = "Refund transaction created. Broadcast with contract.broadcastrefund";

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to refund contract: ") + e.what();
    }

    return result;
}

/**
 * contract.broadcastrelease - Broadcast signed release transaction via mempool ingress
 */
din::Json rpc_context_contract_broadcastrelease(const ExecutionContext& ctx, const din::Json& params) {
    return broadcastSignedEscrowTx(
        ctx,
        params,
        "Usage: contract.broadcastrelease <signed_tx_hex>"
    );
}

/**
 * contract.broadcastrefund - Broadcast signed refund transaction via mempool ingress
 */
din::Json rpc_context_contract_broadcastrefund(const ExecutionContext& ctx, const din::Json& params) {
    return broadcastSignedEscrowTx(
        ctx,
        params,
        "Usage: contract.broadcastrefund <signed_tx_hex>"
    );
}

/**
 * contract.setlocktx - Set lock transaction for contract
 */
din::Json rpc_context_contract_setlocktx(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.size() < 2) {
        result["error"] = "Usage: contract.setlocktx <contract_id> <txid>";
        return result;
    }

    try {
        std::string contract_id = params[0].as<std::string>();
        std::string lock_txid = params[1].as<std::string>();

        auto& registry = ContractRegistry::instance();
        auto contract_opt = registry.getContract(contract_id);

        if (!contract_opt) {
            result["error"] = "Contract not found";
            return result;
        }

        // Update contract with lock txid
        auto contract = *contract_opt;
        contract.lock_txid = lock_txid;
        contract.status = "locked";

        if (!registry.updateContract(contract)) {
            result["error"] = "Failed to update contract";
            return result;
        }

        result["success"] = true;
        result["contract_id"] = contract_id;
        result["lock_txid"] = lock_txid;
        result["message"] = "Lock transaction set successfully";

        dinero::g_logger.info("[Contract RPC] Set lock tx for contract " + contract_id + ": " + lock_txid);

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to set lock transaction: ") + e.what();
    }

    return result;
}

/**
 * contract.getsighash - Currently unavailable in context-aware handler
 */
din::Json rpc_context_contract_getsighash(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    din::Json result;

    std::string contract_id;
    std::string tx_type;
    if (!extractStringParam(params, 0, "contract_id", contract_id) ||
        !extractStringParam(params, 1, "tx_type", tx_type)) {
        result["error"] = "Usage: contract.getsighash <contract_id> <release|refund>";
        return result;
    }

    result["error"] = "contract.getsighash is unavailable in this handler; use wallet signing paths for now";
    result["contract_id"] = contract_id;
    result["tx_type"] = tx_type;
    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

extern RpcRegistry g_rpcRegistry;

void registerContractMethodsContext() {
    // Core contract methods (fully implemented)
    g_rpcRegistry.registerHandler("contract.createescrow",
                                 rpc_context_contract_createescrow,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("contract.status",
                                 rpc_context_contract_status,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("contract.list",
                                 rpc_context_contract_list,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("contract.setlocktx",
                                 rpc_context_contract_setlocktx,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Transaction methods (build + broadcast pipeline)
    g_rpcRegistry.registerHandler("contract.release",
                                 rpc_context_contract_release,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("contract.refund",
                                 rpc_context_contract_refund,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("contract.broadcastrelease",
                                 rpc_context_contract_broadcastrelease,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("contract.broadcastrefund",
                                 rpc_context_contract_broadcastrefund,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("contract.getsighash",
                                 rpc_context_contract_getsighash,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] ✅ 9 contract context-aware handlers registered");
}
