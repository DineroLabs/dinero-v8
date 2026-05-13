#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinero {

/**
 * Supported fiat currencies for on/off-ramp.
 */
enum class FiatCurrency : uint8_t {
    USD = 0,
    EUR = 1,
    GBP = 2,
    MXN = 3,
    BRL = 4,
    ARS = 5,
};

/**
 * Quote direction: buy DPI with fiat, or sell DPI for fiat.
 */
enum class QuoteDirection : uint8_t {
    BUY  = 0,  // Fiat -> DPI
    SELL = 1,  // DPI -> Fiat
};

/**
 * A quote request from the wallet UI.
 */
struct QuoteRequest {
    QuoteDirection direction;
    FiatCurrency currency;
    uint64_t amount_una;         // DPI amount in una (1 DPI = 1e8 una)
    double fiat_amount;            // Fiat amount (used if direction=BUY)
    std::string country_code;      // ISO 3166-1 alpha-2 (e.g., "US", "MX")
};

/**
 * A provider's quote response.
 */
struct ProviderQuote {
    std::string provider_id;       // e.g., "moonpay", "ramp", "p2p-local"
    std::string provider_name;     // Human-readable name
    double rate;                   // Exchange rate (fiat per 1 DPI)
    uint64_t min_amount_una;      // Minimum DPI amount
    uint64_t max_amount_una;      // Maximum DPI amount
    double fee_percent;            // Provider fee as percentage (e.g., 1.5)
    double fee_fixed_fiat;         // Fixed fee in fiat currency
    uint32_t estimated_seconds;    // Estimated completion time
    std::string payment_method;    // e.g., "bank_transfer", "card", "pix"
    bool requires_kyc;             // Whether KYC is needed
};

/**
 * Provider registration entry (static configuration).
 */
struct ProviderConfig {
    std::string provider_id;
    std::string provider_name;
    std::string api_base_url;
    std::vector<FiatCurrency> supported_currencies;
    std::vector<std::string> supported_countries;
    bool is_enabled = true;
};

/**
 * Liquidity provider router.
 *
 * Aggregates quotes from registered providers and selects the best
 * option based on rate, fees, and availability. This is a pure product
 * layer — no on-chain logic.
 *
 * v1: Local provider registry with mock quotes.
 * v2: HTTP quote fetching from provider APIs.
 */
class LPRouter {
public:
    LPRouter() = default;

    /**
     * Register a provider configuration.
     */
    void RegisterProvider(const ProviderConfig& config);

    /**
     * Remove a provider by ID.
     */
    bool RemoveProvider(const std::string& provider_id);

    /**
     * Get all registered providers.
     */
    std::vector<ProviderConfig> GetProviders() const;

    /**
     * Get quotes from all eligible providers for the given request.
     * Filters by currency, country, and amount range.
     */
    std::vector<ProviderQuote> GetQuotes(const QuoteRequest& request) const;

    /**
     * Select the best provider based on effective rate (rate - fees).
     * Returns nullopt if no providers can fulfill the request.
     */
    std::optional<ProviderQuote> SelectBestProvider(const QuoteRequest& request) const;

    /**
     * Compute the effective fiat cost for a BUY quote,
     * or effective fiat received for a SELL quote, after fees.
     */
    static double ComputeEffectiveFiat(const ProviderQuote& quote, uint64_t amount_una);

private:
    std::vector<ProviderConfig> providers_;

    bool IsEligible(const ProviderConfig& config, const QuoteRequest& request) const;
};

} // namespace dinero
