#include "bridge/providers/hybrid_provider.h"
#include "bridge/http_client.h"
#include "common/logger.h"
#include <json/json.h>
#include <random>
#include <sstream>
#include <algorithm>
#include <cstdlib>

namespace dinero {
namespace bridge {

ConversionResult HybridProvider::convert(const ConversionRequest& req) {
    ConversionResult result;

    g_logger.info("[HybridProvider] Using SimpleSwap-style service for " +
                  std::to_string(req.amount) + " " + req.from_asset + " → " + req.to_asset);

    // TODO: Integrate with SimpleSwap or ChangeNOW API
    // POST https://api.simpleswap.io/v1/create_exchange
    // {
    //   "fixed": false,
    //   "currency_from": "din",
    //   "currency_to": "usdt",
    //   "amount": 100,
    //   "address_to": "0x..."
    // }

    result.success = true;
    result.rate = 0.985;  // Slightly lower rate due to fees
    result.received_amount = req.amount * result.rate;
    result.fee_amount = req.amount * 0.015;  // 1.5% fee
    result.slippage_bps = 50;  // 0.5% slippage

    // Generate mock exchange ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    result.txid = "swap_" + std::to_string(dis(gen));

    result.message = "Simulated hybrid swap via SimpleSwap API";

    g_logger.info("[HybridProvider] Swap created: " + result.txid);

    return result;
}

std::optional<double> HybridProvider::get_rate(const std::string& from, const std::string& to) {
    // Try SimpleSwap API first
    auto rate = get_simpleswap_rate(from, to);
    if (rate) {
        return rate;
    }

    // Fallback to mock rates
    g_logger.warning("[HybridProvider] Using fallback rates");

    if (from == "DIN" && to == "USDT") {
        return 0.985;
    }
    if (from == "DIN" && to == "USDC") {
        return 0.984;
    }

    return std::nullopt;
}

bool HybridProvider::is_available() const {
    // TODO: Check if API key is configured and service is reachable
    // const char* api_key = std::getenv("SIMPLESWAP_API_KEY");
    // return api_key != nullptr;
    return true;
}

std::string HybridProvider::create_exchange(const ConversionRequest& req) {
    // TODO: Implement actual HTTP POST to SimpleSwap
    return "{}";
}

std::optional<double> HybridProvider::get_simpleswap_rate(const std::string& from, const std::string& to) {
    const char* api_key = std::getenv("SIMPLESWAP_API_KEY");
    if (!api_key) {
        g_logger.warning("[HybridProvider] SIMPLESWAP_API_KEY not set, using fallback rates");
        return std::nullopt;
    }

    // Convert symbols to lowercase for SimpleSwap API
    std::string from_lower = from;
    std::string to_lower = to;
    std::transform(from_lower.begin(), from_lower.end(), from_lower.begin(), ::tolower);
    std::transform(to_lower.begin(), to_lower.end(), to_lower.begin(), ::tolower);

    // Build SimpleSwap API URL
    std::ostringstream url;
    url << "https://api.simpleswap.io/v1/get_estimated";
    url << "?api_key=" << api_key;
    url << "&currency_from=" << from_lower;
    url << "&currency_to=" << to_lower;
    url << "&amount=100";  // Query rate for 100 units

    try {
        auto response = HttpClient::get(url.str(), {}, 10);

        if (!response.success) {
            g_logger.warning("[HybridProvider] SimpleSwap request failed: " + response.error);
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
            g_logger.error("[HybridProvider] Failed to parse SimpleSwap response: " + errors);
            return std::nullopt;
        }

        // SimpleSwap returns estimated amount, calculate rate
        // Response: {"estimated_amount": "98.5"}
        if (root.isMember("estimated_amount")) {
            double estimated = std::stod(root["estimated_amount"].asString());
            double rate = estimated / 100.0;  // Divide by our query amount
            g_logger.info("[HybridProvider] SimpleSwap rate: " + from + " -> " + to + " = " + std::to_string(rate));
            return rate;
        }

        return std::nullopt;

    } catch (const std::exception& e) {
        g_logger.error("[HybridProvider] SimpleSwap API error: " + std::string(e.what()));
        return std::nullopt;
    }
}

} // namespace bridge
} // namespace dinero
