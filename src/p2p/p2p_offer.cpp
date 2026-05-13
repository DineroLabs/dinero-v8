#include "p2p/p2p_offer.h"
#include "common/logger.h"
#include "common/ilogger.h"
#include <algorithm>
#include <chrono>

namespace dinero {
namespace p2p {

P2POfferRegistry& P2POfferRegistry::instance() {
    static P2POfferRegistry instance;
    return instance;
}

bool P2POfferRegistry::addOffer(const P2POffer& offer) {
    std::lock_guard<std::mutex> lock(get_mutex());

    // Check if offer already exists
    if (offers_.find(offer.offer_id) != offers_.end()) {
        P2PLOG_ERR("[P2POfferRegistry] Offer already exists: " + offer.offer_id);
        return false;
    }

    offers_[offer.offer_id] = offer;

    P2PLOG_INFO("[P2POfferRegistry] New offer added: " + offer.offer_id);
    P2PLOG_INFO("[P2POfferRegistry]   Type: " +
        std::string(offer.type == OfferType::SELL ? "SELL" : "BUY"));
    P2PLOG_INFO("[P2POfferRegistry]   Amount: " + std::to_string(offer.amount) + " DIN");
    P2PLOG_INFO("[P2POfferRegistry]   Price: $" + std::to_string(offer.price_usd) + " USD/DIN");

    // TODO: Broadcast via WebSocket to all connected clients
    // ws_server->broadcast("p2p_new_offer", offer.toJson());

    return true;
}

std::optional<P2POffer> P2POfferRegistry::getOffer(const std::string& offer_id) {
    std::lock_guard<std::mutex> lock(get_mutex());

    auto it = offers_.find(offer_id);
    if (it == offers_.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::vector<P2POffer> P2POfferRegistry::listOffers(
    std::optional<OfferType> type,
    int max_count)
{
    std::lock_guard<std::mutex> lock(get_mutex());

    std::vector<P2POffer> result;

    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    for (const auto& [id, offer] : offers_) {
        // Skip expired or non-active offers
        if (!offer.isActive() || offer.isExpired(current_time)) {
            continue;
        }

        // Filter by type if specified
        if (type && offer.type != *type) {
            continue;
        }

        result.push_back(offer);

        if (max_count > 0 && static_cast<int>(result.size()) >= max_count) {
            break;
        }
    }

    return result;
}

std::vector<P2POffer> P2POfferRegistry::findOffers(
    OfferType type,
    double min_price,
    double max_price,
    const std::string& payment_method)
{
    std::lock_guard<std::mutex> lock(get_mutex());

    std::vector<P2POffer> result;

    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    for (const auto& [id, offer] : offers_) {
        // Skip non-matching type
        if (offer.type != type) {
            continue;
        }

        // Skip expired or non-active
        if (!offer.isActive() || offer.isExpired(current_time)) {
            continue;
        }

        // Price filter
        if (offer.price_usd < min_price || offer.price_usd > max_price) {
            continue;
        }

        // Payment method filter
        if (!payment_method.empty()) {
            bool has_method = false;
            for (const auto& method : offer.payment_methods) {
                if (method == payment_method) {
                    has_method = true;
                    break;
                }
            }
            if (!has_method) {
                continue;
            }
        }

        result.push_back(offer);
    }

    return result;
}

bool P2POfferRegistry::acceptOffer(
    const std::string& offer_id,
    const std::string& acceptor_address)
{
    std::lock_guard<std::mutex> lock(get_mutex());

    auto it = offers_.find(offer_id);
    if (it == offers_.end()) {
        P2PLOG_ERR("[P2POfferRegistry] Offer not found: " + offer_id);
        return false;
    }

    P2POffer& offer = it->second;

    // Validate offer is active
    if (offer.status != OfferStatus::ACTIVE) {
        P2PLOG_ERR("[P2POfferRegistry] Offer not active: " + offer_id);
        return false;
    }

    // Check not expired
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    if (offer.isExpired(current_time)) {
        P2PLOG_ERR("[P2POfferRegistry] Offer expired: " + offer_id);
        offer.status = OfferStatus::EXPIRED;
        return false;
    }

    // Accept offer
    offer.status = OfferStatus::ACCEPTED;
    offer.acceptor_address = acceptor_address;

    P2PLOG_INFO("[P2POfferRegistry] Offer accepted: " + offer_id);
    P2PLOG_INFO("[P2POfferRegistry]   Acceptor: " + acceptor_address);

    // TODO: Broadcast via WebSocket
    // ws_server->broadcast("p2p_offer_accepted", offer.toJson());

    return true;
}

bool P2POfferRegistry::cancelOffer(
    const std::string& offer_id,
    const std::string& creator_address)
{
    std::lock_guard<std::mutex> lock(get_mutex());

    auto it = offers_.find(offer_id);
    if (it == offers_.end()) {
        P2PLOG_ERR("[P2POfferRegistry] Offer not found: " + offer_id);
        return false;
    }

    P2POffer& offer = it->second;

    // Verify creator
    if (offer.creator_address != creator_address) {
        P2PLOG_ERR("[P2POfferRegistry] Unauthorized cancel attempt: " + offer_id);
        return false;
    }

    // Can only cancel active or accepted offers
    if (offer.status != OfferStatus::ACTIVE && offer.status != OfferStatus::ACCEPTED) {
        P2PLOG_ERR("[P2POfferRegistry] Cannot cancel offer in current state: " + offer_id);
        return false;
    }

    offer.status = OfferStatus::CANCELLED;

    P2PLOG_INFO("[P2POfferRegistry] Offer cancelled: " + offer_id);

    // TODO: If escrow exists, initiate refund
    // if (!offer.escrow_id.empty()) {
    //     EscrowManager::instance().refundEscrow(offer.escrow_id);
    // }

    return true;
}

bool P2POfferRegistry::completeOffer(const std::string& offer_id) {
    std::lock_guard<std::mutex> lock(get_mutex());

    auto it = offers_.find(offer_id);
    if (it == offers_.end()) {
        return false;
    }

    P2POffer& offer = it->second;

    if (offer.status != OfferStatus::ACCEPTED) {
        P2PLOG_ERR("[P2POfferRegistry] Cannot complete non-accepted offer: " + offer_id);
        return false;
    }

    offer.status = OfferStatus::COMPLETED;

    P2PLOG_INFO("[P2POfferRegistry] Offer completed: " + offer_id);

    return true;
}

void P2POfferRegistry::processExpiredOffers() {
    std::lock_guard<std::mutex> lock(get_mutex());

    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    int expired_count = 0;

    for (auto& [id, offer] : offers_) {
        if (offer.isActive() && offer.isExpired(current_time)) {
            offer.status = OfferStatus::EXPIRED;
            expired_count++;

            P2PLOG_INFO("[P2POfferRegistry] Offer expired: " + id);

            // TODO: Auto-refund escrow if exists
            // if (!offer.escrow_id.empty()) {
            //     EscrowManager::instance().refundEscrow(offer.escrow_id);
            // }
        }
    }

    if (expired_count > 0) {
        P2PLOG_INFO("[P2POfferRegistry] Processed " +
            std::to_string(expired_count) + " expired offers");
    }
}

int P2POfferRegistry::getActiveOfferCount() {
    std::lock_guard<std::mutex> lock(get_mutex());

    int count = 0;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    for (const auto& [id, offer] : offers_) {
        if (offer.isActive() && !offer.isExpired(current_time)) {
            count++;
        }
    }

    return count;
}

std::vector<P2POffer> P2POfferRegistry::getBestOffers(OfferType type, int limit) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::vector<P2POffer> result;

    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    // Collect active offers of matching type
    for (const auto& [id, offer] : offers_) {
        if (offer.type == type && offer.isActive() && !offer.isExpired(current_time)) {
            result.push_back(offer);
        }
    }

    // Sort by price
    if (type == OfferType::SELL) {
        // For SELL offers, lowest price first (best for buyers)
        std::sort(result.begin(), result.end(),
            [](const P2POffer& a, const P2POffer& b) {
                return a.price_usd < b.price_usd;
            });
    } else {
        // For BUY offers, highest price first (best for sellers)
        std::sort(result.begin(), result.end(),
            [](const P2POffer& a, const P2POffer& b) {
                return a.price_usd > b.price_usd;
            });
    }

    // Limit results
    if (limit > 0 && static_cast<int>(result.size()) > limit) {
        result.resize(limit);
    }

    return result;
}

} // namespace p2p
} // namespace dinero
