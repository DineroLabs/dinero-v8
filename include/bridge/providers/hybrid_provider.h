#pragma once
#include "bridge/fiat_bridge_provider.h"

namespace dinero {
namespace bridge {

/**
 * HybridProvider - Semi-custodial instant swap services
 *
 * Integrates with:
 * - SimpleSwap.io
 * - ChangeNOW
 * - StealthEX
 *
 * Features:
 * - Fast conversions (minutes)
 * - Auto liquidity management
 * - Simple REST API
 *
 * Drawbacks:
 * - Temporary custody during swap
 * - API keys required
 * - Small conversion fees
 */
class HybridProvider : public FiatBridgeProvider {
public:
    ConversionResult convert(const ConversionRequest& req) override;
    std::optional<double> get_rate(const std::string& from, const std::string& to) override;
    std::string name() const override { return "hybrid"; }
    bool is_available() const override;

private:
    // SimpleSwap API integration
    std::string create_exchange(const ConversionRequest& req);
    std::optional<double> get_simpleswap_rate(const std::string& from, const std::string& to);
};

} // namespace bridge
} // namespace dinero
