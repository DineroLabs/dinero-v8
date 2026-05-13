#pragma once

/**
 * Phase 31: Multi-Asset Lightning Settlement Layer
 *
 * Multi-Asset Channel Types and State Machine
 *
 * This extends standard Lightning channels to track multiple asset balances
 * within a single channel, enabling:
 * - Multi-asset capacity channels
 * - In-channel asset swaps
 * - Cross-asset routing
 * - DEX liquidity provision
 */

#include "lightning/lightning_types.h"
#include "assets/asset_id.h"
#include <array>
#include <map>
#include <vector>
#include <optional>

namespace dinero {
namespace lightning {

// Use AssetID from the asset layer
using assets::AssetID;

// ============================================================================
// Multi-Asset Balance Types
// ============================================================================

/**
 * @brief Balance for a single asset within a channel
 *
 * Tracks local/remote balances and state commitment for one asset type.
 */
struct AssetBalance {
    AssetID asset_id;                    // Asset identifier (32 bytes)
    uint64_t amount_local;               // Our balance in base units
    uint64_t amount_remote;              // Peer's balance in base units
    std::array<uint8_t, 32> state_hash;  // Taproot commitment for this asset
    uint64_t reserved_local;             // Amount reserved in pending HTLCs (ours)
    uint64_t reserved_remote;            // Amount reserved in pending HTLCs (peer's)

    AssetBalance()
        : amount_local(0), amount_remote(0),
          reserved_local(0), reserved_remote(0) {
        asset_id.fill(0);
        state_hash.fill(0);
    }

    // Total capacity for this asset
    uint64_t totalCapacity() const {
        return amount_local + amount_remote;
    }

    // Available (non-reserved) balance
    uint64_t availableLocal() const {
        return amount_local > reserved_local ? amount_local - reserved_local : 0;
    }

    uint64_t availableRemote() const {
        return amount_remote > reserved_remote ? amount_remote - reserved_remote : 0;
    }

    // Serialization
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetBalance> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Multi-asset channel extension
 *
 * Extends a standard Channel with multi-asset balance tracking.
 * A single channel can hold DIN plus multiple asset types.
 */
struct MultiAssetChannel {
    // Base channel ID (links to standard Channel)
    std::string channel_id;

    // Asset balances (asset_id -> balance)
    // Note: DIN (native) uses NullAssetID()
    std::map<AssetID, AssetBalance> asset_balances;

    // Master state commitment (root of all asset states)
    std::array<uint8_t, 32> master_state_root;

    // Asset state transition counter
    uint64_t asset_state_version;

    // Supported assets (advertised to peers)
    std::vector<AssetID> supported_assets;

    // DEX capability flags
    bool is_dex_channel;                 // Acts as liquidity router
    bool accepts_swaps;                  // Accepts swap HTLCs

    // Swap rate configuration (for DEX channels)
    struct SwapRate {
        AssetID asset_in;
        AssetID asset_out;
        uint64_t rate_numerator;         // Rate = numerator/denominator
        uint64_t rate_denominator;
        uint64_t min_amount;             // Minimum swap size
        uint64_t max_amount;             // Maximum swap size
        bool is_active;
    };
    std::vector<SwapRate> swap_rates;

    MultiAssetChannel()
        : asset_state_version(0), is_dex_channel(false), accepts_swaps(false) {
        master_state_root.fill(0);
    }

    // Check if channel supports an asset
    bool supportsAsset(const AssetID& asset_id) const;

    // Get balance for an asset (returns nullptr if not supported)
    const AssetBalance* getBalance(const AssetID& asset_id) const;
    AssetBalance* getBalance(const AssetID& asset_id);

    // Add asset support to channel (during open or update)
    bool addAsset(const AssetID& asset_id, uint64_t local_amount, uint64_t remote_amount);

    // Compute master state root from all asset states
    std::array<uint8_t, 32> computeMasterRoot() const;

    // Get swap rate for a pair (returns nullptr if not available)
    const SwapRate* getSwapRate(const AssetID& asset_in, const AssetID& asset_out) const;

    // Serialization
    std::vector<uint8_t> serialize() const;
    static std::optional<MultiAssetChannel> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Multi-Asset Channel Open Request
// ============================================================================

/**
 * @brief Request to open a multi-asset channel
 */
struct MultiAssetOpenRequest {
    std::string peer_node_id;            // Remote peer's node ID

    // Initial funding for each asset
    struct AssetFunding {
        AssetID asset_id;
        uint64_t local_amount;           // Our contribution
        uint64_t remote_amount;          // Expected from peer (for dual-funded)
    };
    std::vector<AssetFunding> asset_funding;

    // DIN funding (native currency for fees/anchors)
    uint64_t din_local_muna;
    uint64_t din_remote_muna;

    // Channel parameters
    uint32_t to_self_delay;              // CSV delay
    uint64_t dust_limit;

    // DEX configuration (optional)
    bool enable_dex;
    bool enable_swaps;
    std::vector<MultiAssetChannel::SwapRate> initial_rates;

    // Validate the request
    bool validate() const;
};

/**
 * @brief Response to multi-asset channel open
 */
struct MultiAssetOpenResponse {
    bool accepted;
    std::string channel_id;              // Assigned if accepted
    std::string error_message;           // If rejected

    // Negotiated parameters
    std::vector<AssetID> accepted_assets;
    uint32_t negotiated_to_self_delay;
    uint64_t negotiated_dust_limit;
};

// ============================================================================
// Multi-Asset Channel Update
// ============================================================================

/**
 * @brief Channel state update for multi-asset channels
 *
 * Represents a state transition that may affect multiple assets.
 */
struct MultiAssetUpdate {
    std::string channel_id;
    uint64_t update_number;              // Monotonic update counter

    // Previous state
    std::array<uint8_t, 32> prev_master_root;
    uint64_t prev_version;

    // New state
    std::array<uint8_t, 32> new_master_root;
    uint64_t new_version;

    // Balance changes by asset
    struct BalanceChange {
        AssetID asset_id;
        int64_t local_delta;             // Positive = increase, negative = decrease
        int64_t remote_delta;
    };
    std::vector<BalanceChange> balance_changes;

    // Applied HTLCs (both asset and swap)
    std::vector<std::string> applied_htlc_ids;

    // Signature over the update
    std::vector<uint8_t> signature;

    // CTV commitment hash (deterministic template)
    std::array<uint8_t, 32> ctv_hash;

    // Validate state transition
    bool validate() const;

    // Check conservation (no inflation)
    bool checkConservation() const;

    // Compute update hash
    std::array<uint8_t, 32> computeHash() const;
};

// ============================================================================
// DEX Channel Features
// ============================================================================

/**
 * @brief DEX liquidity pool within a channel
 *
 * Enables automated market making between two assets.
 */
struct InChannelPool {
    std::string channel_id;
    AssetID asset_a;
    AssetID asset_b;

    // Pool reserves (from channel balances)
    uint64_t reserve_a;
    uint64_t reserve_b;

    // Constant product (k = reserve_a * reserve_b)
    // Maintained across swaps

    // Fee configuration
    uint64_t fee_bps;                    // Fee in basis points (e.g., 30 = 0.3%)

    // Pool statistics
    uint64_t total_volume_a;             // Lifetime volume in asset A
    uint64_t total_volume_b;             // Lifetime volume in asset B
    uint64_t total_fees_a;               // Total fees collected in A
    uint64_t total_fees_b;               // Total fees collected in B

    InChannelPool()
        : reserve_a(0), reserve_b(0), fee_bps(30),
          total_volume_a(0), total_volume_b(0),
          total_fees_a(0), total_fees_b(0) {
        asset_a.fill(0);
        asset_b.fill(0);
    }

    // Calculate output for given input (constant product formula)
    uint64_t getAmountOut(const AssetID& asset_in, uint64_t amount_in) const;

    // Calculate required input for desired output
    uint64_t getAmountIn(const AssetID& asset_out, uint64_t amount_out) const;

    // Execute swap (updates reserves)
    bool executeSwap(const AssetID& asset_in, uint64_t amount_in, uint64_t min_amount_out);

    // Get current price (asset_b per asset_a)
    double getPrice() const;
};

// ============================================================================
// Multi-Asset Channel Manager Interface
// ============================================================================

/**
 * @brief Interface for multi-asset channel operations
 */
class IMultiAssetChannelManager {
public:
    virtual ~IMultiAssetChannelManager() = default;

    // Channel lifecycle
    virtual Result<std::string> openMultiAssetChannel(const MultiAssetOpenRequest& request) = 0;
    virtual Result<void> acceptMultiAssetChannel(const std::string& channel_id,
                                                  const MultiAssetOpenResponse& response) = 0;
    virtual Result<void> closeMultiAssetChannel(const std::string& channel_id) = 0;

    // Asset management
    virtual Result<void> addAssetToChannel(const std::string& channel_id,
                                            const AssetID& asset_id,
                                            uint64_t local_amount) = 0;
    virtual Result<void> removeAssetFromChannel(const std::string& channel_id,
                                                 const AssetID& asset_id) = 0;

    // State updates
    virtual Result<void> applyUpdate(const MultiAssetUpdate& update) = 0;
    virtual Result<MultiAssetUpdate> createUpdate(const std::string& channel_id,
                                                   const std::vector<MultiAssetUpdate::BalanceChange>& changes) = 0;

    // DEX operations
    virtual Result<void> enableDEX(const std::string& channel_id) = 0;
    virtual Result<void> setSwapRate(const std::string& channel_id,
                                      const MultiAssetChannel::SwapRate& rate) = 0;
    virtual Result<InChannelPool> createPool(const std::string& channel_id,
                                              const AssetID& asset_a,
                                              const AssetID& asset_b,
                                              uint64_t reserve_a,
                                              uint64_t reserve_b) = 0;

    // Queries
    virtual std::optional<MultiAssetChannel> getMultiAssetChannel(const std::string& channel_id) const = 0;
    virtual std::vector<std::string> getChannelsWithAsset(const AssetID& asset_id) const = 0;
    virtual std::vector<std::string> getDEXChannels() const = 0;
};

// ============================================================================
// Utility Functions
// ============================================================================

// Check if an AssetID is the native asset (DIN)
inline bool isNativeAsset(const AssetID& asset_id) {
    return assets::IsNativeAsset(asset_id);
}

// Get native asset ID
inline AssetID nativeAssetID() {
    return assets::NullAssetID();
}

} // namespace lightning
} // namespace dinero
