#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <optional>
#include <cstdint>

namespace dinero {

// Forward declarations
class ILogger;  // Forward declaration for dependency injection

namespace p2p {

/**
 * OfferType - Buy or Sell offer
 */
enum class OfferType {
    BUY,   // Buyer wants to buy DIN
    SELL   // Seller wants to sell DIN
};

/**
 * OfferStatus - Current state of an offer
 */
enum class OfferStatus {
    ACTIVE,      // Offer is live and accepting
    ACCEPTED,    // Offer has been accepted
    COMPLETED,   // Trade completed successfully
    CANCELLED,   // Offer cancelled by creator
    EXPIRED      // Offer expired (time or escrow expiry)
};

/**
 * P2POffer - Complete P2P marketplace offer
 */
struct P2POffer {
    std::string offer_id;                    // Unique offer identifier
    OfferType type;                          // BUY or SELL
    std::string creator_address;             // Wallet address of creator
    std::string creator_pubkey;              // Public key for verification
    double amount;                           // DIN amount
    double price_usd;                        // Price per DIN in USD
    std::vector<std::string> payment_methods; // e.g., ["ApplePay", "Zelle"]
    std::string escrow_id;                   // Associated escrow ID (if sell offer)
    std::string escrow_txid;                 // Escrow lock transaction ID
    uint64_t created_at;                     // Unix timestamp
    uint64_t expires_at;                     // Unix timestamp
    OfferStatus status;                      // Current status
    std::string acceptor_address;            // Address of acceptor (if accepted)
    std::string notes;                       // Optional notes from creator
    double min_amount;                       // Minimum trade amount (optional)
    double max_amount;                       // Maximum trade amount (optional)
    int reputation_required;                 // Minimum reputation to accept (0 = none)

    // Calculate total value
    double totalValue() const {
        return amount * price_usd;
    }

    // Check if offer is active
    bool isActive() const {
        return status == OfferStatus::ACTIVE;
    }

    // Check if expired
    bool isExpired(uint64_t current_time) const {
        return current_time >= expires_at;
    }
};

/**
 * P2POfferRegistry - Manages all P2P offers
 *
 * Features:
 * - Store and retrieve offers
 * - Filter offers by type, price, payment method
 * - Match buyers and sellers
 * - Track offer lifecycle
 * - Broadcast new offers via WebSocket
 */
class P2POfferRegistry {
public:
    static P2POfferRegistry& instance();

    /**
     * Add a new offer to the registry
     *
     * @param offer P2P offer to add
     * @return true if added successfully
     */
    bool addOffer(const P2POffer& offer);

    /**
     * Get a specific offer by ID
     *
     * @param offer_id Unique offer identifier
     * @return P2POffer, or nullopt if not found
     */
    std::optional<P2POffer> getOffer(const std::string& offer_id);

    /**
     * List all active offers
     *
     * @param type Optional filter by offer type (BUY/SELL)
     * @param max_count Maximum number of offers to return (0 = all)
     * @return Vector of active offers
     */
    std::vector<P2POffer> listOffers(
        std::optional<OfferType> type = std::nullopt,
        int max_count = 0
    );

    /**
     * Find offers matching criteria
     *
     * @param type BUY or SELL
     * @param min_price Minimum price per DIN
     * @param max_price Maximum price per DIN
     * @param payment_method Required payment method (empty = any)
     * @return Vector of matching offers
     */
    std::vector<P2POffer> findOffers(
        OfferType type,
        double min_price,
        double max_price,
        const std::string& payment_method = ""
    );

    /**
     * Accept an offer
     *
     * @param offer_id Unique offer identifier
     * @param acceptor_address Address of acceptor
     * @return true if accepted successfully
     */
    bool acceptOffer(const std::string& offer_id, const std::string& acceptor_address);

    /**
     * Cancel an offer
     *
     * @param offer_id Unique offer identifier
     * @param creator_address Address of creator (for verification)
     * @return true if cancelled successfully
     */
    bool cancelOffer(const std::string& offer_id, const std::string& creator_address);

    /**
     * Complete an offer (mark as done)
     *
     * @param offer_id Unique offer identifier
     * @return true if completed successfully
     */
    bool completeOffer(const std::string& offer_id);

    /**
     * Process expired offers
     * Should be called periodically by daemon
     */
    void processExpiredOffers();

    /**
     * Get total number of active offers
     *
     * @return Count of active offers
     */
    int getActiveOfferCount();

    /**
     * Get best buy/sell offers (sorted by price)
     *
     * @param type BUY or SELL
     * @param limit Maximum number to return
     * @return Vector of best offers
     */
    std::vector<P2POffer> getBestOffers(OfferType type, int limit = 10);

    /**
     * Set logger for dependency injection
     *
     * @param logger ILogger pointer
     */
    void setLogger(ILogger* logger) { logger_ = logger; }

private:
    P2POfferRegistry() = default;
    ~P2POfferRegistry() = default;

    std::map<std::string, P2POffer> offers_;  // offer_id → offer

    // Use function-local static for mutex to avoid static initialization order issues
    static std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }

    // Logger dependency injection
    ILogger* logger_ = nullptr;

    // Helper macros for cleaner DI logging
    #define P2PLOG_INFO(msg)  if (logger_) logger_->info(msg)
    #define P2PLOG_DEBUG(msg) if (logger_) logger_->debug(msg)
    #define P2PLOG_WARN(msg)  if (logger_) logger_->warning(msg)
    #define P2PLOG_ERR(msg)   if (logger_) logger_->error(msg)
};

} // namespace p2p
} // namespace dinero
