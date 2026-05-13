/**
 * KYC (Know Your Customer) Manager
 *
 * Implements tiered verification system:
 * - Tier 0: Unverified (10-1000 DIN per trade)
 * - Tier 1: Light KYC - Email/Phone verified (10-10000 DIN)
 * - Tier 2: Full KYC - ID verified (10-100000 DIN)
 */

#pragma once

#include "din_json.h"
#include <string>
#include <map>
#include <mutex>
#include <optional>

namespace din {
namespace p2p {

/**
 * KYC Verification Tier
 */
enum class KYCTier {
    UNVERIFIED = 0,    // No verification - basic limits
    LIGHT_KYC = 1,     // Email + Phone verified
    FULL_KYC = 2       // Government ID verified
};

/**
 * Verification status for email/phone/ID
 */
enum class VerificationStatus {
    PENDING,           // Verification requested but not completed
    VERIFIED,          // Successfully verified
    REJECTED,          // Verification failed
    EXPIRED            // Verification expired (needs re-verification)
};

/**
 * Trade limits for each tier
 */
struct TradeLimits {
    double min_trade_din;                // Minimum trade size
    double max_trade_din;                // Maximum single trade
    double daily_volume_din;             // Max 24h volume
    double monthly_volume_din;           // Max 30-day volume
    int max_active_trades;               // Max concurrent trades

    Json toJson() const;
    static TradeLimits fromJson(const Json& j);
    static TradeLimits getDefaultLimits(KYCTier tier);
};

/**
 * Email verification record
 */
struct EmailVerification {
    std::string email;
    std::string verification_code;       // 6-digit code
    int64_t sent_at;                     // Timestamp
    int64_t verified_at;                 // 0 if not verified
    VerificationStatus status;
    int attempts;                        // Failed verification attempts

    Json toJson() const;
    static EmailVerification fromJson(const Json& j);
};

/**
 * Phone verification record
 */
struct PhoneVerification {
    std::string phone;                   // E.164 format
    std::string verification_code;       // 6-digit code
    int64_t sent_at;
    int64_t verified_at;
    VerificationStatus status;
    int attempts;

    Json toJson() const;
    static PhoneVerification fromJson(const Json& j);
};

/**
 * ID verification record (for Full KYC)
 */
struct IDVerification {
    std::string document_type;           // "passport", "drivers_license", "national_id"
    std::string document_number_hash;    // SHA-256 of document number
    std::string country;                 // ISO country code
    std::string selfie_hash;             // Hash of selfie image
    std::string document_hash;           // Hash of ID document image
    int64_t submitted_at;
    int64_t reviewed_at;
    std::string reviewer_pubkey;         // Mediator who reviewed
    VerificationStatus status;
    std::string rejection_reason;

    Json toJson() const;
    static IDVerification fromJson(const Json& j);
};

/**
 * User's KYC profile
 */
struct KYCProfile {
    std::string user_pubkey;             // User's public key
    KYCTier tier;                        // Current verification tier
    std::optional<EmailVerification> email;
    std::optional<PhoneVerification> phone;
    std::optional<IDVerification> id_document;
    TradeLimits limits;                  // Current trade limits
    int64_t created_at;
    int64_t updated_at;

    // Trade volume tracking
    double volume_24h;                   // DIN traded in last 24h
    double volume_30d;                   // DIN traded in last 30 days
    int active_trades_count;             // Current open trades

    Json toJson() const;
    static KYCProfile fromJson(const Json& j);
};

/**
 * KYCManager - Manages user verification and trade limits
 */
class KYCManager {
public:
    static KYCManager& instance();

    // ═══════════════════════════════════════════════════════════════
    // USER PROFILE MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    /**
     * Get user's KYC profile (creates if doesn't exist)
     */
    KYCProfile getProfile(const std::string& user_pubkey);

    /**
     * Check if user can perform trade
     *
     * @param user_pubkey User's public key
     * @param amount_din Trade amount in DIN
     * @return true if trade is allowed within limits
     */
    bool canTrade(const std::string& user_pubkey, double amount_din);

    /**
     * Record a trade for volume tracking
     */
    void recordTrade(const std::string& user_pubkey, double amount_din);

    /**
     * Get remaining trade capacity
     *
     * @return How much DIN user can still trade today
     */
    double getRemainingDailyCapacity(const std::string& user_pubkey);

    // ═══════════════════════════════════════════════════════════════
    // EMAIL VERIFICATION
    // ═══════════════════════════════════════════════════════════════

    /**
     * Send email verification code
     *
     * @param user_pubkey User's public key
     * @param email Email address to verify
     * @return Verification record with code (for testing)
     */
    EmailVerification sendEmailVerification(
        const std::string& user_pubkey,
        const std::string& email
    );

    /**
     * Verify email with code
     *
     * @param user_pubkey User's public key
     * @param code 6-digit verification code
     * @return true if verified successfully
     */
    bool verifyEmail(const std::string& user_pubkey, const std::string& code);

    // ═══════════════════════════════════════════════════════════════
    // PHONE VERIFICATION
    // ═══════════════════════════════════════════════════════════════

    /**
     * Send phone verification code (SMS)
     *
     * @param user_pubkey User's public key
     * @param phone Phone number (E.164 format)
     * @return Verification record with code
     */
    PhoneVerification sendPhoneVerification(
        const std::string& user_pubkey,
        const std::string& phone
    );

    /**
     * Verify phone with code
     *
     * @param user_pubkey User's public key
     * @param code 6-digit verification code
     * @return true if verified successfully
     */
    bool verifyPhone(const std::string& user_pubkey, const std::string& code);

    // ═══════════════════════════════════════════════════════════════
    // ID VERIFICATION (FULL KYC)
    // ═══════════════════════════════════════════════════════════════

    /**
     * Submit ID documents for verification
     *
     * @param user_pubkey User's public key
     * @param document_type Type of ID document
     * @param document_data Base64 encoded document image
     * @param selfie_data Base64 encoded selfie
     * @return Submission record
     */
    IDVerification submitIDVerification(
        const std::string& user_pubkey,
        const std::string& document_type,
        const std::string& document_data,
        const std::string& selfie_data
    );

    /**
     * Review ID verification (mediator function)
     *
     * @param user_pubkey User being reviewed
     * @param reviewer_pubkey Mediator's public key
     * @param approved true to approve, false to reject
     * @param rejection_reason Reason if rejected
     * @return true if review recorded
     */
    bool reviewIDVerification(
        const std::string& user_pubkey,
        const std::string& reviewer_pubkey,
        bool approved,
        const std::string& rejection_reason = ""
    );

    // ═══════════════════════════════════════════════════════════════
    // TIER MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    /**
     * Get user's current KYC tier
     */
    KYCTier getTier(const std::string& user_pubkey);

    /**
     * Check if user has reached specific tier
     */
    bool hasTier(const std::string& user_pubkey, KYCTier required_tier);

    /**
     * Upgrade user tier (called automatically after verification)
     */
    void upgradeTier(const std::string& user_pubkey, KYCTier new_tier);

    // ═══════════════════════════════════════════════════════════════
    // PERSISTENCE
    // ═══════════════════════════════════════════════════════════════

    void setDataDir(const std::string& data_dir);
    void save();
    void load();

    ~KYCManager();

private:
    KYCManager();
    KYCManager(const KYCManager&) = delete;
    KYCManager& operator=(const KYCManager&) = delete;

    std::string generateVerificationCode();
    void updateVolumes(const std::string& user_pubkey);
    void initializeDatabase();

    std::map<std::string, KYCProfile> profiles_;
    std::string data_dir_;
    mutable std::unique_ptr<std::recursive_mutex> mutex_ptr_;  // Recursive to handle nested calls

    static std::unique_ptr<KYCManager> instance_;
};

} // namespace p2p
} // namespace din
