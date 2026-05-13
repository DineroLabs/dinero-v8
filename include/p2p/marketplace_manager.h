/**
 * Marketplace Manager - P2P Trading Platform
 *
 * Manages marketplace offers, trades, reputation, and integrates with escrow system.
 */

#pragma once

#include "din_json.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>

namespace din {

/**
 * Marketplace offer structure
 */
struct MarketplaceOffer {
    std::string offer_id;
    std::string type;  // "buy" or "sell"
    std::string asset;  // "DIN", "BTC", "services", etc.
    double amount;
    double price;
    std::string currency;  // "DIN", "USD", "BTC", etc.
    std::string description;
    std::string creator_pubkey;
    std::string mediator_pubkey;
    double min_trade;
    double max_trade;
    std::vector<std::string> payment_methods;
    int64_t delivery_time;  // in blocks
    std::string status;  // "active", "filled", "cancelled", "expired"
    int64_t created_at;
    int64_t updated_at;

    Json toJson() const;
    static MarketplaceOffer fromJson(const Json& j);
};

/**
 * Trade structure (links offer to escrow contract)
 */
struct Trade {
    std::string trade_id;
    std::string offer_id;
    std::string buyer_pubkey;
    std::string seller_pubkey;
    std::string mediator_pubkey;
    double amount;
    double price;
    double total_value;
    std::string currency;
    std::string contract_id;  // Escrow contract ID
    std::string escrow_address;
    std::string status;  // "pending_funding", "funded", "in_progress", "completed", "disputed", "refunded"
    int64_t created_at;
    int64_t completed_at;
    int rating;  // 1-5 stars
    std::string review;

    Json toJson() const;
    static Trade fromJson(const Json& j);
};

/**
 * Reputation structure
 */
struct Reputation {
    std::string user_pubkey;
    int total_trades;
    int successful_trades;
    int disputed_trades;
    double average_rating;
    std::vector<int> rating_distribution;  // [1star_count, 2star, 3star, 4star, 5star]
    int64_t first_trade_date;
    int64_t last_trade_date;

    Json toJson() const;
    static Reputation fromJson(const Json& j);
};

/**
 * Dispute structure
 */
struct Dispute {
    std::string dispute_id;
    std::string trade_id;
    std::string complainant_pubkey;
    std::string reason;
    std::string evidence;
    std::string mediator_pubkey;
    std::string resolution;
    std::string status;  // "open", "investigating", "resolved"
    int64_t created_at;
    int64_t resolved_at;

    Json toJson() const;
    static Dispute fromJson(const Json& j);
};

/**
 * MarketplaceManager - Singleton manager for P2P marketplace
 */
class MarketplaceManager {
public:
    static MarketplaceManager& instance();

    // Offer management
    Json createOffer(const Json& params);
    bool cancelOffer(const std::string& offer_id, const std::string& creator_pubkey);
    Json updateOffer(const std::string& offer_id, const Json& updates, const std::string& creator_pubkey);
    Json getOffer(const std::string& offer_id);
    std::vector<Json> listOffers(const std::string& type, const std::string& asset,
                                  double min_price, double max_price, int limit, int offset);
    std::vector<Json> search(const std::string& query, const std::string& asset,
                             double min_price, double max_price,
                             const std::string& sort_by, const std::string& sort_order);
    std::vector<Json> getOffersByCreator(const std::string& creator_pubkey, const std::string& status);

    // Trade management
    Json createTrade(const Json& params);
    Json getTrade(const std::string& trade_id);
    bool updateTrade(const std::string& trade_id, const Json& updates);
    std::vector<Json> getTradesByUser(const std::string& user_pubkey, const std::string& role, const std::string& status);

    // Reputation
    Json getReputation(const std::string& user_pubkey);
    void addRating(const std::string& rated_pubkey, int rating, const std::string& review, const std::string& rater_pubkey);

    // Disputes
    Json createDispute(const Json& params);
    Json getDispute(const std::string& dispute_id);
    bool resolveDispute(const std::string& dispute_id, const std::string& resolution, const std::string& mediator_pubkey);

    // Persistence
    void save();
    void load();
    void setDataDir(const std::string& data_dir);

    ~MarketplaceManager();

private:
    MarketplaceManager();

    MarketplaceManager(const MarketplaceManager&) = delete;
    MarketplaceManager& operator=(const MarketplaceManager&) = delete;

    std::string generateOfferId();
    std::string generateTradeId();
    std::string generateDisputeId();

    void calculateReputation(const std::string& user_pubkey);

    std::map<std::string, MarketplaceOffer> offers_;
    std::map<std::string, Trade> trades_;
    std::map<std::string, Reputation> reputations_;
    std::map<std::string, Dispute> disputes_;

    std::string data_dir_;

    // Use function-local static for mutex to avoid static initialization order issues
    static std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }

    static std::unique_ptr<MarketplaceManager> instance_;
    static std::once_flag init_flag_;
};

} // namespace din
