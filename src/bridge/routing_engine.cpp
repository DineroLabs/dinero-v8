#include "bridge/routing_engine.h"
#include "bridge/fiat_bridge_provider.h"
#include "common/logger.h"
#include <algorithm>
#include <queue>
#include <set>
#include <cmath>
#include <sstream>

namespace dinero {
namespace bridge {

// ═══════════════════════════════════════════════════════════════
// ConversionRoute Methods
// ═══════════════════════════════════════════════════════════════

std::string ConversionRoute::description() const {
    if (hops.empty()) return "empty_route";

    std::ostringstream oss;
    oss << hops[0].from_asset;
    for (const auto& hop : hops) {
        oss << "→" << hop.to_asset;
    }
    oss << " via ";
    for (size_t i = 0; i < hops.size(); ++i) {
        if (i > 0) oss << "+";
        oss << hops[i].provider;
    }
    return oss.str();
}

double ConversionRoute::effective_rate() const {
    if (total_rate <= 0) return 0.0;

    // Apply fees and slippage
    double fee_multiplier = 1.0 - (total_fee_bps / 10000.0);
    double slippage_multiplier = 1.0 - (slippage_bps / 10000.0);

    return total_rate * fee_multiplier * slippage_multiplier;
}

// ═══════════════════════════════════════════════════════════════
// RoutingEngine Implementation
// ═══════════════════════════════════════════════════════════════

const std::vector<std::string>& RoutingEngine::intermediate_assets() {
    // Common bridge assets for multi-hop routing
    static const std::vector<std::string> assets = {
        "BTC", "ETH", "USDT", "USDC", "DAI", "BUSD"
    };
    return assets;
}

RoutingEngine::RateGraph RoutingEngine::build_rate_graph(
    const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers)
{
    RateGraph graph;

    // Query all providers for available pairs
    std::vector<std::pair<std::string, std::string>> pairs_to_check = {
        // DIN pairs
        {"DIN", "BTC"}, {"DIN", "ETH"}, {"DIN", "USDT"}, {"DIN", "USDC"},
        {"DIN", "USD"}, {"DIN", "EUR"}, {"DIN", "GBP"},

        // BTC pairs
        {"BTC", "USD"}, {"BTC", "EUR"}, {"BTC", "USDT"}, {"BTC", "USDC"},
        {"BTC", "ETH"},

        // ETH pairs
        {"ETH", "USD"}, {"ETH", "EUR"}, {"ETH", "USDT"}, {"ETH", "USDC"},

        // Stablecoin pairs
        {"USDT", "USD"}, {"USDT", "EUR"}, {"USDT", "USDC"},
        {"USDC", "USD"}, {"USDC", "EUR"},
        {"DAI", "USD"}, {"DAI", "USDT"}
    };

    for (const auto& [from, to] : pairs_to_check) {
        for (const auto& provider : providers) {
            if (!provider->is_available()) continue;

            auto rate = provider->get_rate(from, to);
            if (rate && *rate > 0) {
                RateEdge edge;
                edge.to = to;
                edge.rate = *rate;
                edge.fee_bps = 150;  // Default 1.5% fee (override per provider if needed)
                edge.provider = provider->name();

                graph[from].push_back(edge);

                // Also add reverse direction with inverse rate
                RateEdge reverse;
                reverse.to = from;
                reverse.rate = 1.0 / *rate;
                reverse.fee_bps = 150;
                reverse.provider = provider->name();
                graph[to].push_back(reverse);
            }
        }
    }

    return graph;
}

std::optional<std::vector<std::string>> RoutingEngine::find_path(
    const RateGraph& graph,
    const std::string& from,
    const std::string& to,
    int max_hops)
{
    // Dijkstra-style pathfinding optimized for best exchange rate
    // We use negative log of rates to turn multiplication into addition
    // Best rate = highest product = smallest sum of -log(rate)

    struct Node {
        std::string asset;
        double cost;  // -log(cumulative_rate)
        std::vector<std::string> path;

        bool operator>(const Node& other) const {
            return cost > other.cost;  // Min-heap
        }
    };

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;
    std::set<std::string> visited;

    // Start node
    pq.push({from, 0.0, {from}});

    while (!pq.empty()) {
        Node current = pq.top();
        pq.pop();

        // Check if we reached destination
        if (current.asset == to) {
            g_logger.info("[RoutingEngine] Found path: " +
                         std::to_string(current.path.size() - 1) + " hops, " +
                         "effective rate: " + std::to_string(std::exp(-current.cost)));
            return current.path;
        }

        // Skip if already visited
        if (visited.count(current.asset)) continue;
        visited.insert(current.asset);

        // Prune if too many hops
        if (current.path.size() > static_cast<size_t>(max_hops) + 1) continue;

        // Explore neighbors
        auto it = graph.find(current.asset);
        if (it == graph.end()) continue;

        for (const auto& edge : it->second) {
            // Avoid cycles
            if (std::find(current.path.begin(), current.path.end(), edge.to) != current.path.end()) {
                continue;
            }

            // Calculate cost: -log(rate * (1 - fee))
            double fee_multiplier = 1.0 - (edge.fee_bps / 10000.0);
            double effective_rate = edge.rate * fee_multiplier;

            if (effective_rate <= 0) continue;

            double edge_cost = -std::log(effective_rate);
            double new_cost = current.cost + edge_cost;

            // Create new path
            std::vector<std::string> new_path = current.path;
            new_path.push_back(edge.to);

            pq.push({edge.to, new_cost, new_path});
        }
    }

    // No path found
    return std::nullopt;
}

std::optional<ConversionRoute> RoutingEngine::path_to_route(
    const std::vector<std::string>& path,
    const RateGraph& graph)
{
    if (path.size() < 2) return std::nullopt;

    ConversionRoute route;
    route.hop_count = static_cast<int>(path.size()) - 1;
    route.total_rate = 1.0;
    route.total_fee_bps = 0.0;
    route.slippage_bps = route.hop_count * 10;  // ~0.1% slippage per hop

    for (size_t i = 0; i < path.size() - 1; ++i) {
        const std::string& from_asset = path[i];
        const std::string& to_asset = path[i + 1];

        // Find edge in graph
        auto it = graph.find(from_asset);
        if (it == graph.end()) return std::nullopt;

        const RateEdge* best_edge = nullptr;
        for (const auto& edge : it->second) {
            if (edge.to == to_asset) {
                if (!best_edge || edge.rate > best_edge->rate) {
                    best_edge = &edge;
                }
            }
        }

        if (!best_edge) return std::nullopt;

        // Create hop
        RouteHop hop;
        hop.from_asset = from_asset;
        hop.to_asset = to_asset;
        hop.rate = best_edge->rate;
        hop.fee_bps = best_edge->fee_bps;
        hop.provider = best_edge->provider;

        route.hops.push_back(hop);
        route.total_rate *= hop.rate;
        route.total_fee_bps += hop.fee_bps;
    }

    return route;
}

std::optional<ConversionRoute> RoutingEngine::find_best_route(
    const std::string& from,
    const std::string& to,
    const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers,
    int max_hops)
{
    g_logger.info("[RoutingEngine] Finding best route: " + from + " → " + to);

    // Build rate graph from all providers
    auto graph = build_rate_graph(providers);

    if (graph.empty()) {
        g_logger.warning("[RoutingEngine] Rate graph is empty, no providers available");
        return std::nullopt;
    }

    g_logger.info("[RoutingEngine] Built graph with " +
                  std::to_string(graph.size()) + " nodes");

    // Find path using Dijkstra
    auto path = find_path(graph, from, to, max_hops);

    if (!path) {
        g_logger.warning("[RoutingEngine] No path found from " + from + " to " + to);
        return std::nullopt;
    }

    // Convert path to route
    auto route = path_to_route(*path, graph);

    if (route) {
        g_logger.info("[RoutingEngine] Best route: " + route->description() +
                     " (rate: " + std::to_string(route->total_rate) +
                     ", effective: " + std::to_string(route->effective_rate()) + ")");
    }

    return route;
}

std::vector<ConversionRoute> RoutingEngine::find_all_routes(
    const std::string& from,
    const std::string& to,
    const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers,
    int max_hops)
{
    std::vector<ConversionRoute> routes;

    // Build graph
    auto graph = build_rate_graph(providers);
    if (graph.empty()) return routes;

    // Try direct path first
    auto direct = find_path(graph, from, to, 1);
    if (direct) {
        auto route = path_to_route(*direct, graph);
        if (route) routes.push_back(*route);
    }

    // Try paths through common intermediates
    for (const auto& intermediate : intermediate_assets()) {
        if (intermediate == from || intermediate == to) continue;

        // Try from → intermediate → to
        auto path1 = find_path(graph, from, intermediate, max_hops / 2);
        auto path2 = find_path(graph, intermediate, to, max_hops / 2);

        if (path1 && path2) {
            // Combine paths
            std::vector<std::string> combined = *path1;
            combined.insert(combined.end(), path2->begin() + 1, path2->end());

            auto route = path_to_route(combined, graph);
            if (route) routes.push_back(*route);
        }
    }

    // Sort by effective rate (best first)
    std::sort(routes.begin(), routes.end(), [](const auto& a, const auto& b) {
        return a.effective_rate() > b.effective_rate();
    });

    // Remove duplicates based on description
    std::set<std::string> seen;
    routes.erase(std::remove_if(routes.begin(), routes.end(), [&seen](const auto& r) {
        return !seen.insert(r.description()).second;
    }), routes.end());

    return routes;
}

} // namespace bridge
} // namespace dinero
