/**
 * MultiAsset RPC Methods - vNext Architecture
 *
 * Cross-asset escrow and conversion methods with bridge integration.
 */

#include "rpc/rpc_method_builder.h"
#include "contracts/multiasset_escrow_contract.h"
#include "bridge/fiat_bridge_manager.h"
#include "common/logger.h"
#include <iostream>

namespace din {
namespace rpc {

// Legacy functions from methods_multiasset.cpp (without ExecutionContext)
extern din::Json multiasset_createescrow(const din::Json& params);
extern din::Json multiasset_releaseescrow(const din::Json& params);
extern din::Json multiasset_refundescrow(const din::Json& params);
extern din::Json multiasset_getcontract(const din::Json& params);
extern din::Json multiasset_listcontracts(const din::Json& params);
extern din::Json multiasset_getconversionroutes(const din::Json& params);
extern din::Json multiasset_estimateconversion(const din::Json& params);
extern din::Json multiasset_stats(const din::Json& params);
extern din::Json multiasset_supportedassets(const din::Json& params);

// Wrapper functions to add ExecutionContext support
din::Json multiasset_createescrow_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_createescrow(params);
}

din::Json multiasset_releaseescrow_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_releaseescrow(params);
}

din::Json multiasset_refundescrow_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_refundescrow(params);
}

din::Json multiasset_getcontract_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_getcontract(params);
}

din::Json multiasset_listcontracts_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_listcontracts(params);
}

din::Json multiasset_getconversionroutes_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_getconversionroutes(params);
}

din::Json multiasset_estimateconversion_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_estimateconversion(params);
}

din::Json multiasset_stats_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_stats(params);
}

din::Json multiasset_supportedassets_impl(const ExecutionContext& ctx, const din::Json& params) {
    return multiasset_supportedassets(params);
}

void registerMultiAssetMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // MULTI-ASSET ESCROW CREATION & MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("multiasset.createescrow", "multiasset")
        .description("Creates a multi-asset escrow contract with optional cross-chain conversion")
        .param("buyer_pubkey", "string", "Buyer's public key (hex)", true)
        .param("seller_pubkey", "string", "Seller's public key (hex)", true)
        .param("mediator_pubkey", "string", "Mediator's public key (hex)", true)
        .param("asset_id", "string", "Asset identifier (DIN, BTC, ETH, USDT, etc.)", true)
        .param("amount", "number", "Escrow amount in asset units", true)
        .param("refund_blocks", "number", "Blocks until automatic refund", true)
        .param("release_asset", "string", "Asset to convert to on release (optional for auto-conversion)", false)
        .result("object", "Escrow contract with contract_id, P2SH address, and conversion route")
        .handler(multiasset_createescrow_impl)
        .examples({
            "multiasset.createescrow '{\"buyer_pubkey\":\"03abc...\",\"seller_pubkey\":\"03def...\",\"mediator_pubkey\":\"03ghi...\",\"asset_id\":\"BTC\",\"amount\":0.5,\"refund_blocks\":1000}'",
            "multiasset.createescrow '{\"buyer_pubkey\":\"03abc...\",\"seller_pubkey\":\"03def...\",\"mediator_pubkey\":\"03ghi...\",\"asset_id\":\"USDT\",\"amount\":1000,\"refund_blocks\":500,\"release_asset\":\"DIN\"}'"
        });

    RPC_METHOD("multiasset.getcontract", "multiasset")
        .description("Gets detailed information about a multi-asset escrow contract")
        .param("contract_id", "string", "Unique contract identifier", true)
        .result("object", "Contract details including asset info, status, parties, and conversion route")
        .handler(multiasset_getcontract_impl)
        .examples({
            "multiasset.getcontract \"contract_abc123...\""
        });

    RPC_METHOD("multiasset.listcontracts", "multiasset")
        .description("Lists all multi-asset escrow contracts")
        .param("filter", "object", "Filter by asset_id, status, or party address (optional)", false)
        .result("array", "Array of contract objects")
        .handler(multiasset_listcontracts_impl)
        .examples({
            "multiasset.listcontracts",
            "multiasset.listcontracts '{\"asset_id\":\"BTC\"}'",
            "multiasset.listcontracts '{\"status\":\"active\"}'"
        });

    // ═══════════════════════════════════════════════════════════════
    // ESCROW RELEASE & REFUND WITH CONVERSION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("multiasset.releaseescrow", "multiasset")
        .description("Releases multi-asset escrow to seller with optional cross-chain conversion")
        .param("contract_id", "string", "Contract ID", true)
        .param("to_address", "string", "Seller's receiving address", true)
        .param("sig_buyer", "string", "Buyer's signature", true)
        .param("sig_seller", "string", "Seller's signature (or mediator if dispute)", true)
        .result("object", "Release transaction with txid and conversion details if applicable")
        .handler(multiasset_releaseescrow_impl)
        .examples({
            "multiasset.releaseescrow \"contract_abc123...\" \"din1q...\" \"sig_buyer_abc...\" \"sig_seller_def...\""
        });

    RPC_METHOD("multiasset.refundescrow", "multiasset")
        .description("Refunds multi-asset escrow to buyer after timeout or dispute")
        .param("contract_id", "string", "Contract ID", true)
        .param("refund_address", "string", "Buyer's refund address", true)
        .param("sig_buyer", "string", "Buyer's signature", true)
        .result("object", "Refund transaction with txid")
        .handler(multiasset_refundescrow_impl)
        .examples({
            "multiasset.refundescrow \"contract_abc123...\" \"din1q...\" \"sig_abc123...\""
        });

    // ═══════════════════════════════════════════════════════════════
    // CROSS-ASSET CONVERSION ROUTING
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("multiasset.getconversionroutes", "multiasset")
        .description("Gets available conversion routes between assets")
        .param("from_asset", "string", "Source asset (DIN, BTC, USDT, etc.)", true)
        .param("to_asset", "string", "Target asset", true)
        .param("amount", "number", "Amount to convert (optional, for accurate routing)", false)
        .result("object", "Available routes with providers, rates, fees, and estimated output")
        .handler(multiasset_getconversionroutes_impl)
        .examples({
            "multiasset.getconversionroutes \"BTC\" \"DIN\"",
            "multiasset.getconversionroutes \"USDT\" \"DIN\" 1000"
        });

    RPC_METHOD("multiasset.estimateconversion", "multiasset")
        .description("Estimates conversion output for a given route")
        .param("from_asset", "string", "Source asset", true)
        .param("to_asset", "string", "Target asset", true)
        .param("amount", "number", "Input amount", true)
        .param("route", "string", "Preferred route or provider (optional)", false)
        .result("object", "Conversion estimate with output amount, fees, and exchange rates")
        .handler(multiasset_estimateconversion_impl)
        .examples({
            "multiasset.estimateconversion \"BTC\" \"DIN\" 0.1",
            "multiasset.estimateconversion \"USDT\" \"DIN\" 500 \"custodial\""
        });

    // ═══════════════════════════════════════════════════════════════
    // ASSET INFORMATION & STATISTICS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("multiasset.supportedassets", "multiasset")
        .description("Lists all supported assets for escrow and conversion")
        .params({})
        .result("object", "Supported assets with metadata: name, symbol, decimals, bridge providers")
        .handler(multiasset_supportedassets_impl)
        .examples({
            "multiasset.supportedassets"
        });

    RPC_METHOD("multiasset.stats", "multiasset")
        .description("Returns multi-asset system statistics")
        .params({})
        .result("object", "Stats including total contracts, locked value per asset, conversions executed")
        .handler(multiasset_stats_impl)
        .examples({
            "multiasset.stats"
        });

    std::cout << "[MultiAsset RPC vNext] ✅ Registered 9 multi-asset methods with full metadata" << std::endl;
}

} // namespace rpc
} // namespace din

// Auto-register at program startup
static auto _multiasset_vnext_init = (din::rpc::registerMultiAssetMethodsVNext(), 0);
