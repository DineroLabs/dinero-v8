#pragma once
#include "bridge/fiat_bridge_provider.h"

namespace dinero {
namespace bridge {

/**
 * DexProvider - Decentralized exchange aggregator
 *
 * Integrates with:
 * - Li.Fi (DEX aggregator)
 * - Thorchain (cross-chain swaps)
 * - CoinGecko (price discovery)
 *
 * Features:
 * - Non-custodial swaps
 * - No KYC required
 * - Trustless routing
 *
 * Drawbacks:
 * - Limited liquidity for new coins
 * - Slippage on low-volume pairs
 */
class DexProvider : public FiatBridgeProvider {
public:
    ConversionResult convert(const ConversionRequest& req) override;
    std::optional<double> get_rate(const std::string& from, const std::string& to) override;
    std::string name() const override { return "dex"; }
    bool is_available() const override;

private:
    // Li.Fi API endpoint
    std::string get_lifi_quote(const std::string& from, const std::string& to, double amount);

    // CoinGecko price API
    std::optional<double> get_coingecko_rate(const std::string& from, const std::string& to);
};

} // namespace bridge
} // namespace dinero
