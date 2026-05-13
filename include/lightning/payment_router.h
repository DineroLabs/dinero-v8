#pragma once

#include "lightning/lightning_types.h"
#include "lightning/onion_error.h"
#include "din_json.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>

// Forward declaration (DaemonContext is in global namespace)
struct DaemonContext;

// Forward declaration for time oracle (::lightning namespace)
namespace lightning {
    class ITimeOracle;
}

namespace dinero {
namespace lightning {

// Forward declarations
class ChannelManager;
class HTLCManager;

/**
 * @class PaymentRouter
 * @brief Routes Lightning payments across the network
 *
 * Phase 7.5: Payment pathfinding and routing
 *
 * Responsibilities:
 * - Find payment routes from source to destination
 * - Calculate routing fees and timelock deltas
 * - Implement Dijkstra's algorithm for shortest path
 * - Manage channel liquidity and capacity constraints
 * - Track failed routes and implement routing hints
 * - Multi-path payments (split large payments)
 *
 * Routing Strategy:
 * - Minimize total fee (primary)
 * - Minimize total timelock (secondary)
 * - Prefer channels with sufficient liquidity
 * - Avoid recently failed routes
 *
 * Thread Safety: All public methods are thread-safe
 */
class PaymentRouter {
public:
    /**
     * @brief Construct PaymentRouter
     * @param ctx Reference to DaemonContext
     * @param channel_mgr Reference to ChannelManager for channel graph
     * @param htlc_mgr Reference to HTLCManager for payment execution
     * @param time_oracle Time oracle for deterministic timestamps (Phase 8.5)
     */
    PaymentRouter(
        DaemonContext& ctx,
        ChannelManager& channel_mgr,
        HTLCManager& htlc_mgr,
        ::lightning::ITimeOracle* time_oracle
    );
    ~PaymentRouter();

    // ═══════════════════════════════════════════════════════════════════════════
    // Public Types (for RPC access)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @struct RouteFailure
     * @brief Records a failed payment route with BOLT #4 failure information
     */
    struct RouteFailure {
        std::vector<std::string> channel_ids;  // Channel IDs in route
        FailureCode code;                      // BOLT #4 failure code
        size_t failing_hop_index;              // Which hop failed (0-based)
        std::string reason;                    // Human-readable description
        uint64_t failed_at;                    // Unix timestamp
    };

    /**
     * @struct ChannelPenalty
     * @brief Temporary cost penalty applied to a channel after failure
     */
    struct ChannelPenalty {
        std::string channel_id;
        FailureCode code;
        uint64_t penalty_muna;     // Cost penalty added to routing
        uint64_t expires_at;        // Unix timestamp when penalty expires
    };

    /**
     * @struct NodeBlacklist
     * @brief Permanent exclusion of a node from routing
     */
    struct NodeBlacklist {
        std::string node_id;
        FailureCode code;
        uint64_t blacklisted_at;    // Unix timestamp
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // Route Finding
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Find route from local node to destination
     *
     * Uses Dijkstra's algorithm to find the optimal route considering:
     * - Channel capacity
     * - Routing fees
     * - Timelock deltas
     * - Failed route history
     *
     * @param destination_node_id Target node pubkey (33-byte hex)
     * @param amount_msats Payment amount in milliuna
     * @param max_fee_msats Maximum acceptable routing fee (optional)
     * @param max_hops Maximum route length (default: 20)
     * @return Result<Route> Route or error if no path found
     */
    Result<Route> findRoute(
        const std::string& destination_node_id,
        uint64_t amount_msats,
        uint64_t max_fee_msats = 0,  // 0 = no limit
        uint32_t max_hops = constants::MAX_PAYMENT_HOPS
    );

    /**
     * @brief Find multiple routes for multi-path payment
     *
     * Splits a large payment across multiple routes to avoid
     * capacity constraints.
     *
     * @param destination_node_id Target node pubkey
     * @param total_amount_msats Total payment amount
     * @param max_fee_msats Maximum total routing fee
     * @param max_paths Maximum number of paths (default: 4)
     * @return Result<std::vector<Route>> Routes or error
     */
    Result<std::vector<Route>> findMultiPathRoutes(
        const std::string& destination_node_id,
        uint64_t total_amount_msats,
        uint64_t max_fee_msats = 0,
        uint32_t max_paths = 4
    );

    /**
     * @brief Validate route feasibility
     *
     * Checks if a route is still valid:
     * - All channels are still open
     * - Sufficient liquidity in each channel
     * - Timelocks are acceptable
     *
     * @param route Route to validate
     * @return Result<void> Success or error with reason
     */
    Result<void> validateRoute(const Route& route);

    // ═══════════════════════════════════════════════════════════════════════════
    // Payment Execution
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Send payment along route
     *
     * Executes payment by creating HTLCs along the route.
     * Waits for settlement or failure.
     *
     * @param route Payment route
     * @param payment_hash Payment hash (32 bytes)
     * @param timeout_ms Payment timeout in milliseconds (default: 60000 = 1 minute)
     * @return Result<std::vector<uint8_t>> Preimage if successful, or error
     */
    Result<std::vector<uint8_t>> sendPayment(
        const Route& route,
        const std::vector<uint8_t>& payment_hash,
        uint64_t timeout_ms = 60000
    );

    /**
     * @brief Send payment with automatic retry on failure
     *
     * Attempts to send payment with automatic retries if the payment fails.
     * On failure, finds alternative routes and retries up to max_attempts.
     *
     * @param destination_node_id Destination node pubkey (33-byte hex)
     * @param amount_muna Payment amount in milli-una
     * @param payment_hash Payment hash (32 bytes)
     * @param max_attempts Maximum number of payment attempts (default: 3)
     * @param timeout_ms Timeout per attempt in milliseconds (default: 60000 = 1 minute)
     * @return Result<std::vector<uint8_t>> Preimage if successful, or error after all retries exhausted
     */
    Result<std::vector<uint8_t>> sendPaymentWithRetry(
        const std::string& destination_node_id,
        uint64_t amount_muna,
        const std::vector<uint8_t>& payment_hash,
        uint32_t max_attempts = 3,
        uint64_t timeout_ms = 60000
    );

    /**
     * @brief Send multi-path payment
     *
     * Executes payment across multiple routes simultaneously.
     *
     * @param routes Payment routes
     * @param payment_hash Payment hash (32 bytes)
     * @param timeout_ms Payment timeout
     * @return Result<std::vector<uint8_t>> Preimage if successful, or error
     */
    Result<std::vector<uint8_t>> sendMultiPathPayment(
        const std::vector<Route>& routes,
        const std::vector<uint8_t>& payment_hash,
        uint64_t timeout_ms = 60000
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Graph Management
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Update channel graph with new channel
     *
     * Called when a new channel is opened or discovered.
     *
     * @param channel Channel to add to graph
     * @return Result<void> Success or error
     */
    Result<void> addChannelToGraph(const Channel& channel);

    /**
     * @brief Remove channel from graph
     *
     * Called when a channel is closed.
     *
     * @param channel_id Channel to remove
     * @return Result<void> Success or error
     */
    Result<void> removeChannelFromGraph(const std::string& channel_id);

    /**
     * @brief Update channel liquidity estimate
     *
     * Updates our estimate of channel liquidity based on:
     * - Successful payments
     * - Failed payments (insufficient capacity)
     * - Channel updates from network gossip
     *
     * @param channel_id Channel to update
     * @param estimated_balance_msats Estimated balance
     * @return Result<void> Success or error
     */
    Result<void> updateChannelLiquidity(
        const std::string& channel_id,
        uint64_t estimated_balance_msats
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Route Failure Tracking
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Mark route as failed (legacy string-based)
     *
     * Records route failure to avoid retrying the same path immediately.
     * Failed routes are penalized in route finding for a decay period.
     *
     * @param route Failed route
     * @param reason Failure reason
     */
    void markRouteFailed(const Route& route, const std::string& reason);

    /**
     * @brief Mark route as failed with BOLT #4 failure code
     *
     * Records route failure with proper failure classification for intelligent retry.
     * Applies temporary penalties or permanent blacklists based on failure code.
     *
     * @param route Failed route
     * @param code BOLT #4 failure code
     * @param failing_hop_index Index of hop that failed (0-based)
     */
    void markRouteFailedWithCode(
        const Route& route,
        FailureCode code,
        size_t failing_hop_index
    );

    /**
     * @brief Mark route as successful
     *
     * Records successful payment to improve routing heuristics.
     *
     * @param route Successful route
     */
    void markRouteSuccessful(const Route& route);

    /**
     * @brief Clear expired route failures
     *
     * Removes old route failure records to allow retrying.
     *
     * @param max_age_seconds Maximum age of failures to keep (default: 3600 = 1 hour)
     */
    void clearExpiredFailures(uint64_t max_age_seconds = 3600);

    // ═══════════════════════════════════════════════════════════════════════════
    // Statistics and Queries
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get routing statistics
     * @return din::Json Statistics (total routes found, success rate, etc.)
     */
    din::Json getStats() const;

    /**
     * @brief Get channel graph info
     * @return din::Json Graph info (node count, channel count, etc.)
     */
    din::Json getGraphInfo() const;

    /**
     * @brief Get all recorded route failures
     * @return std::vector<RouteFailure> List of failed routes with BOLT #4 codes
     */
    std::vector<RouteFailure> getFailedRoutes() const;

    /**
     * @brief Get all active channel penalties
     * @return std::vector<ChannelPenalty> List of channels with temporary penalties
     */
    std::vector<ChannelPenalty> getChannelPenalties() const;

    /**
     * @brief Get all blacklisted nodes
     * @return std::vector<NodeBlacklist> List of permanently blacklisted nodes
     */
    std::vector<NodeBlacklist> getNodeBlacklist() const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext& m_daemon_ctx;                           // For block height, etc.
    ChannelManager& m_channel_mgr;                         // Channel graph source
    HTLCManager& m_htlc_mgr;                               // Payment execution
    ::lightning::ITimeOracle* m_time_oracle;               // Phase 8.5: Deterministic time (NOT owned)

    // Channel graph (adjacency list representation)
    struct ChannelEdge {
        std::string channel_id;
        std::string peer_node_id;
        uint64_t capacity_una;
        uint64_t estimated_liquidity_muna;  // Our estimate of available balance
        uint64_t base_fee_muna;
        uint64_t fee_rate_ppm;               // Parts per million
        uint32_t cltv_delta;
        bool is_active;
        uint64_t last_update;
    };

    mutable std::mutex m_graph_mutex;
    std::map<std::string, std::vector<ChannelEdge>> m_channel_graph;  // node_id → outgoing edges

    // Route failure tracking (BOLT #4 enhanced) - structs defined in public section
    mutable std::mutex m_failures_mutex;
    std::vector<RouteFailure> m_failed_routes;

    // Channel penalties (temporary failures) - struct defined in public section
    mutable std::mutex m_penalties_mutex;
    std::vector<ChannelPenalty> m_channel_penalties;

    // Node blacklist (permanent failures) - struct defined in public section
    mutable std::mutex m_blacklist_mutex;
    std::vector<NodeBlacklist> m_node_blacklist;

    // Statistics
    struct Stats {
        uint64_t routes_found = 0;
        uint64_t routes_failed = 0;
        uint64_t payments_sent = 0;
        uint64_t payments_succeeded = 0;
        uint64_t payments_failed = 0;
        uint64_t total_amount_sent_msats = 0;
        uint64_t total_fees_paid_msats = 0;
    };
    mutable Stats m_stats;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Routing Algorithms
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Dijkstra's shortest path algorithm
     *
     * Finds the lowest-cost route from source to destination.
     *
     * @param source_node_id Source node (our node)
     * @param dest_node_id Destination node
     * @param amount_msats Payment amount
     * @param max_hops Maximum route length
     * @return Result<Route> Route or error if no path found
     */
    Result<Route> dijkstra(
        const std::string& source_node_id,
        const std::string& dest_node_id,
        uint64_t amount_msats,
        uint32_t max_hops
    );

    /**
     * @brief Calculate route cost
     *
     * Cost function for routing:
     * - Primary: Total fee
     * - Secondary: Total timelock
     * - Penalties: Failed routes, low liquidity
     *
     * @param edge Channel edge to traverse
     * @param amount_msats Payment amount through this edge
     * @return uint64_t Cost in milliuna (with penalties)
     */
    uint64_t calculateEdgeCost(const ChannelEdge& edge, uint64_t amount_msats) const;

    /**
     * @brief Calculate routing fee for edge
     *
     * Fee = base_fee + (amount * fee_rate / 1,000,000)
     *
     * @param edge Channel edge
     * @param amount_msats Payment amount
     * @return uint64_t Fee in milliuna
     */
    uint64_t calculateFee(const ChannelEdge& edge, uint64_t amount_msats) const;

    /**
     * @brief Check if edge has sufficient liquidity
     *
     * @param edge Channel edge
     * @param amount_msats Required amount
     * @return bool True if edge can forward payment
     */
    bool hasLiquidity(const ChannelEdge& edge, uint64_t amount_msats) const;

    /**
     * @brief Check if route is in failure list
     *
     * @param route Route to check
     * @return bool True if route recently failed
     */
    bool isRouteFailed(const Route& route) const;

    /**
     * @brief Build channel graph from ChannelManager
     *
     * Reconstructs the routing graph from open channels.
     */
    void rebuildChannelGraph();

    /**
     * @brief Get our node ID
     * @return std::string Our node pubkey (33-byte hex)
     */
    std::string getOurNodeId() const;

    /**
     * @brief Check if channel is penalized
     * @param channel_id Channel to check
     * @return bool True if channel has active penalty
     */
    bool isChannelPenalized(const std::string& channel_id) const;

    /**
     * @brief Get channel penalty amount
     * @param channel_id Channel to check
     * @return uint64_t Penalty in muna (0 if no penalty)
     */
    uint64_t getChannelPenalty(const std::string& channel_id) const;

    /**
     * @brief Check if node is blacklisted
     * @param node_id Node to check
     * @return bool True if node is blacklisted
     */
    bool isNodeBlacklisted(const std::string& node_id) const;

    /**
     * @brief Penalize channel for temporary failure
     * @param channel_id Channel to penalize
     * @param code Failure code
     * @param penalty_duration_seconds Duration of penalty (default: 300 = 5 minutes)
     */
    void penalizeChannel(
        const std::string& channel_id,
        FailureCode code,
        uint64_t penalty_duration_seconds = 300
    );

    /**
     * @brief Blacklist node for permanent failure
     * @param node_id Node to blacklist
     * @param code Failure code
     */
    void blacklistNode(const std::string& node_id, FailureCode code);

    /**
     * @brief Clear expired channel penalties
     */
    void clearExpiredPenalties();

    /**
     * @brief Classify failure as retryable based on FailureCode
     * @param code BOLT #4 failure code
     * @return bool True if failure is retryable
     */
    static bool isRetryableFailure(FailureCode code);
};

} // namespace lightning
} // namespace dinero
