#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <memory>

namespace dinero {
namespace bridge {

// Forward declarations
class FiatBridgeProvider;

/**
 * Represents a single hop in a conversion route
 */
struct RouteHop {
    std::string from_asset;
    std::string to_asset;
    double rate = 0.0;
    double fee_bps = 0.0;  // Fee in basis points
    std::string provider;
};

/**
 * Represents a complete conversion route with multiple hops
 */
struct ConversionRoute {
    std::vector<RouteHop> hops;
    double total_rate = 0.0;      // Combined rate across all hops
    double total_fee_bps = 0.0;   // Total fees in basis points
    double slippage_bps = 0.0;    // Expected slippage
    int hop_count = 0;

    // Route description (e.g., "DIN→BTC→USD via dex+coinbase")
    std::string description() const;

    // Calculate effective rate after fees and slippage
    double effective_rate() const;
};

/**
 * Dynamic routing engine for finding optimal conversion paths
 *
 * Features:
 * - Multi-hop pathfinding (DIN→BTC→USD, DIN→USDT→USD, etc.)
 * - Automatic best-route selection based on effective rates
 * - Provider abstraction (queries all registered providers)
 * - Caching for performance
 * - Dijkstra-based shortest path algorithm
 */
class RoutingEngine {
public:
    /**
     * Find the best conversion route between two assets
     *
     * @param from Source asset (e.g., "DIN")
     * @param to Destination asset (e.g., "USD")
     * @param providers List of available providers to query
     * @param max_hops Maximum number of intermediate conversions (default: 3)
     * @return Best route if found, nullopt otherwise
     */
    static std::optional<ConversionRoute> find_best_route(
        const std::string& from,
        const std::string& to,
        const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers,
        int max_hops = 3
    );

    /**
     * Get all possible routes between two assets
     *
     * @param from Source asset
     * @param to Destination asset
     * @param providers Available providers
     * @param max_hops Maximum hops allowed
     * @return Vector of all viable routes, sorted by effective rate (best first)
     */
    static std::vector<ConversionRoute> find_all_routes(
        const std::string& from,
        const std::string& to,
        const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers,
        int max_hops = 3
    );

private:
    /**
     * Build a rate graph from all providers
     *
     * Graph structure: asset → [(dest_asset, rate, fee, provider)]
     */
    struct RateEdge {
        std::string to;
        double rate;
        double fee_bps;
        std::string provider;
    };

    using RateGraph = std::map<std::string, std::vector<RateEdge>>;

    static RateGraph build_rate_graph(
        const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers
    );

    /**
     * Dijkstra-style pathfinding for best rate
     * Returns path as vector of asset symbols
     */
    static std::optional<std::vector<std::string>> find_path(
        const RateGraph& graph,
        const std::string& from,
        const std::string& to,
        int max_hops
    );

    /**
     * Convert asset path to full route with rates
     */
    static std::optional<ConversionRoute> path_to_route(
        const std::vector<std::string>& path,
        const RateGraph& graph
    );

    /**
     * Common intermediate assets to try
     */
    static const std::vector<std::string>& intermediate_assets();
};

} // namespace bridge
} // namespace dinero
