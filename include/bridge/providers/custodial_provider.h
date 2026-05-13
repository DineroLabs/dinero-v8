#pragma once
#include "bridge/fiat_bridge_provider.h"

namespace dinero {
namespace bridge {

/**
 * CustodialProvider - Centralized exchange integration
 *
 * Integrates with:
 * - Coinbase Commerce
 * - Binance Pay
 * - Kraken REST API
 *
 * Features:
 * - High liquidity
 * - Fiat settlements (USD, EUR, etc.)
 * - KYC compliance
 * - Business-grade reliability
 *
 * Drawbacks:
 * - Centralized custody
 * - Regional restrictions
 * - Requires API credentials
 * - Rate limits
 */
class CustodialProvider : public FiatBridgeProvider {
public:
    ConversionResult convert(const ConversionRequest& req) override;
    std::optional<double> get_rate(const std::string& from, const std::string& to) override;
    std::string name() const override { return "custodial"; }
    bool is_available() const override;

private:
    // Coinbase Commerce integration
    std::string create_charge(const ConversionRequest& req);

    // Binance Pay integration
    std::string create_order(const ConversionRequest& req);
};

} // namespace bridge
} // namespace dinero
