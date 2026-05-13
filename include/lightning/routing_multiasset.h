#pragma once

/**
 * Phase 31: Multi-Asset Lightning Settlement Layer
 *
 * Multi-Asset Routing Engine
 *
 * Extends Lightning routing to support:
 * - Per-asset liquidity tracking
 * - Multi-dimensional pathfinding
 * - Swap-enabled routing (asset conversion during routing)
 * - DEX liquidity integration
 */

#include "lightning/lightning_types.h"
#include "lightning/asset_channel.h"
#include "lightning/asset_htlc.h"
#include "lightning/swap_htlc.h"
#include "assets/asset_id.h"
#include <map>
#include <set>
#include <vector>
#include <optional>
#include <functional>
#include <memory>

namespace dinero {
namespace lightning {

using assets::AssetID;

// ============================================================================
// Node Asset Capabilities
// ============================================================================

/**
 * @brief Asset support advertised by a node
 */
struct NodeAssetCapability {
    std::string node_id;
    AssetID asset_id;

    // Liquidity information
    uint64_t total_capacity;                 // Total across all channels
    uint64_t available_capacity;             // Available for routing
    uint64_t max_htlc_size;                  // Maximum single HTLC

    // Routing fees for this asset
    uint64_t base_fee;                       // Base fee in asset units
    uint64_t fee_rate_ppm;                   // Proportional fee (parts per million)

    // CLTV requirements
    uint32_t min_cltv_delta;

    // Swap capabilities
    bool can_swap_from;                      // Can accept this asset in swaps
    bool can_swap_to;                        // Can provide this asset in swaps

    // Timestamp
    uint64_t last_update;
};

/**
 * @brief Swap rate advertised by a node
 */
struct NodeSwapRate {
    std::string node_id;
    AssetID asset_from;
    AssetID asset_to;

    // Rate (asset_to per asset_from)
    uint64_t rate_numerator;
    uint64_t rate_denominator;

    // Limits
    uint64_t min_amount;
    uint64_t max_amount;

    // Fees (in addition to exchange rate)
    uint64_t swap_fee_bps;                   // Basis points

    // Validity
    uint64_t valid_until;
    uint64_t last_update;
};

/**
 * @brief Complete asset capabilities for a channel
 */
struct ChannelAssetInfo {
    std::string channel_id;
    std::string node_id_1;
    std::string node_id_2;

    // Per-asset liquidity
    struct AssetLiquidity {
        AssetID asset_id;
        uint64_t capacity_1_to_2;            // Liquidity from node1 to node2
        uint64_t capacity_2_to_1;            // Liquidity from node2 to node1
        uint64_t base_fee;
        uint64_t fee_rate_ppm;
    };
    std::vector<AssetLiquidity> asset_liquidity;

    // Channel-level swap support
    bool supports_swaps;
    std::vector<NodeSwapRate> swap_rates;

    // Last update
    uint64_t last_update;
};

// ============================================================================
// Multi-Asset Route
// ============================================================================

/**
 * @brief Hop in a multi-asset route
 *
 * Each hop may optionally perform an asset conversion.
 */
struct MultiAssetHop {
    std::string channel_id;
    std::string node_id;                     // Node at this hop

    // Asset flow
    AssetID asset_in;                        // Asset entering this hop
    uint64_t amount_in;
    AssetID asset_out;                       // Asset leaving this hop (may differ)
    uint64_t amount_out;

    // Fees
    uint64_t routing_fee;                    // Standard routing fee
    uint64_t swap_fee;                       // If conversion happens

    // Timelock
    uint32_t cltv_delta;

    // Is this hop a swap?
    bool is_swap;
    std::optional<NodeSwapRate> swap_rate;

    MultiAssetHop()
        : amount_in(0), amount_out(0), routing_fee(0), swap_fee(0),
          cltv_delta(0), is_swap(false) {
        asset_in.fill(0);
        asset_out.fill(0);
    }

    uint64_t totalFee() const { return routing_fee + swap_fee; }
};

/**
 * @brief Complete multi-asset payment route
 */
struct MultiAssetRoute {
    std::vector<MultiAssetHop> hops;

    // Source info
    AssetID source_asset;
    uint64_t source_amount;

    // Destination info
    AssetID dest_asset;
    uint64_t dest_amount;

    // Aggregated costs
    uint64_t total_routing_fees;
    uint64_t total_swap_fees;
    uint32_t total_cltv;

    // Number of conversions in route
    uint32_t num_conversions;

    // Route reliability score (0-100)
    uint32_t reliability_score;

    MultiAssetRoute()
        : source_amount(0), dest_amount(0), total_routing_fees(0),
          total_swap_fees(0), total_cltv(0), num_conversions(0),
          reliability_score(0) {
        source_asset.fill(0);
        dest_asset.fill(0);
    }

    // Total fees
    uint64_t totalFees() const { return total_routing_fees + total_swap_fees; }

    // Validate route consistency
    bool validate() const;

    // Check if route involves conversions
    bool hasConversions() const { return num_conversions > 0; }
};

// ============================================================================
// Routing Preferences
// ============================================================================

/**
 * @brief Preferences for route finding
 */
struct RoutingPreferences {
    // Cost weights (0-100, must sum to 100)
    uint32_t fee_weight;                     // Prioritize low fees
    uint32_t latency_weight;                 // Prioritize low CLTV
    uint32_t reliability_weight;             // Prioritize reliable channels

    // Conversion preferences
    bool allow_conversions;                  // Allow asset swaps during routing
    uint32_t max_conversions;                // Maximum number of swaps
    uint32_t max_slippage_bps;               // Max acceptable slippage

    // Route constraints
    uint32_t max_hops;
    uint32_t max_cltv;
    uint64_t max_fee;

    // Excluded nodes/channels
    std::set<std::string> excluded_nodes;
    std::set<std::string> excluded_channels;

    // Preferred nodes (bonus in scoring)
    std::set<std::string> preferred_nodes;

    RoutingPreferences()
        : fee_weight(50), latency_weight(25), reliability_weight(25),
          allow_conversions(true), max_conversions(2), max_slippage_bps(100),
          max_hops(20), max_cltv(1008), max_fee(UINT64_MAX) {}
};

// ============================================================================
// Routing Algorithm Types
// ============================================================================

/**
 * @brief Routing algorithm selection
 */
enum class RoutingAlgorithm {
    DIJKSTRA,            // Standard single-asset shortest path
    MULTI_DIM_DIJKSTRA,  // Multi-dimensional for multi-asset
    A_STAR,              // A* with heuristic
    BELLMAN_FORD,        // For negative edges (rare)
    EPPSTEIN_K_PATHS     // K-shortest paths
};

// ============================================================================
// Multi-Asset Router Interface
// ============================================================================

/**
 * @brief Interface for multi-asset payment routing
 */
class IMultiAssetRouter {
public:
    virtual ~IMultiAssetRouter() = default;

    // Route finding
    virtual Result<MultiAssetRoute> findRoute(
        const std::string& source_node,
        const std::string& dest_node,
        const AssetID& source_asset,
        uint64_t amount,
        const AssetID& dest_asset,
        const RoutingPreferences& prefs = RoutingPreferences()) = 0;

    virtual Result<std::vector<MultiAssetRoute>> findMultipleRoutes(
        const std::string& source_node,
        const std::string& dest_node,
        const AssetID& source_asset,
        uint64_t amount,
        const AssetID& dest_asset,
        uint32_t num_routes,
        const RoutingPreferences& prefs = RoutingPreferences()) = 0;

    // Same-asset routing (backward compatible)
    virtual Result<MultiAssetRoute> findSameAssetRoute(
        const std::string& source_node,
        const std::string& dest_node,
        const AssetID& asset,
        uint64_t amount,
        const RoutingPreferences& prefs = RoutingPreferences()) = 0;

    // Swap-only routing (find best conversion path)
    virtual Result<SwapRoute> findSwapRoute(
        const AssetID& asset_from,
        const AssetID& asset_to,
        uint64_t amount,
        const RoutingPreferences& prefs = RoutingPreferences()) = 0;

    // Graph updates
    virtual void updateChannelInfo(const ChannelAssetInfo& info) = 0;
    virtual void updateNodeCapability(const NodeAssetCapability& cap) = 0;
    virtual void updateSwapRate(const NodeSwapRate& rate) = 0;
    virtual void removeChannel(const std::string& channel_id) = 0;
    virtual void removeNode(const std::string& node_id) = 0;

    // Queries
    virtual std::vector<AssetID> getSupportedAssets() const = 0;
    virtual std::vector<std::string> getNodesWithAsset(const AssetID& asset_id) const = 0;
    virtual std::vector<NodeSwapRate> getSwapRatesForAsset(const AssetID& asset_id) const = 0;
    virtual uint64_t getTotalLiquidity(const AssetID& asset_id) const = 0;
};

// ============================================================================
// Multi-Asset Network Graph
// ============================================================================

/**
 * @brief Network graph optimized for multi-asset routing
 */
class MultiAssetNetworkGraph {
public:
    MultiAssetNetworkGraph();
    ~MultiAssetNetworkGraph();

    // Graph construction
    void addChannel(const ChannelAssetInfo& info);
    void removeChannel(const std::string& channel_id);
    void updateChannel(const ChannelAssetInfo& info);

    void addNode(const std::string& node_id);
    void removeNode(const std::string& node_id);
    void updateNodeCapabilities(const std::string& node_id,
                                 const std::vector<NodeAssetCapability>& caps);
    void updateNodeSwapRates(const std::string& node_id,
                              const std::vector<NodeSwapRate>& rates);

    // Queries
    std::vector<std::string> getNeighbors(const std::string& node_id) const;
    std::vector<std::string> getChannelsBetween(const std::string& node1,
                                                  const std::string& node2) const;
    std::optional<ChannelAssetInfo> getChannelInfo(const std::string& channel_id) const;

    // Asset-specific queries
    std::vector<std::string> getChannelsWithAsset(const AssetID& asset_id) const;
    uint64_t getChannelCapacity(const std::string& channel_id,
                                 const AssetID& asset_id,
                                 const std::string& direction_node) const;

    // Swap queries
    std::vector<std::string> getSwapNodes(const AssetID& from,
                                           const AssetID& to) const;
    std::optional<NodeSwapRate> getBestSwapRate(const AssetID& from,
                                                 const AssetID& to) const;

    // Statistics
    size_t numNodes() const;
    size_t numChannels() const;
    size_t numAssets() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Pathfinding Algorithms
// ============================================================================

/**
 * @brief Multi-dimensional Dijkstra for multi-asset routing
 *
 * Finds shortest path considering:
 * - Multiple asset types
 * - Conversion costs at swap points
 * - Per-asset fees
 */
class MultiDimDijkstra {
public:
    explicit MultiDimDijkstra(const MultiAssetNetworkGraph& graph);

    Result<MultiAssetRoute> findPath(
        const std::string& source,
        const std::string& dest,
        const AssetID& source_asset,
        uint64_t amount,
        const AssetID& dest_asset,
        const RoutingPreferences& prefs);

private:
    const MultiAssetNetworkGraph& graph_;

    // Cost function for edge traversal
    uint64_t edgeCost(
        const std::string& from_node,
        const std::string& to_node,
        const std::string& channel_id,
        const AssetID& asset,
        uint64_t amount,
        const RoutingPreferences& prefs) const;

    // Cost function for asset conversion
    uint64_t swapCost(
        const std::string& node,
        const AssetID& from_asset,
        const AssetID& to_asset,
        uint64_t amount,
        const RoutingPreferences& prefs) const;
};

// ============================================================================
// Payment Splitter
// ============================================================================

/**
 * @brief Split large payments across multiple routes
 */
class PaymentSplitter {
public:
    struct SplitResult {
        std::vector<MultiAssetRoute> routes;
        std::vector<uint64_t> amounts;       // Amount per route
        uint64_t total_fees;
        bool is_complete;                    // All amount routed
    };

    explicit PaymentSplitter(IMultiAssetRouter& router);

    Result<SplitResult> splitPayment(
        const std::string& source_node,
        const std::string& dest_node,
        const AssetID& source_asset,
        uint64_t total_amount,
        const AssetID& dest_asset,
        uint32_t max_splits,
        const RoutingPreferences& prefs);

private:
    IMultiAssetRouter& router_;
};

// ============================================================================
// Route Scorer
// ============================================================================

/**
 * @brief Score routes for comparison
 */
class RouteScorer {
public:
    explicit RouteScorer(const RoutingPreferences& prefs);

    // Calculate overall score (higher is better)
    uint64_t score(const MultiAssetRoute& route) const;

    // Compare two routes
    bool isBetter(const MultiAssetRoute& a, const MultiAssetRoute& b) const;

    // Sort routes by score (best first)
    void sortRoutes(std::vector<MultiAssetRoute>& routes) const;

private:
    const RoutingPreferences& prefs_;

    uint64_t feeScore(const MultiAssetRoute& route) const;
    uint64_t latencyScore(const MultiAssetRoute& route) const;
    uint64_t reliabilityScore(const MultiAssetRoute& route) const;
};

// ============================================================================
// Probe Manager
// ============================================================================

/**
 * @brief Probe channels to gather liquidity information
 */
class LiquidityProber {
public:
    struct ProbeResult {
        std::string channel_id;
        AssetID asset_id;
        uint64_t available_capacity;
        uint64_t estimated_max_htlc;
        bool success;
        std::string error;
    };

    // Probe a specific channel
    Result<ProbeResult> probeChannel(
        const std::string& channel_id,
        const AssetID& asset_id,
        uint64_t probe_amount);

    // Probe all channels on a path
    Result<std::vector<ProbeResult>> probePath(const MultiAssetRoute& route);

    // Update graph with probe results
    void applyProbeResults(MultiAssetNetworkGraph& graph,
                           const std::vector<ProbeResult>& results);
};

} // namespace lightning
} // namespace dinero
