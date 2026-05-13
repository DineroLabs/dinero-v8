/**
 * Payment Adapter System - Modular Payment Method Support
 *
 * Handles multiple fiat payment rails with standardized interface.
 * Phase 1: US/Canada (Zelle, Cash App, Venmo, Interac)
 * Future: EU (SEPA), LATAM (PIX), Africa (M-Pesa)
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include "din_json.h"

namespace din {
namespace p2p {

/**
 * Supported geographic regions
 */
enum class PaymentRegion {
    US_CANADA,      // United States & Canada
    EU,             // European Union
    LATAM,          // Latin America
    AFRICA,         // Africa
    ASIA_PACIFIC,   // Asia Pacific
    GLOBAL          // Works everywhere (e.g., PayPal)
};

/**
 * Payment method characteristics
 */
struct PaymentMethodInfo {
    std::string id;                          // Unique identifier (e.g., "zelle")
    std::string display_name;                // User-facing name (e.g., "Zelle")
    std::string icon_emoji;                  // UI icon (e.g., "💵")
    PaymentRegion region;                    // Geographic availability
    int typical_settlement_minutes;          // How fast payments clear
    bool requires_phone;                     // Needs phone number
    bool requires_email;                     // Needs email address
    bool requires_account_name;              // Needs account holder name
    bool supports_instant;                   // Instant settlement
    std::string handle_placeholder;          // UI placeholder text
    std::vector<std::string> supported_currencies; // Fiat currencies

    Json toJson() const;
    static PaymentMethodInfo fromJson(const Json& j);
};

/**
 * User's payment handle (encrypted)
 */
struct PaymentHandle {
    std::string method_id;                   // Payment method ID
    std::string encrypted_handle;            // Encrypted phone/email/account
    std::string handle_hash;                 // SHA-256 for verification
    std::string display_hint;                // Last 4 digits/chars (e.g., "***@****.com")
    bool verified;                           // Verified via 2FA or test payment
    int64_t added_at;                        // Timestamp when added
    int64_t last_used_at;                    // Last trade timestamp

    Json toJson() const;
    static PaymentHandle fromJson(const Json& j);
};

/**
 * PaymentAdapter - Base class for payment method implementations
 */
class PaymentAdapter {
public:
    virtual ~PaymentAdapter() = default;

    /**
     * Get payment method info
     */
    virtual PaymentMethodInfo getInfo() const = 0;

    /**
     * Validate payment handle format
     *
     * @param handle Unencrypted handle (phone, email, etc.)
     * @return true if valid format
     */
    virtual bool validateHandle(const std::string& handle) const = 0;

    /**
     * Create display hint from handle
     *
     * @param handle Unencrypted handle
     * @return Masked version (e.g., "+1***555***12")
     */
    virtual std::string createDisplayHint(const std::string& handle) const = 0;

    /**
     * Generate payment instructions for trade
     *
     * @param handle Decrypted payment handle
     * @param amount Fiat amount
     * @param currency Fiat currency code
     * @param reference Unique payment reference
     * @return Human-readable payment instructions
     */
    virtual std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const = 0;
};

// ═══════════════════════════════════════════════════════════════
// US/CANADA PAYMENT ADAPTERS
// ═══════════════════════════════════════════════════════════════

/**
 * Zelle (US Bank-to-Bank) - Most popular in US
 */
class ZelleAdapter : public PaymentAdapter {
public:
    PaymentMethodInfo getInfo() const override;
    bool validateHandle(const std::string& handle) const override;
    std::string createDisplayHint(const std::string& handle) const override;
    std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const override;
};

/**
 * Cash App (Square)
 */
class CashAppAdapter : public PaymentAdapter {
public:
    PaymentMethodInfo getInfo() const override;
    bool validateHandle(const std::string& handle) const override;
    std::string createDisplayHint(const std::string& handle) const override;
    std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const override;
};

/**
 * Venmo (PayPal-owned)
 */
class VenmoAdapter : public PaymentAdapter {
public:
    PaymentMethodInfo getInfo() const override;
    bool validateHandle(const std::string& handle) const override;
    std::string createDisplayHint(const std::string& handle) const override;
    std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const override;
};

/**
 * Apple Pay (limited P2P support via iMessage)
 */
class ApplePayAdapter : public PaymentAdapter {
public:
    PaymentMethodInfo getInfo() const override;
    bool validateHandle(const std::string& handle) const override;
    std::string createDisplayHint(const std::string& handle) const override;
    std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const override;
};

/**
 * Google Pay
 */
class GooglePayAdapter : public PaymentAdapter {
public:
    PaymentMethodInfo getInfo() const override;
    bool validateHandle(const std::string& handle) const override;
    std::string createDisplayHint(const std::string& handle) const override;
    std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const override;
};

/**
 * Interac e-Transfer (Canada)
 */
class InteracAdapter : public PaymentAdapter {
public:
    PaymentMethodInfo getInfo() const override;
    bool validateHandle(const std::string& handle) const override;
    std::string createDisplayHint(const std::string& handle) const override;
    std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const override;
};

/**
 * Bank Transfer (Generic)
 */
class BankTransferAdapter : public PaymentAdapter {
public:
    PaymentMethodInfo getInfo() const override;
    bool validateHandle(const std::string& handle) const override;
    std::string createDisplayHint(const std::string& handle) const override;
    std::string generateInstructions(
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const override;
};

// ═══════════════════════════════════════════════════════════════
// PAYMENT ADAPTER REGISTRY
// ═══════════════════════════════════════════════════════════════

/**
 * PaymentAdapterRegistry - Manages all payment adapters
 */
class PaymentAdapterRegistry {
public:
    static PaymentAdapterRegistry& instance();

    /**
     * Register a payment adapter
     */
    void registerAdapter(const std::string& method_id, std::unique_ptr<PaymentAdapter> adapter);

    /**
     * Get adapter for payment method
     */
    PaymentAdapter* getAdapter(const std::string& method_id) const;

    /**
     * List all available payment methods
     */
    std::vector<PaymentMethodInfo> listMethods() const;

    /**
     * List methods available in specific region
     */
    std::vector<PaymentMethodInfo> listMethodsByRegion(PaymentRegion region) const;

    /**
     * Validate payment handle
     */
    bool validateHandle(const std::string& method_id, const std::string& handle) const;

    /**
     * Create display hint
     */
    std::string createDisplayHint(const std::string& method_id, const std::string& handle) const;

    /**
     * Generate payment instructions
     */
    std::string generateInstructions(
        const std::string& method_id,
        const std::string& handle,
        double amount,
        const std::string& currency,
        const std::string& reference
    ) const;

public:
    ~PaymentAdapterRegistry() = default;

private:
    PaymentAdapterRegistry();

    void registerDefaultAdapters();

    std::map<std::string, std::unique_ptr<PaymentAdapter>> adapters_;
    static std::unique_ptr<PaymentAdapterRegistry> instance_;
};

} // namespace p2p
} // namespace din
