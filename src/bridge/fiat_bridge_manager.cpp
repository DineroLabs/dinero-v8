#include "bridge/fiat_bridge_manager.h"
#include "common/logger.h"
#include <chrono>

namespace dinero {
namespace bridge {

FiatBridgeManager& FiatBridgeManager::instance() {
    static FiatBridgeManager inst;
    return inst;
}

void FiatBridgeManager::register_provider(const std::shared_ptr<FiatBridgeProvider>& provider) {
    if (!provider) {
        throw std::invalid_argument("Cannot register null provider");
    }

    std::lock_guard<std::mutex> lock(get_mutex());
    providers_[provider->name()] = provider;
    g_logger.info("[FiatBridge] Registered provider: " + provider->name());
}

ConversionResult FiatBridgeManager::convert(const ConversionRequest& req) {
    auto provider = select_provider(req);

    if (!provider) {
        ConversionResult result;
        result.success = false;
        result.message = "No available provider for conversion";
        return result;
    }

    g_logger.info("[FiatBridge] Converting " + std::to_string(req.amount) + " " +
                  req.from_asset + " -> " + req.to_asset + " via " + provider->name());

    auto result = provider->convert(req);
    result.provider = provider->name();
    result.timestamp = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;

    return result;
}

std::optional<double> FiatBridgeManager::get_rate(const std::string& from, const std::string& to) {
    std::lock_guard<std::mutex> lock(get_mutex());

    // Check cache first
    std::string key = from + "->" + to;
    auto cache_it = rate_cache_.find(key);
    if (cache_it != rate_cache_.end()) {
        return cache_it->second;
    }

    // Query providers
    for (auto& [name, provider] : providers_) {
        if (!provider->is_available()) {
            continue;
        }

        auto rate = provider->get_rate(from, to);
        if (rate) {
            rate_cache_[key] = *rate;
            return rate;
        }
    }

    return std::nullopt;
}

std::optional<double> FiatBridgeManager::get_rate_auto(
    const std::string& from,
    const std::string& to,
    ConversionRoute* route_info)
{
    std::lock_guard<std::mutex> lock(get_mutex());

    // Try direct rate first
    auto direct_rate = get_rate(from, to);
    if (direct_rate) {
        g_logger.info("[FiatBridge] Using direct rate: " + from + " → " + to + " = " + std::to_string(*direct_rate));
        return direct_rate;
    }

    // No direct rate, use routing engine
    g_logger.info("[FiatBridge] No direct rate, trying multi-hop routing...");

    std::vector<std::shared_ptr<FiatBridgeProvider>> provider_list;
    for (const auto& [name, provider] : providers_) {
        provider_list.push_back(provider);
    }

    auto best_route = RoutingEngine::find_best_route(from, to, provider_list, 3);

    if (!best_route) {
        g_logger.warning("[FiatBridge] No route found from " + from + " to " + to);
        return std::nullopt;
    }

    // Return route info if requested
    if (route_info) {
        *route_info = *best_route;
    }

    g_logger.info("[FiatBridge] Found multi-hop route: " + best_route->description() +
                  " (effective rate: " + std::to_string(best_route->effective_rate()) + ")");

    return best_route->effective_rate();
}

std::vector<ConversionRoute> FiatBridgeManager::get_all_routes(
    const std::string& from,
    const std::string& to,
    int max_hops)
{
    std::lock_guard<std::mutex> lock(get_mutex());

    std::vector<std::shared_ptr<FiatBridgeProvider>> provider_list;
    for (const auto& [name, provider] : providers_) {
        provider_list.push_back(provider);
    }

    return RoutingEngine::find_all_routes(from, to, provider_list, max_hops);
}

void FiatBridgeManager::refresh_rates() {
    std::lock_guard<std::mutex> lock(get_mutex());

    g_logger.info("[FiatBridge] Refreshing exchange rates...");
    rate_cache_.clear();

    // Refresh common pairs
    std::vector<std::pair<std::string, std::string>> pairs = {
        {"DIN", "USDT"},
        {"DIN", "USDC"},
        {"DIN", "USD"},
        {"DIN", "EUR"},
        {"DIN", "BTC"},
        {"DIN", "ETH"}
    };

    int updated = 0;
    for (const auto& [from, to] : pairs) {
        for (auto& [name, provider] : providers_) {
            if (!provider->is_available()) {
                continue;
            }

            auto rate = provider->get_rate(from, to);
            if (rate) {
                std::string key = from + "->" + to;
                rate_cache_[key] = *rate;
                updated++;
                break;  // Got rate from one provider, move to next pair
            }
        }
    }

    g_logger.info("[FiatBridge] Refreshed " + std::to_string(updated) + " exchange rates");
}

std::vector<std::string> FiatBridgeManager::list_providers() const {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::vector<std::string> names;
    names.reserve(providers_.size());

    for (const auto& [name, provider] : providers_) {
        names.push_back(name);
    }

    return names;
}

std::map<std::string, double> FiatBridgeManager::get_cached_rates() const {
    std::lock_guard<std::mutex> lock(get_mutex());
    return rate_cache_;
}

std::shared_ptr<FiatBridgeProvider> FiatBridgeManager::select_provider(const ConversionRequest& req) {
    std::lock_guard<std::mutex> lock(get_mutex());

    // If provider hint specified, use it
    if (!req.provider_hint.empty()) {
        auto it = providers_.find(req.provider_hint);
        if (it != providers_.end() && it->second->is_available()) {
            return it->second;
        }
        g_logger.warning("[FiatBridge] Requested provider '" + req.provider_hint + "' not available");
    }

    // Auto-select: prefer DEX for small amounts, custodial for large
    const double LARGE_CONVERSION_THRESHOLD = 10000.0;  // USD equivalent

    for (auto& [name, provider] : providers_) {
        if (!provider->is_available()) {
            continue;
        }

        // Simple heuristic: use first available provider
        // TODO: Implement smart routing based on amount, fees, liquidity
        return provider;
    }

    return nullptr;
}

} // namespace bridge
} // namespace dinero
