#pragma once
#include <string>
#include <optional>
#include <cstdint>

namespace dinero {
namespace bridge {

/**
 * ConversionRequest - Parameters for a fiat/crypto conversion
 */
struct ConversionRequest {
    std::string from_asset;      // "DIN"
    std::string to_asset;        // "USDT", "USDC", "USD", "EUR", etc.
    double amount;               // Amount to convert (in from_asset units)
    std::string dest_address;    // Destination wallet address for receiving asset
    std::string provider_hint;   // "dex", "hybrid", "custodial" (or empty for auto)

    // Optional parameters
    std::string webhook_url;     // Callback URL for async notifications
    uint32_t max_slippage_bps;   // Max slippage in basis points (100 = 1%)
    uint32_t timeout_seconds;    // Max time to wait for conversion
};

/**
 * ConversionResult - Result of a conversion operation
 */
struct ConversionResult {
    bool success = false;
    std::string txid;                 // Transaction ID or order ID
    double received_amount = 0.0;     // Actual amount received (may differ due to fees/slippage)
    double rate = 0.0;                // Exchange rate used
    std::string provider;             // Provider that handled the conversion
    std::string message;              // Human-readable status/error message

    // Additional metadata
    double fee_amount = 0.0;          // Fee charged (in from_asset units)
    double slippage_bps = 0.0;        // Actual slippage (basis points)
    uint64_t timestamp = 0;           // Unix timestamp
};

/**
 * FiatBridgeProvider - Abstract base class for conversion providers
 *
 * Implementations: DexProvider (Li.Fi, Thorchain),
 *                  HybridProvider (SimpleSwap, ChangeNOW),
 *                  CustodialProvider (Coinbase, Binance)
 */
class FiatBridgeProvider {
public:
    virtual ~FiatBridgeProvider() = default;

    /**
     * Execute a conversion from one asset to another
     *
     * @param req Conversion request with source/dest asset and amount
     * @return ConversionResult with transaction details
     */
    virtual ConversionResult convert(const ConversionRequest& req) = 0;

    /**
     * Get current exchange rate between two assets
     *
     * @param from Source asset (e.g., "DIN")
     * @param to Destination asset (e.g., "USDT")
     * @return Current rate, or nullopt if unavailable
     */
    virtual std::optional<double> get_rate(const std::string& from, const std::string& to) = 0;

    /**
     * Get provider name (e.g., "dex", "hybrid", "custodial")
     */
    virtual std::string name() const = 0;

    /**
     * Check if provider is available/configured
     */
    virtual bool is_available() const { return true; }
};

} // namespace bridge
} // namespace dinero
