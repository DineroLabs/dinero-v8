/**
 * Smart Contract RPC Methods - vNext Architecture
 *
 * Escrow and contract management methods with full metadata.
 */

#include "rpc/rpc_method_builder.h"
#include "p2p/escrow_manager.h"
#include "common/logger.h"
#include <iostream>

namespace din {
namespace rpc {

// Implementation functions from methods_contract.cpp
extern din::Json contract_createescrow_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_status_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_list_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_release_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_refund_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_broadcastrelease_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_broadcastrefund_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_setlocktx_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_getsighash_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json mediator_getpubkey_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json contract_requestmediation_impl(const ExecutionContext& ctx, const din::Json& params);

void registerContractMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // ESCROW CONTRACT CREATION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("contract.createescrow", "contract")
        .description("Creates a new escrow contract (2-of-2, 2-of-3 auto, or 2-of-3 manual)")
        .param("buyer_pubkey", "string", "Buyer's public key (hex)", true)
        .param("seller_pubkey", "string", "Seller's public key (hex)", true)
        .param("amount", "number", "Escrow amount in DIN", true)
        .param("type", "string", "Contract type: 2of2, auto, or manual (default: manual)", false)
        .param("mediator_pubkey", "string", "Mediator's public key (required for manual type)", false)
        .param("refund_blocks", "number", "Blocks until automatic refund (default: 2880)", false)
        .param("seller_window_blocks", "number", "Blocks until daemon favors seller for auto type (default: 6)", false)
        .result("object", "Escrow contract with address, redeemScript, and funding info")
        .handler(contract_createescrow_impl)
        .examples({
            "contract.createescrow {\"buyer_pubkey\":\"03abc...\",\"seller_pubkey\":\"03def...\",\"amount\":100.0,\"type\":\"auto\"}",
            "contract.createescrow {\"buyer_pubkey\":\"03abc...\",\"seller_pubkey\":\"03def...\",\"amount\":50.0,\"type\":\"2of2\"}",
            "contract.createescrow {\"buyer_pubkey\":\"03abc...\",\"seller_pubkey\":\"03def...\",\"mediator_pubkey\":\"03ghi...\",\"amount\":100.0,\"type\":\"manual\"}"
        });

    // ═══════════════════════════════════════════════════════════════
    // ESCROW MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("contract.status", "contract")
        .description("Gets the status of an escrow contract")
        .param("escrow_address", "string", "Escrow contract address", true)
        .result("object", "Contract status with state, balance, and parties")
        .handler(contract_status_impl)
        .examples({
            "contract.status \"din1q...\""
        });

    RPC_METHOD("contract.list", "contract")
        .description("Lists all escrow contracts for this wallet")
        .param("filter", "string", "Filter: all, active, completed, expired (default: active)", false)
        .result("array", "Array of escrow contract objects")
        .handler(contract_list_impl)
        .examples({
            "contract.list",
            "contract.list active",
            "contract.list all"
        });

    // ═══════════════════════════════════════════════════════════════
    // ESCROW RELEASE & REFUND
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("contract.release", "contract")
        .description("Creates a release transaction (buyer + seller or buyer + mediator)")
        .param("escrow_address", "string", "Escrow contract address", true)
        .param("destination", "string", "Seller's receiving address", true)
        .result("object", "Partially signed release transaction")
        .handler(contract_release_impl)
        .examples({
            "contract.release \"din1q_escrow...\" \"din1q_seller...\""
        });

    RPC_METHOD("contract.refund", "contract")
        .description("Creates a refund transaction (buyer + mediator or timeout)")
        .param("escrow_address", "string", "Escrow contract address", true)
        .param("destination", "string", "Buyer's receiving address", true)
        .result("object", "Partially signed refund transaction")
        .handler(contract_refund_impl)
        .examples({
            "contract.refund \"din1q_escrow...\" \"din1q_buyer...\""
        });

    RPC_METHOD("contract.broadcastrelease", "contract")
        .description("Broadcasts a fully signed release transaction")
        .param("signed_tx", "string", "Fully signed release transaction hex", true)
        .result("string", "Transaction ID")
        .handler(contract_broadcastrelease_impl)
        .examples({
            "contract.broadcastrelease \"0100000001...\""
        });

    RPC_METHOD("contract.broadcastrefund", "contract")
        .description("Broadcasts a fully signed refund transaction")
        .param("signed_tx", "string", "Fully signed refund transaction hex", true)
        .result("string", "Transaction ID")
        .handler(contract_broadcastrefund_impl)
        .examples({
            "contract.broadcastrefund \"0100000001...\""
        });

    // ═══════════════════════════════════════════════════════════════
    // ADVANCED CONTRACT OPERATIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("contract.setlocktx", "contract")
        .description("Sets the funding transaction for an escrow contract")
        .param("escrow_address", "string", "Escrow contract address", true)
        .param("funding_txid", "string", "Funding transaction ID", true)
        .param("vout", "number", "Output index", true)
        .result("object", "Updated contract state")
        .handler(contract_setlocktx_impl)
        .examples({
            "contract.setlocktx \"din1q_escrow...\" \"abc123...\" 0"
        });

    RPC_METHOD("contract.getsighash", "contract")
        .description("Gets the signature hash for signing an escrow transaction")
        .param("escrow_address", "string", "Escrow contract address", true)
        .param("tx_type", "string", "Transaction type: release or refund", true)
        .result("string", "Signature hash (hex)")
        .handler(contract_getsighash_impl)
        .examples({
            "contract.getsighash \"din1q_escrow...\" release",
            "contract.getsighash \"din1q_escrow...\" refund"
        });

    // ═══════════════════════════════════════════════════════════════
    // DAEMON MEDIATOR (Automatic Escrow Resolution)
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("mediator.getpubkey", "contract")
        .description("Gets the daemon's mediator public key for auto-mediated escrows")
        .result("string", "Mediator public key (hex)")
        .handler(mediator_getpubkey_impl)
        .examples({
            "mediator.getpubkey"
        });

    RPC_METHOD("contract.requestmediation", "contract")
        .description("Request daemon mediator signature for escrow release/refund")
        .param("contract_id", "string", "Contract ID", true)
        .param("favor_seller", "boolean", "true = sign for seller, false = sign for buyer", true)
        .param("tx_hash", "string", "Transaction hash to sign", true)
        .param("input_index", "number", "Input index to sign", true)
        .result("object", "Signature and mediator decision")
        .handler(contract_requestmediation_impl)
        .examples({
            "contract.requestmediation \"contract_abc123\" true \"tx_hash\" 0",
            "contract.requestmediation \"contract_abc123\" false \"tx_hash\" 0"
        });

    std::cout << "[Contract RPC vNext] ✅ Registered 11 contract methods (+ 2 mediator methods)" << std::endl;
}

} // namespace rpc
} // namespace din

// Auto-register at startup
static auto _contract_vnext_init = (din::rpc::registerContractMethodsVNext(), 0);
