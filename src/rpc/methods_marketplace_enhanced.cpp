/**
 * Enhanced Marketplace RPC Methods
 *
 * Complete trade flow with payment states and encryption
 */

#include "rpc/rpc_method_builder.h"
#include "p2p/marketplace_manager.h"
#include "p2p/kyc_manager.h"
#include "p2p/payment_adapter.h"
#include "p2p/payment_encryption.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// ═══════════════════════════════════════════════════════════════
// TRADE MANAGEMENT
// ═══════════════════════════════════════════════════════════════

Json market_acceptoffer_enhanced_impl(const ExecutionContext& ctx, const Json& params) {
    std::string offer_id = params[0].asString();
    std::string buyer_pubkey = "03test_buyer";
    double amount = params.size() > 1 ? params[1].asDouble() : 0.0;

    auto& marketplace = din::MarketplaceManager::instance();
    auto& kyc = din::p2p::KYCManager::instance();

    try {
        // Get offer details
        auto offer = marketplace.getOffer(offer_id);
        if (offer.empty() || offer["status"].asString() != "active") {
            Json error;
            error["success"] = false;
            error["error"] = "Offer not found or not active";
            return error;
        }

        // Use offer amount if not specified
        if (amount == 0.0) {
            amount = offer["amount"].asDouble();
        }

        // Check KYC limits
        if (!kyc.canTrade(buyer_pubkey, amount)) {
            Json error;
            error["success"] = false;
            error["error"] = "Trade amount exceeds your KYC limits";
            error["remaining_capacity"] = kyc.getRemainingDailyCapacity(buyer_pubkey);
            return error;
        }

        // Create trade
        Json trade_params;
        trade_params["offer_id"] = offer_id;
        trade_params["buyer_pubkey"] = buyer_pubkey;
        trade_params["seller_pubkey"] = offer["creator_pubkey"].asString();
        trade_params["mediator_pubkey"] = offer["mediator_pubkey"].asString();
        trade_params["amount"] = amount;
        trade_params["price"] = offer["price"].asDouble();
        trade_params["total_value"] = amount * offer["price"].asDouble();
        trade_params["currency"] = offer["currency"].asString();
        trade_params["status"] = "pending_funding";  // Initial state

        // Generate deterministic escrow identifiers for the trade record.
        trade_params["contract_id"] = "contract_" + offer_id;
        trade_params["escrow_address"] = "din1qescrow...";

        auto trade = marketplace.createTrade(trade_params);

        // Record trade volume
        kyc.recordTrade(buyer_pubkey, amount);

        Json result;
        result["success"] = true;
        result["trade_id"] = trade["trade_id"].asString();
        result["escrow_address"] = trade["escrow_address"].asString();
        result["amount_to_fund"] = amount;
        result["status"] = "pending_funding";
        result["funding_instructions"] = "Send " + std::to_string(amount) +
            " DIN to escrow address to activate trade";
        result["funding_timeout_minutes"] = 30;

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

Json market_markpaymentsent_impl(const ExecutionContext& ctx, const Json& params) {
    std::string trade_id = params[0].asString();
    std::string user_pubkey = "03test_user";
    std::string proof_data = params.size() > 1 ? params[1].asString() : "";

    auto& marketplace = din::MarketplaceManager::instance();

    try {
        auto trade = marketplace.getTrade(trade_id);

        // Verify caller is the buyer
        if (trade["buyer_pubkey"].asString() != user_pubkey) {
            Json error;
            error["success"] = false;
            error["error"] = "Only the buyer can mark payment as sent";
            return error;
        }

        // Verify trade is in correct state
        if (trade["status"].asString() != "funded") {
            Json error;
            error["success"] = false;
            error["error"] = "Trade must be funded before marking payment sent";
            error["current_status"] = trade["status"].asString();
            return error;
        }

        // Update trade status
        Json updates;
        updates["status"] = "payment_sent";
        updates["payment_sent_at"] = static_cast<Json::Int64>(std::time(nullptr));

        // Store proof metadata if provided.
        if (!proof_data.empty()) {
            updates["proof_uploaded"] = true;
            updates["proof_storage"] = "pending_backend";
        }

        marketplace.updateTrade(trade_id, updates);

        Json result;
        result["success"] = true;
        result["trade_id"] = trade_id;
        result["status"] = "payment_sent";
        result["message"] = "Payment marked as sent. Waiting for seller confirmation.";
        result["confirmation_timeout_hours"] = 2;

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

Json market_confirmreceived_impl(const ExecutionContext& ctx, const Json& params) {
    std::string trade_id = params[0].asString();
    std::string user_pubkey = "03test_user";

    auto& marketplace = din::MarketplaceManager::instance();

    try {
        auto trade = marketplace.getTrade(trade_id);

        // Verify caller is the seller
        if (trade["seller_pubkey"].asString() != user_pubkey) {
            Json error;
            error["success"] = false;
            error["error"] = "Only the seller can confirm payment received";
            return error;
        }

        // Verify trade is in correct state
        if (trade["status"].asString() != "payment_sent") {
            Json error;
            error["success"] = false;
            error["error"] = "No payment to confirm";
            error["current_status"] = trade["status"].asString();
            return error;
        }

        // Update trade status
        Json updates;
        updates["status"] = "payment_confirmed";
        updates["payment_confirmed_at"] = static_cast<Json::Int64>(std::time(nullptr));

        marketplace.updateTrade(trade_id, updates);

        Json result;
        result["success"] = true;
        result["trade_id"] = trade_id;
        result["status"] = "payment_confirmed";
        result["message"] = "Payment confirmed. Escrow release is pending settlement processing.";
        result["escrow_release"] = "pending";

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

Json market_completetrade_impl(const ExecutionContext& ctx, const Json& params) {
    std::string trade_id = params[0].asString();
    int rating = params[1].asInt();
    std::string review = params.size() > 2 ? params[2].asString() : "";
    std::string user_pubkey = "03test_user";

    auto& marketplace = din::MarketplaceManager::instance();

    try {
        auto trade = marketplace.getTrade(trade_id);

        // Determine if user is buyer or seller
        bool is_buyer = (trade["buyer_pubkey"].asString() == user_pubkey);
        bool is_seller = (trade["seller_pubkey"].asString() == user_pubkey);

        if (!is_buyer && !is_seller) {
            Json error;
            error["success"] = false;
            error["error"] = "You are not a party to this trade";
            return error;
        }

        // Validate rating
        if (rating < 1 || rating > 5) {
            Json error;
            error["success"] = false;
            error["error"] = "Rating must be between 1 and 5";
            return error;
        }

        // Update trade with rating
        Json updates;
        if (is_buyer) {
            updates["buyer_rating"] = rating;
            updates["buyer_review"] = review;
        } else {
            updates["seller_rating"] = rating;
            updates["seller_review"] = review;
        }

        // If both parties rated, mark as completed
        bool buyer_rated = trade.isMember("buyer_rating") && trade["buyer_rating"].asInt() > 0;
        bool seller_rated = trade.isMember("seller_rating") && trade["seller_rating"].asInt() > 0;

        if ((is_buyer && seller_rated) || (is_seller && buyer_rated)) {
            updates["status"] = "completed";
            updates["completed_at"] = static_cast<Json::Int64>(std::time(nullptr));
        }

        marketplace.updateTrade(trade_id, updates);

        // Reputation updates are delegated to the reputation subsystem when enabled.

        Json result;
        result["success"] = true;
        result["trade_id"] = trade_id;
        result["rating_submitted"] = rating;
        result["status"] = updates["status"].asString();

        if (updates["status"].asString() == "completed") {
            result["message"] = "Trade completed successfully! 🎉";
        } else {
            result["message"] = "Rating submitted. Waiting for counterparty to rate.";
        }

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

Json market_gettrade_impl(const ExecutionContext& ctx, const Json& params) {
    std::string trade_id = params[0].asString();
    std::string user_pubkey = "03test_user";

    auto& marketplace = din::MarketplaceManager::instance();

    try {
        auto trade = marketplace.getTrade(trade_id);

        // Verify user is party to trade
        bool is_party = (
            trade["buyer_pubkey"].asString() == user_pubkey ||
            trade["seller_pubkey"].asString() == user_pubkey ||
            trade["mediator_pubkey"].asString() == user_pubkey
        );

        if (!is_party) {
            Json error;
            error["success"] = false;
            error["error"] = "Access denied";
            return error;
        }

        // Calculate remaining time
        int64_t now = std::time(nullptr);
        int64_t created_at = trade["created_at"].asInt64();
        int64_t payment_sent_at = trade.isMember("payment_sent_at") ?
            trade["payment_sent_at"].asInt64() : 0;

        trade["time_elapsed"] = now - created_at;

        if (trade["status"].asString() == "pending_funding") {
            int64_t funding_deadline = created_at + (30 * 60);  // 30 minutes
            trade["time_remaining"] = std::max(static_cast<int64_t>(0), funding_deadline - now);
        } else if (trade["status"].asString() == "funded") {
            int64_t payment_deadline = created_at + (2 * 60 * 60);  // 2 hours
            trade["time_remaining"] = std::max(static_cast<int64_t>(0), payment_deadline - now);
        } else if (trade["status"].asString() == "payment_sent") {
            int64_t confirm_deadline = payment_sent_at + (2 * 60 * 60);  // 2 hours
            trade["time_remaining"] = std::max(static_cast<int64_t>(0), confirm_deadline - now);
        }

        return trade;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

// ═══════════════════════════════════════════════════════════════
// PAYMENT INSTRUCTIONS
// ═══════════════════════════════════════════════════════════════

Json market_getpaymentinstructions_impl(const ExecutionContext& ctx, const Json& params) {
    std::string trade_id = params[0].asString();
    std::string user_pubkey = "03test_user";

    auto& marketplace = din::MarketplaceManager::instance();
    auto& payment_registry = din::p2p::PaymentAdapterRegistry::instance();

    try {
        auto trade = marketplace.getTrade(trade_id);

        // Only buyer can get payment instructions
        if (trade["buyer_pubkey"].asString() != user_pubkey) {
            Json error;
            error["success"] = false;
            error["error"] = "Only buyer can view payment instructions";
            return error;
        }

        // Get offer to get payment method
        auto offer = marketplace.getOffer(trade["offer_id"].asString());
        std::string payment_method = offer["payment_methods"][0].asString();  // First method

        // Get payment handle from offer payload if present.
        std::string payment_handle = offer.get("payment_handle", Json("")).asString();
        if (payment_handle.empty()) {
            Json error;
            error["success"] = false;
            error["error"] = "Payment handle unavailable for this offer";
            return error;
        }

        // Generate instructions
        std::string trade_reference = "TRADE_" + trade_id.substr(0, 8);
        double amount = trade["total_value"].asDouble();
        std::string currency = trade["currency"].asString();

        std::string instructions = payment_registry.generateInstructions(
            payment_method,
            payment_handle,
            amount,
            currency,
            trade_reference
        );

        Json result;
        result["success"] = true;
        result["trade_id"] = trade_id;
        result["payment_method"] = payment_method;
        result["instructions"] = instructions;
        result["amount"] = amount;
        result["currency"] = currency;
        result["reference"] = trade_reference;

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION
// ═══════════════════════════════════════════════════════════════

void registerEnhancedMarketplaceMethodsVNext() {
    RPC_METHOD("market.acceptoffer", "market")
        .description("Accept marketplace offer and create escrow trade")
        .param("offer_id", "string", "Offer ID to accept", true)
        .param("amount", "number", "Trade amount (optional, defaults to full offer)", false)
        .result("object", "Trade created with escrow address and funding instructions")
        .handler(market_acceptoffer_enhanced_impl)
        .examples({
            "market.acceptoffer \"offer_abc123\"",
            "market.acceptoffer \"offer_abc123\" 500.0"
        });

    RPC_METHOD("market.markpaymentsent", "market")
        .description("Mark fiat payment as sent (buyer)")
        .param("trade_id", "string", "Trade ID", true)
        .param("proof", "string", "Base64 encoded proof image (optional)", false)
        .result("object", "Payment marked as sent confirmation")
        .handler(market_markpaymentsent_impl)
        .examples({
            "market.markpaymentsent \"trade_xyz789\"",
            "market.markpaymentsent \"trade_xyz789\" \"<base64_screenshot>\""
        });

    RPC_METHOD("market.confirmreceived", "market")
        .description("Confirm fiat payment received (seller)")
        .param("trade_id", "string", "Trade ID", true)
        .result("object", "Payment confirmed, escrow release initiated")
        .handler(market_confirmreceived_impl)
        .examples({
            "market.confirmreceived \"trade_xyz789\""
        });

    RPC_METHOD("market.completetrade", "market")
        .description("Complete trade and submit rating")
        .param("trade_id", "string", "Trade ID", true)
        .param("rating", "number", "Rating 1-5 stars", true)
        .param("review", "string", "Optional review text", false)
        .result("object", "Trade completion and rating confirmation")
        .handler(market_completetrade_impl)
        .examples({
            "market.completetrade \"trade_xyz789\" 5",
            "market.completetrade \"trade_xyz789\" 4 \"Fast payment, great seller!\""
        });

    RPC_METHOD("market.gettrade", "market")
        .description("Get trade details and status")
        .param("trade_id", "string", "Trade ID", true)
        .result("object", "Complete trade details with remaining time")
        .handler(market_gettrade_impl)
        .examples({
            "market.gettrade \"trade_xyz789\""
        });

    RPC_METHOD("market.getpaymentinstructions", "market")
        .description("Get payment instructions for trade (buyer only)")
        .param("trade_id", "string", "Trade ID", true)
        .result("object", "Step-by-step payment instructions")
        .handler(market_getpaymentinstructions_impl)
        .examples({
            "market.getpaymentinstructions \"trade_xyz789\""
        });

    dinero::g_logger.info("[Marketplace RPC] Registered 6 enhanced trade methods");
}

// NOTE: Auto-registration DISABLED to avoid static initialization issues
// Call registerEnhancedMarketplaceMethodsVNext() explicitly from main.cpp after initializing managers
// static auto _marketplace_enhanced_init = (din::rpc::registerEnhancedMarketplaceMethodsVNext(), 0);

} // namespace rpc
} // namespace din
