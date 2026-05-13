#include "bridge/providers/dex_provider.h"
#include "bridge/http_client.h"
#include "common/logger.h"
#include <json/json.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace bridge {

ConversionResult DexProvider::convert(const ConversionRequest& req) {
    ConversionResult result;

    g_logger.info("[DexProvider] Swapping " + std::to_string(req.amount) + " " +
                  req.from_asset + " → " + req.to_asset);

    // TODO: Integrate with Li.Fi or Thorchain API
    // For now, simulate conversion with mock data

    result.success = true;
    result.rate = 0.99;  // Mock rate (99 cents per DIN)
    result.received_amount = req.amount * result.rate;
    result.fee_amount = req.amount * 0.01;  // 1% fee
    result.slippage_bps = 100;  // 1% slippage

    // Generate mock transaction ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000000, 9999999);
    result.txid = "dex_" + std::to_string(dis(gen));

    result.message = "Simulated DEX conversion via Li.Fi aggregator";

    g_logger.info("[DexProvider] Conversion complete: " + result.txid +
                  " (" + std::to_string(result.received_amount) + " " + req.to_asset + ")");

    return result;
}

// Enhanced mock rate table for testing multi-asset escrow
static const std::map<std::pair<std::string, std::string>, double> MOCK_RATES = {
    // DIN rates
    {{"DIN", "USD"}, 0.10}, {{"DIN", "EUR"}, 0.092}, {{"DIN", "GBP"}, 0.079},
    {{"DIN", "BTC"}, 0.0000024}, {{"DIN", "ETH"}, 0.000041},
    {{"DIN", "USDT"}, 0.10}, {{"DIN", "USDC"}, 0.10}, {{"DIN", "DAI"}, 0.10},

    // BTC rates
    {{"BTC", "USD"}, 42000.0}, {{"BTC", "EUR"}, 38640.0}, {{"BTC", "GBP"}, 33180.0},
    {{"BTC", "USDT"}, 42000.0}, {{"BTC", "USDC"}, 42000.0}, {{"BTC", "DAI"}, 42000.0},
    {{"BTC", "ETH"}, 17.5}, {{"BTC", "DIN"}, 420000.0},

    // ETH rates
    {{"ETH", "USD"}, 2400.0}, {{"ETH", "EUR"}, 2208.0}, {{"ETH", "GBP"}, 1896.0},
    {{"ETH", "USDT"}, 2400.0}, {{"ETH", "USDC"}, 2400.0}, {{"ETH", "DAI"}, 2400.0},
    {{"ETH", "BTC"}, 0.057}, {{"ETH", "DIN"}, 24000.0},

    // Stablecoin to fiat
    {{"USDT", "USD"}, 1.0}, {{"USDT", "EUR"}, 0.92}, {{"USDT", "GBP"}, 0.79},
    {{"USDT", "USDC"}, 1.0}, {{"USDT", "DAI"}, 1.0},
    {{"USDC", "USD"}, 1.0}, {{"USDC", "EUR"}, 0.92}, {{"USDC", "GBP"}, 0.79},
    {{"USDC", "USDT"}, 1.0}, {{"USDC", "DAI"}, 1.0},
    {{"DAI", "USD"}, 1.0}, {{"DAI", "EUR"}, 0.92}, {{"DAI", "GBP"}, 0.79},
    {{"DAI", "USDT"}, 1.0}, {{"DAI", "USDC"}, 1.0},

    // Fiat to fiat
    {{"USD", "EUR"}, 0.92}, {{"USD", "GBP"}, 0.79},
    {{"EUR", "USD"}, 1.087}, {{"EUR", "GBP"}, 0.859},
    {{"GBP", "USD"}, 1.266}, {{"GBP", "EUR"}, 1.164},
};

std::optional<double> DexProvider::get_rate(const std::string& from, const std::string& to) {
    // Same asset
    if (from == to) {
        return 1.0;
    }

    // Try CoinGecko first for price data
    auto rate = get_coingecko_rate(from, to);
    if (rate) {
        return rate;
    }

    // Fallback to mock rates if API unavailable
    g_logger.warning("[DexProvider] CoinGecko API unavailable, using fallback rates");

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

bool DexProvider::is_available() const {
    // TODO: Check if Li.Fi API is reachable
    // For now, always available
    return true;
}

std::string DexProvider::get_lifi_quote(const std::string& from, const std::string& to, double amount) {
    // TODO: Implement actual HTTP request to Li.Fi API
    // Example endpoint: https://li.quest/v1/quote?fromChain=DIN&toChain=ETH&fromToken=DIN&toToken=USDT&fromAmount=100
    return "{}";
}

std::optional<double> DexProvider::get_coingecko_rate(const std::string& from, const std::string& to) {
    // Map asset symbols to CoinGecko IDs
    std::string from_id = (from == "DIN") ? "dinero" : from;
    std::string to_currency = to;
    std::transform(to_currency.begin(), to_currency.end(), to_currency.begin(), ::tolower);

    // Build CoinGecko API URL
    std::ostringstream url;
    url << "https://api.coingecko.com/api/v3/simple/price";
    url << "?ids=" << from_id;
    url << "&vs_currencies=" << to_currency;
    url << "&precision=full";

    try {
        // Make API request
        auto response = HttpClient::get(url.str(), {}, 10);

        if (!response.success) {
            g_logger.warning("[DexProvider] CoinGecko request failed: " + response.error);
            return std::nullopt;
        }

        // Parse JSON response
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        Json::Value root;
        std::string errors;

        bool parsed = reader->parse(
            response.body.c_str(),
            response.body.c_str() + response.body.size(),
            &root,
            &errors
        );
        delete reader;

        if (!parsed) {
            g_logger.error("[DexProvider] Failed to parse CoinGecko response: " + errors);
            return std::nullopt;
        }

        // Extract rate from response
        // Response format: {"dinero": {"usd": 0.98, "usdt": 0.99}}
        if (root.isMember(from_id) && root[from_id].isMember(to_currency)) {
            double rate = root[from_id][to_currency].asDouble();
            g_logger.info("[DexProvider] CoinGecko rate: " + from + " -> " + to + " = " + std::to_string(rate));
            return rate;
        }

        g_logger.warning("[DexProvider] Rate not found in CoinGecko response");
        return std::nullopt;

    } catch (const std::exception& e) {
        g_logger.error("[DexProvider] CoinGecko API error: " + std::string(e.what()));
        return std::nullopt;
    }
}

} // namespace bridge
} // namespace dinero
