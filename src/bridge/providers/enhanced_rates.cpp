#include "bridge/providers/dex_provider.h"
#include "bridge/providers/hybrid_provider.h"
#include "bridge/providers/custodial_provider.h"
#include "common/logger.h"

namespace dinero {
namespace bridge {

/**
 * Enhanced mock rates for multi-asset escrow testing
 *
 * This provides realistic conversion rates for all supported asset pairs
 * until real API integrations are completed.
 */

// Add static rate tables to each provider
namespace enhanced_rates {

// Common crypto to fiat rates (mock, but realistic)
static const std::map<std::pair<std::string, std::string>, double> MOCK_RATES = {
    // DIN rates
    {{"DIN", "USD"}, 0.10},
    {{"DIN", "EUR"}, 0.092},
    {{"DIN", "GBP"}, 0.079},
    {{"DIN", "BTC"}, 0.0000024},
    {{"DIN", "ETH"}, 0.000041},
    {{"DIN", "USDT"}, 0.10},
    {{"DIN", "USDC"}, 0.10},
    {{"DIN", "DAI"}, 0.10},

    // BTC rates
    {{"BTC", "USD"}, 42000.0},
    {{"BTC", "EUR"}, 38640.0},
    {{"BTC", "GBP"}, 33180.0},
    {{"BTC", "USDT"}, 42000.0},
    {{"BTC", "USDC"}, 42000.0},
    {{"BTC", "DAI"}, 42000.0},
    {{"BTC", "ETH"}, 17.5},
    {{"BTC", "DIN"}, 420000.0},

    // ETH rates
    {{"ETH", "USD"}, 2400.0},
    {{"ETH", "EUR"}, 2208.0},
    {{"ETH", "GBP"}, 1896.0},
    {{"ETH", "USDT"}, 2400.0},
    {{"ETH", "USDC"}, 2400.0},
    {{"ETH", "DAI"}, 2400.0},
    {{"ETH", "BTC"}, 0.057},
    {{"ETH", "DIN"}, 24000.0},

    // Stablecoin to fiat
    {{"USDT", "USD"}, 1.0},
    {{"USDT", "EUR"}, 0.92},
    {{"USDT", "GBP"}, 0.79},
    {{"USDT", "USDC"}, 1.0},
    {{"USDT", "DAI"}, 1.0},

    {{"USDC", "USD"}, 1.0},
    {{"USDC", "EUR"}, 0.92},
    {{"USDC", "GBP"}, 0.79},
    {{"USDC", "USDT"}, 1.0},
    {{"USDC", "DAI"}, 1.0},

    {{"DAI", "USD"}, 1.0},
    {{"DAI", "EUR"}, 0.92},
    {{"DAI", "GBP"}, 0.79},
    {{"DAI", "USDT"}, 1.0},
    {{"DAI", "USDC"}, 1.0},

    // Fiat to fiat
    {{"USD", "EUR"}, 0.92},
    {{"USD", "GBP"}, 0.79},
    {{"EUR", "USD"}, 1.087},
    {{"EUR", "GBP"}, 0.859},
    {{"GBP", "USD"}, 1.266},
    {{"GBP", "EUR"}, 1.164},

    // Reverse rates (crypto to stables)
    {{"USD", "USDT"}, 1.0},
    {{"EUR", "USDT"}, 1.087},
    {{"GBP", "USDT"}, 1.266},
};

std::optional<double> getMockRate(const std::string& from, const std::string& to) {
    // Direct lookup
    auto key = std::make_pair(from, to);
    auto it = MOCK_RATES.find(key);
    if (it != MOCK_RATES.end()) {
        return it->second;
    }

    // Try inverse rate
    auto inv_key = std::make_pair(to, from);
    auto inv_it = MOCK_RATES.find(inv_key);
    if (inv_it != MOCK_RATES.end() && inv_it->second != 0.0) {
        return 1.0 / inv_it->second;
    }

    return std::nullopt;
}

} // namespace enhanced_rates

} // namespace bridge
} // namespace dinero
