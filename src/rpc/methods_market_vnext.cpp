/**
 * Marketplace RPC Methods - vNext Architecture
 *
 * P2P marketplace for trading DIN, goods, and services with integrated escrow.
 */

#include "rpc/rpc_method_builder.h"
#include "p2p/marketplace_manager.h"
#include "p2p/escrow_manager.h"
#include "common/logger.h"
#include <iostream>

namespace din {
namespace rpc {

// Forward declarations for implementation functions
extern din::Json market_createoffer_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_canceloffer_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_updateoffer_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_listoffers_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_getoffer_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_search_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_acceptoffer_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_completetrade_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_disputetrade_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_getreputation_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_myoffers_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json market_mytrades_impl(const ExecutionContext& ctx, const din::Json& params);

void registerMarketplaceMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // MARKETPLACE OFFER MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("market.createoffer", "market")
        .description("Creates a new marketplace offer (buy/sell DIN, goods, services)")
        .param("type", "string", "Offer type: 'buy' or 'sell'", true)
        .param("asset", "string", "Asset being traded (DIN, BTC, USDT, goods, services)", true)
        .param("amount", "number", "Amount of asset", true)
        .param("price", "number", "Price per unit in base currency", true)
        .param("currency", "string", "Price currency (DIN, USD, BTC, etc.)", true)
        .param("description", "string", "Offer description", true)
        .param("mediator_pubkey", "string", "Optional mediator public key (hex)", false)
        .param("min_trade", "number", "Minimum trade amount (optional)", false)
        .param("max_trade", "number", "Maximum trade amount (optional)", false)
        .param("payment_methods", "array", "Accepted payment methods (optional)", false)
        .param("delivery_time", "number", "Estimated delivery time in blocks (optional)", false)
        .result("object", "Created offer with offer_id, escrow details")
        .handler(market_createoffer_impl)
        .examples({
            R"(market.createoffer "sell" "DIN" 1000.0 0.15 "USD" "Selling 1000 DIN")",
            R"(market.createoffer "buy" "services" 1.0 50.0 "DIN" "Need web design services" "03abc...")"
        });

    RPC_METHOD("market.canceloffer", "market")
        .description("Cancels an active marketplace offer")
        .param("offer_id", "string", "Offer ID to cancel", true)
        .result("object", "Cancellation status")
        .handler(market_canceloffer_impl)
        .examples({
            R"(market.canceloffer "offer_abc123")"
        });

    RPC_METHOD("market.updateoffer", "market")
        .description("Updates marketplace offer details (price, amount, description)")
        .param("offer_id", "string", "Offer ID to update", true)
        .param("price", "number", "New price (optional)", false)
        .param("amount", "number", "New amount (optional)", false)
        .param("description", "string", "New description (optional)", false)
        .result("object", "Updated offer object")
        .handler(market_updateoffer_impl)
        .examples({
            R"(market.updateoffer "offer_abc123" 0.14 900.0)",
            R"(market.updateoffer "offer_abc123" null null "Updated description")"
        });

    // ═══════════════════════════════════════════════════════════════
    // MARKETPLACE BROWSING
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("market.listoffers", "market")
        .description("Lists all active marketplace offers with filtering")
        .param("type", "string", "Filter by type: 'buy', 'sell', 'all' (default: all)", false)
        .param("asset", "string", "Filter by asset (optional)", false)
        .param("min_price", "number", "Minimum price filter (optional)", false)
        .param("max_price", "number", "Maximum price filter (optional)", false)
        .param("limit", "number", "Maximum results to return (default: 50)", false)
        .param("offset", "number", "Results offset for pagination (default: 0)", false)
        .result("array", "Array of offer objects with seller reputation")
        .handler(market_listoffers_impl)
        .examples({
            R"(market.listoffers)",
            R"(market.listoffers "sell" "DIN" 0.10 0.20 100 0)"
        });

    RPC_METHOD("market.getoffer", "market")
        .description("Gets full details of a specific marketplace offer")
        .param("offer_id", "string", "Offer ID", true)
        .result("object", "Complete offer details with seller history and escrow info")
        .handler(market_getoffer_impl)
        .examples({
            R"(market.getoffer "offer_abc123")"
        });

    RPC_METHOD("market.search", "market")
        .description("Searches marketplace offers by keyword, asset, and price range")
        .param("query", "string", "Search query (optional)", false)
        .param("asset", "string", "Filter by asset (optional)", false)
        .param("min_price", "number", "Minimum price (optional)", false)
        .param("max_price", "number", "Maximum price (optional)", false)
        .param("sort_by", "string", "Sort: 'price', 'amount', 'reputation', 'date' (default: date)", false)
        .param("sort_order", "string", "Order: 'asc' or 'desc' (default: desc)", false)
        .result("array", "Matching offers sorted by criteria")
        .handler(market_search_impl)
        .examples({
            R"(market.search "bitcoin")",
            R"(market.search "" "DIN" 0.10 0.20 "price" "asc")"
        });

    // ═══════════════════════════════════════════════════════════════
    // MARKETPLACE TRANSACTIONS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("market.acceptoffer", "market")
        .description("Accepts a marketplace offer and automatically creates escrow contract")
        .param("offer_id", "string", "Offer ID to accept", true)
        .param("buyer_pubkey", "string", "Buyer's public key (hex)", true)
        .param("amount", "number", "Trade amount (optional, defaults to full offer)", false)
        .result("object", "Trade details with contract_id, escrow_address, funding instructions")
        .handler(market_acceptoffer_impl)
        .examples({
            R"(market.acceptoffer "offer_abc123" "03buyer...")",
            R"(market.acceptoffer "offer_abc123" "03buyer..." 500.0)"
        });

    RPC_METHOD("market.completetrade", "market")
        .description("Marks trade as complete, triggers escrow release, and records rating")
        .param("trade_id", "string", "Trade ID", true)
        .param("rating", "number", "Rating 1-5 stars", true)
        .param("review_text", "string", "Optional review text", false)
        .result("object", "Completion status with release_tx_id and reputation update")
        .handler(market_completetrade_impl)
        .examples({
            R"(market.completetrade "trade_xyz789" 5)",
            R"(market.completetrade "trade_xyz789" 4 "Fast delivery, great seller!")"
        });

    RPC_METHOD("market.disputetrade", "market")
        .description("Opens a dispute for a trade and involves the mediator")
        .param("trade_id", "string", "Trade ID", true)
        .param("reason", "string", "Dispute reason", true)
        .param("evidence", "string", "Evidence or description (optional)", false)
        .result("object", "Dispute details with dispute_id and mediator contact")
        .handler(market_disputetrade_impl)
        .examples({
            R"(market.disputetrade "trade_xyz789" "Item not received")",
            R"(market.disputetrade "trade_xyz789" "Wrong item delivered" "Photos attached...")"
        });

    // ═══════════════════════════════════════════════════════════════
    // REPUTATION & HISTORY
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("market.getreputation", "market")
        .description("Gets reputation score and trade history for a user")
        .param("identifier", "string", "User public key or address", true)
        .result("object", "Reputation score, trade statistics, and recent ratings")
        .handler(market_getreputation_impl)
        .examples({
            R"(market.getreputation "03abc...")",
            R"(market.getreputation "din1q...")"
        });

    RPC_METHOD("market.myoffers", "market")
        .description("Lists all offers created by this wallet")
        .param("status", "string", "Filter: 'active', 'completed', 'cancelled', 'all' (default: active)", false)
        .result("array", "Array of user's offers with trade statistics")
        .handler(market_myoffers_impl)
        .examples({
            R"(market.myoffers)",
            R"(market.myoffers "all")"
        });

    RPC_METHOD("market.mytrades", "market")
        .description("Lists all trades where wallet is buyer or seller")
        .param("role", "string", "Filter: 'buyer', 'seller', 'all' (default: all)", false)
        .param("status", "string", "Filter: 'active', 'completed', 'disputed', 'all' (default: all)", false)
        .result("array", "Array of trades with escrow status and counterparty info")
        .handler(market_mytrades_impl)
        .examples({
            R"(market.mytrades)",
            R"(market.mytrades "buyer" "active")",
            R"(market.mytrades "seller" "completed")"
        });

    // Marketplace methods registered successfully
}

// Auto-register at startup
static auto _market_vnext_init = (din::rpc::registerMarketplaceMethodsVNext(), 0);

} // namespace rpc
} // namespace din
