#pragma once
#include "bridge/fiat_bridge_provider.h"
#include "bridge/routing_engine.h"
#include <memory>
#include <mutex>
#include <map>
#include <vector>

namespace dinero {
namespace bridge {

/**
 * FiatBridgeManager - Central orchestrator for fiat/crypto conversions
 *
 * Manages multiple conversion providers (DEX, hybrid, custodial) and
 * provides unified interface for merchant payment settlements.
 *
 * Features:
 * - Provider registration and discovery
 * - Rate caching and aggregation
 * - Automatic provider selection
 * - Thread-safe operations
 */
class FiatBridgeManager {
public:
    /**
     * Get singleton instance
     */
    static FiatBridgeManager& instance();

    /**
     * Register a conversion provider
     *
     * @param provider Shared pointer to provider implementation
     */
    void register_provider(const std::shared_ptr<FiatBridgeProvider>& provider);

    /**
     * Execute a conversion using specified or auto-selected provider
     *
     * @param req Conversion request
     * @return ConversionResult with transaction details
     */
    ConversionResult convert(const ConversionRequest& req);

    /**
     * Get current exchange rate from any available provider
     *
     * @param from Source asset
     * @param to Destination asset
     * @return Current rate, or nullopt if unavailable
     */
    std::optional<double> get_rate(const std::string& from, const std::string& to);

    /**
     * Get exchange rate using automatic multi-hop routing
     *
     * Tries direct conversion first, then automatically finds best multi-hop route
     * (e.g., DIN→BTC→USD or DIN→USDT→USD)
     *
     * @param from Source asset
     * @param to Destination asset
     * @param route_info Optional output parameter with route details
     * @return Best rate found, or nullopt if no route exists
     */
    std::optional<double> get_rate_auto(
        const std::string& from,
        const std::string& to,
        ConversionRoute* route_info = nullptr
    );

    /**
     * Get all possible routes between two assets
     *
     * @param from Source asset
     * @param to Destination asset
     * @param max_hops Maximum intermediate conversions
     * @return Vector of routes sorted by effectiveness (best first)
     */
    std::vector<ConversionRoute> get_all_routes(
        const std::string& from,
        const std::string& to,
        int max_hops = 3
    );

    /**
     * Refresh cached exchange rates from all providers
     */
    void refresh_rates();

    /**
     * Get list of registered provider names
     */
    std::vector<std::string> list_providers() const;

    /**
     * Get cached rates for display/analysis
     */
    std::map<std::string, double> get_cached_rates() const;

private:
    FiatBridgeManager() = default;

    // Thread safety - use function-local static to avoid static initialization order issues
    static std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }

    // Registered providers (key: provider name)
    std::map<std::string, std::shared_ptr<FiatBridgeProvider>> providers_;

    // Rate cache (key: "FROM->TO", value: rate)
    std::map<std::string, double> rate_cache_;

    // Select best provider for a conversion
    std::shared_ptr<FiatBridgeProvider> select_provider(const ConversionRequest& req);
};

} // namespace bridge
} // namespace dinero
