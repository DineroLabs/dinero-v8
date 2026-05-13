/**
 * Trade State Machine - Complete Fiat Payment Workflow
 *
 * Implements the full lifecycle of a P2P fiat trade:
 * 1. Created → Escrow funded
 * 2. Funded → Buyer sends fiat payment
 * 3. Payment Sent → Seller confirms receipt
 * 4. Confirmed → Escrow released
 * 5. Completed → Trade finalized with ratings
 *
 * Timeouts:
 * - Funding: 30 minutes
 * - Payment: 30-60 minutes (configurable)
 * - Confirmation: 2 hours
 */

#pragma once

#include "din_json.h"
#include "p2p/payment_encryption.h"
#include <string>
#include <optional>
#include <vector>

namespace din {
namespace p2p {

/**
 * Trade status enum - complete workflow
 */
enum class TradeStatus {
    CREATED,                // Trade created, waiting for escrow funding
    FUNDED,                 // Escrow funded, waiting for buyer to send fiat
    PAYMENT_SENT,           // Buyer claims fiat payment sent
    PAYMENT_CONFIRMED,      // Seller confirms fiat received
    RELEASING_ESCROW,       // Escrow release in progress
    COMPLETED,              // Trade completed successfully

    // Error states
    FUNDING_TIMEOUT,        // Escrow not funded in time
    PAYMENT_TIMEOUT,        // Buyer didn't send payment in time
    CONFIRMATION_TIMEOUT,   // Seller didn't confirm in time
    DISPUTED,               // Dispute opened by either party
    CANCELLED,              // Trade cancelled
    REFUNDED                // Escrow refunded
};

/**
 * Proof of payment (screenshot, receipt, etc.)
 */
struct PaymentProof {
    std::string proof_type;             // "screenshot", "receipt", "transaction_id"
    std::vector<uint8_t> data;          // Encrypted proof data
    std::string data_hash;              // SHA-256 hash
    int64_t uploaded_at;                // Timestamp
    std::string uploader_pubkey;        // Who uploaded it

    Json toJson() const;
    static PaymentProof fromJson(const Json& j);
};

/**
 * Enhanced trade structure with complete workflow
 */
struct EnhancedTrade {
    // Basic trade info (from existing Trade struct)
    std::string trade_id;
    std::string offer_id;
    std::string buyer_pubkey;
    std::string seller_pubkey;
    std::string mediator_pubkey;
    double amount_din;
    double price_per_din;
    double total_fiat;
    std::string fiat_currency;

    // Escrow info
    std::string contract_id;
    std::string escrow_address;
    std::string escrow_txid;            // Funding transaction

    // Payment method & details
    std::string payment_method_id;      // e.g., "zelle"
    EncryptedPaymentHandle payment_handle;  // Encrypted seller payment details
    std::string payment_instructions;   // Generated instructions for buyer

    // State & timestamps
    TradeStatus status;
    int64_t created_at;
    int64_t funded_at;
    int64_t payment_sent_at;
    int64_t payment_confirmed_at;
    int64_t completed_at;

    // Timeouts (in seconds)
    int funding_timeout;                // Default: 1800 (30 minutes)
    int payment_timeout;                // Default: 2400 (40 minutes)
    int confirmation_timeout;           // Default: 7200 (2 hours)

    // Payment proof
    std::vector<PaymentProof> proofs;

    // Dispute info
    bool disputed;
    std::string dispute_reason;
    int64_t disputed_at;
    std::string dispute_resolution;

    // Ratings
    int buyer_rating;                   // 1-5 stars (0 = not rated)
    int seller_rating;
    std::string buyer_review;
    std::string seller_review;

    // Fees
    double marketplace_fee;             // 0.5% of amount_din

    Json toJson() const;
    static EnhancedTrade fromJson(const Json& j);

    // Helper methods
    bool isFundingExpired(int64_t current_time) const;
    bool isPaymentExpired(int64_t current_time) const;
    bool isConfirmationExpired(int64_t current_time) const;
    int64_t getRemainingTime(int64_t current_time) const;
    std::string getStatusString() const;
};

/**
 * TradeStateMachine - Manages trade lifecycle & transitions
 */
class TradeStateMachine {
public:
    /**
     * Create new trade from offer acceptance
     */
    static EnhancedTrade createTrade(
        const std::string& offer_id,
        const std::string& buyer_pubkey,
        const std::string& seller_pubkey,
        const std::string& mediator_pubkey,
        double amount_din,
        double price_per_din,
        const std::string& fiat_currency,
        const std::string& payment_method_id,
        const EncryptedPaymentHandle& payment_handle
    );

    /**
     * Transition: CREATED → FUNDED
     * Called when escrow receives funds
     */
    static bool markFunded(
        EnhancedTrade& trade,
        const std::string& escrow_txid
    );

    /**
     * Transition: FUNDED → PAYMENT_SENT
     * Called when buyer claims payment sent
     */
    static bool markPaymentSent(
        EnhancedTrade& trade,
        const std::vector<PaymentProof>& proofs = {}
    );

    /**
     * Transition: PAYMENT_SENT → PAYMENT_CONFIRMED
     * Called when seller confirms receipt
     */
    static bool markPaymentConfirmed(
        EnhancedTrade& trade
    );

    /**
     * Transition: PAYMENT_CONFIRMED → RELEASING_ESCROW → COMPLETED
     * Called to release escrow and finalize trade
     */
    static bool completeTrade(
        EnhancedTrade& trade,
        const std::string& release_txid,
        int buyer_rating = 0,
        int seller_rating = 0
    );

    /**
     * Open dispute
     */
    static bool openDispute(
        EnhancedTrade& trade,
        const std::string& reason,
        const std::string& initiator_pubkey
    );

    /**
     * Resolve dispute (mediator only)
     */
    static bool resolveDispute(
        EnhancedTrade& trade,
        const std::string& resolution,
        const std::string& mediator_pubkey,
        bool refund_buyer  // true = refund, false = release to seller
    );

    /**
     * Cancel trade
     */
    static bool cancelTrade(
        EnhancedTrade& trade,
        const std::string& reason
    );

    /**
     * Check and process timeouts
     * Should be called periodically by daemon
     */
    static void processTimeouts(
        std::vector<EnhancedTrade>& active_trades,
        int64_t current_time
    );

    /**
     * Validate state transition
     */
    static bool canTransition(
        TradeStatus from,
        TradeStatus to
    );

    /**
     * Get allowed next states
     */
    static std::vector<TradeStatus> getAllowedTransitions(TradeStatus current);

    /**
     * Calculate marketplace fee (0.5%)
     */
    static double calculateFee(double amount_din);
};

} // namespace p2p
} // namespace din
