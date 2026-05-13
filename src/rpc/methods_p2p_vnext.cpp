/**
 * P2P Marketplace RPC Methods - vNext Architecture
 *
 * Peer-to-peer marketplace with escrow and offer management.
 */

#include "rpc/rpc_method_builder.h"
#include "p2p/p2p_offer.h"
#include "p2p/escrow_manager.h"
#include "common/logger.h"
#include <iostream>

// Forward declarations for P2P implementation functions from methods_p2p.cpp
namespace din {
namespace rpc {
    extern Json p2p_createoffer_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_acceptoffer_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_canceloffer_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_completeoffer_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_getoffer_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_listoffers_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_bestoffers_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_verifyoffer_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_escrowinfo_impl(const ::ExecutionContext& ctx, const Json& params);
    extern Json p2p_releaseescrow_impl(const ::ExecutionContext& ctx, const Json& params);
} }

namespace din {
namespace rpc {

void registerP2PMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // P2P OFFER CREATION & MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("p2p.createoffer", "p2p")
        .description("Creates a new P2P buy/sell offer with escrow")
        .param("type", "string", "Offer type: buy or sell", true)
        .param("amount", "number", "Amount of DIN to buy/sell", true)
        .param("price_usd", "number", "Price per DIN in USD", true)
        .param("payment_methods", "array", "Accepted payment methods (e.g., [\"Bank Transfer\", \"PayPal\"])", true)
        .param("min_amount", "number", "Minimum trade amount (optional)", false)
        .param("max_amount", "number", "Maximum trade amount (optional)", false)
        .param("expiration_hours", "number", "Hours until offer expires (default: 24)", false)
        .param("notes", "string", "Additional notes for counterparty (optional)", false)
        .param("reputation_required", "number", "Minimum reputation score required (optional)", false)
        .result("object", "Created offer with offer_id, escrow details, and expiration")
        .handler(p2p_createoffer_impl)
        .examples({
            "p2p.createoffer '{\"type\":\"sell\",\"amount\":100,\"price_usd\":0.50,\"payment_methods\":[\"Bank Transfer\"]}'",
            "p2p.createoffer '{\"type\":\"buy\",\"amount\":50,\"price_usd\":0.48,\"payment_methods\":[\"PayPal\",\"Zelle\"],\"min_amount\":10}'"
        });

    RPC_METHOD("p2p.listoffers", "p2p")
        .description("Lists all active P2P offers (optionally filtered)")
        .param("filter", "object", "Filter criteria: type, min_amount, max_amount, payment_method (optional)", false)
        .result("array", "Array of offer objects with id, type, amount, price, and status")
        .handler(p2p_listoffers_impl)
        .examples({
            "p2p.listoffers",
            "p2p.listoffers '{\"type\":\"buy\"}'",
            "p2p.listoffers '{\"min_amount\":50,\"payment_method\":\"Bank Transfer\"}'"
        });

    RPC_METHOD("p2p.getoffer", "p2p")
        .description("Gets detailed information about a specific offer")
        .param("offer_id", "string", "Unique offer identifier", true)
        .result("object", "Complete offer details including escrow status and counterparty info")
        .handler(p2p_getoffer_impl)
        .examples({
            "p2p.getoffer \"offer_67890abc_1234\""
        });

    RPC_METHOD("p2p.bestoffers", "p2p")
        .description("Returns best buy and sell offers sorted by price")
        .param("limit", "number", "Maximum offers to return per type (default: 10)", false)
        .result("object", "Best buy and sell offers with prices and liquidity")
        .handler(p2p_bestoffers_impl)
        .examples({
            "p2p.bestoffers",
            "p2p.bestoffers 5"
        });

    // ═══════════════════════════════════════════════════════════════
    // P2P TRADE EXECUTION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("p2p.acceptoffer", "p2p")
        .description("Accepts a P2P offer and initiates escrow")
        .param("offer_id", "string", "Offer ID to accept", true)
        .param("amount", "number", "Amount to trade (must be within offer's min/max range)", true)
        .param("payment_details", "string", "Your payment details for the counterparty (optional)", false)
        .result("object", "Escrow transaction details and next steps")
        .handler(p2p_acceptoffer_impl)
        .examples({
            "p2p.acceptoffer \"offer_67890abc_1234\" 50",
            "p2p.acceptoffer \"offer_67890abc_1234\" 25 \"Bank: XYZ, Account: 123456\""
        });

    RPC_METHOD("p2p.completeoffer", "p2p")
        .description("Marks offer as completed after successful payment")
        .param("offer_id", "string", "Offer ID to complete", true)
        .param("proof", "string", "Payment proof or confirmation details (optional)", false)
        .result("object", "Completion status and escrow release transaction")
        .handler(p2p_completeoffer_impl)
        .examples({
            "p2p.completeoffer \"offer_67890abc_1234\"",
            "p2p.completeoffer \"offer_67890abc_1234\" \"TxID: abc123...\""
        });

    RPC_METHOD("p2p.canceloffer", "p2p")
        .description("Cancels an active P2P offer")
        .param("offer_id", "string", "Offer ID to cancel", true)
        .param("reason", "string", "Cancellation reason (optional)", false)
        .result("object", "Cancellation confirmation and refunded escrow")
        .handler(p2p_canceloffer_impl)
        .examples({
            "p2p.canceloffer \"offer_67890abc_1234\"",
            "p2p.canceloffer \"offer_67890abc_1234\" \"Price changed\""
        });

    RPC_METHOD("p2p.verifyoffer", "p2p")
        .description("Verifies offer authenticity and escrow funding")
        .param("offer_id", "string", "Offer ID to verify", true)
        .result("object", "Verification status including escrow balance and block confirmations")
        .handler(p2p_verifyoffer_impl)
        .examples({
            "p2p.verifyoffer \"offer_67890abc_1234\""
        });

    // ═══════════════════════════════════════════════════════════════
    // P2P ESCROW MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("p2p.escrowinfo", "p2p")
        .description("Gets escrow contract information for an offer")
        .param("offer_id", "string", "Offer ID", true)
        .result("object", "Escrow details including address, balance, parties, and timeout")
        .handler(p2p_escrowinfo_impl)
        .examples({
            "p2p.escrowinfo \"offer_67890abc_1234\""
        });

    RPC_METHOD("p2p.releaseescrow", "p2p")
        .description("Releases escrowed funds to counterparty (seller/buyer)")
        .param("offer_id", "string", "Offer ID", true)
        .param("destination", "string", "Recipient address", true)
        .param("signature", "string", "Your signature authorizing release", true)
        .result("object", "Release transaction with txid and confirmation status")
        .handler(p2p_releaseescrow_impl)
        .examples({
            "p2p.releaseescrow \"offer_67890abc_1234\" \"din1q...\" \"sig_abc123...\""
        });

    std::cout << "[P2P RPC vNext] ✅ Registered 10 P2P marketplace methods with full metadata" << std::endl;
}

} // namespace rpc
} // namespace din

// Registration is called explicitly from main.cpp
// No static initializer needed since we have explicit call in daemon startup
