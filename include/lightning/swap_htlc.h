#pragma once

/**
 * Phase 31: Multi-Asset Lightning Settlement Layer
 *
 * Swap HTLC - Cross-Asset Atomic Swaps
 *
 * Enables trustless exchange between different asset types:
 * - DIN <-> Stablecoins
 * - DIN <-> Tokens
 * - Token <-> Token
 *
 * Uses a single hashlock to atomically release both sides of a swap,
 * enforced by CTV templates and CCV contract logic.
 */

#include "lightning/lightning_types.h"
#include "lightning/asset_channel.h"
#include "lightning/asset_htlc.h"
#include "assets/asset_id.h"
#include <array>
#include <vector>
#include <optional>

namespace dinero {
namespace lightning {

using assets::AssetID;

// ============================================================================
// Swap Rate Types
// ============================================================================

/**
 * @brief Rate source for swap pricing
 */
enum class RateSource : uint8_t {
    FIXED = 0,           // Fixed rate set by channel operator
    ORACLE = 1,          // Rate from signed oracle
    POOL = 2,            // Rate from in-channel AMM pool
    NEGOTIATED = 3       // Rate negotiated between parties
};

inline std::string rateSourceToString(RateSource source) {
    switch (source) {
        case RateSource::FIXED: return "FIXED";
        case RateSource::ORACLE: return "ORACLE";
        case RateSource::POOL: return "POOL";
        case RateSource::NEGOTIATED: return "NEGOTIATED";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Signed oracle rate for swap pricing
 */
struct OracleRate {
    AssetID base_asset;                      // e.g., DIN
    AssetID quote_asset;                     // e.g., USDS
    uint64_t rate_numerator;                 // Price = num/denom
    uint64_t rate_denominator;
    uint64_t timestamp;                      // Rate timestamp
    uint64_t valid_until;                    // Expiry timestamp

    // Oracle signature (BIP340 Schnorr)
    std::array<uint8_t, 32> oracle_pubkey;
    std::array<uint8_t, 64> signature;

    // Validate oracle signature
    bool verifySignature() const;

    // Calculate output amount for given input
    uint64_t getOutputAmount(uint64_t input_amount, bool base_to_quote) const;

    std::vector<uint8_t> serialize() const;
    static std::optional<OracleRate> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Swap HTLC Structure
// ============================================================================

/**
 * @brief Cross-asset atomic swap HTLC
 *
 * Atomically exchanges one asset for another using a single hashlock.
 * Both legs are released when the preimage is revealed.
 */
struct SwapHTLC {
    // Identification
    std::string htlc_id;                     // Unique swap identifier
    std::string channel_id;                  // Channel executing the swap
    HTLCType type;                           // Always SWAP

    // Hashlock (same for both legs)
    std::array<uint8_t, 32> payment_hash;
    uint32_t cltv_expiry;
    bool is_incoming;                        // We receive asset_in

    // Input leg (what we give)
    AssetID asset_in;
    uint64_t amount_in;

    // Output leg (what we receive)
    AssetID asset_out;
    uint64_t amount_out;

    // Rate information
    RateSource rate_source;
    std::optional<OracleRate> oracle_rate;   // If rate_source == ORACLE
    uint64_t rate_numerator;                 // Effective rate
    uint64_t rate_denominator;

    // Slippage tolerance (basis points)
    uint32_t max_slippage_bps;               // e.g., 50 = 0.5%

    // State proofs
    MerkleProof state_proof_in;              // Proof of asset_in
    MerkleProof state_proof_out;             // Proof of asset_out availability
    std::array<uint8_t, 32> prev_state_hash;

    // CTV/CCV enforcement
    std::array<uint8_t, 32> ctv_hash;        // Template commitment
    std::array<uint8_t, 32> ccv_hash;        // Contract commitment
    std::vector<uint8_t> swap_script;        // CCV swap logic

    // Routing (for multi-hop swaps)
    std::string next_hop_channel;
    std::string prev_hop_channel;
    std::vector<uint8_t> onion_packet;

    // State
    AssetHTLC::State state;
    uint64_t created_at;
    uint64_t updated_at;

    SwapHTLC()
        : type(HTLCType::SWAP), cltv_expiry(0), is_incoming(false),
          amount_in(0), amount_out(0),
          rate_source(RateSource::FIXED),
          rate_numerator(1), rate_denominator(1),
          max_slippage_bps(100),
          state(AssetHTLC::State::PENDING),
          created_at(0), updated_at(0) {
        payment_hash.fill(0);
        asset_in.fill(0);
        asset_out.fill(0);
        prev_state_hash.fill(0);
        ctv_hash.fill(0);
        ccv_hash.fill(0);
    }

    // Validation
    bool validate() const;
    bool validateRate() const;
    bool checkSlippage(uint64_t expected_out) const;

    // Serialization
    std::vector<uint8_t> serialize() const;
    static std::optional<SwapHTLC> deserialize(const std::vector<uint8_t>& data);

    // Compute hash for commitment
    std::array<uint8_t, 32> computeHash() const;

    // Get effective exchange rate
    double getEffectiveRate() const;
};

// ============================================================================
// Swap Request/Response Messages
// ============================================================================

/**
 * @brief Request to initiate a swap
 */
struct SwapRequest {
    std::string channel_id;
    std::string swap_id;                     // Proposed swap ID

    // Swap parameters
    AssetID asset_give;                      // What requester gives
    uint64_t amount_give;
    AssetID asset_want;                      // What requester wants
    uint64_t min_amount_want;                // Minimum acceptable

    // Rate preference
    RateSource preferred_rate_source;
    std::optional<OracleRate> oracle_rate;

    // Hashlock
    std::array<uint8_t, 32> payment_hash;
    uint32_t cltv_expiry;

    // Signature
    std::vector<uint8_t> signature;

    bool validate() const;
    std::vector<uint8_t> serialize() const;
    static std::optional<SwapRequest> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Response to swap request
 */
struct SwapResponse {
    std::string channel_id;
    std::string swap_id;
    bool accepted;
    std::string rejection_reason;

    // If accepted, the offered amount
    uint64_t offered_amount;                 // Amount responder will give
    uint64_t rate_numerator;
    uint64_t rate_denominator;

    // New channel state
    std::array<uint8_t, 32> new_state_hash;

    // Signature
    std::vector<uint8_t> signature;

    std::vector<uint8_t> serialize() const;
    static std::optional<SwapResponse> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Execute (settle) a swap with preimage
 */
struct SwapExecute {
    std::string channel_id;
    std::string swap_id;
    std::array<uint8_t, 32> preimage;

    // Final state
    std::array<uint8_t, 32> new_state_hash;
    std::vector<uint8_t> signature;

    std::vector<uint8_t> serialize() const;
    static std::optional<SwapExecute> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Multi-Hop Swap Route
// ============================================================================

/**
 * @brief Single hop in a multi-hop swap route
 */
struct SwapHop {
    std::string channel_id;
    std::string node_id;
    AssetID asset_in;                        // Asset entering this hop
    uint64_t amount_in;
    AssetID asset_out;                       // Asset leaving this hop
    uint64_t amount_out;
    uint64_t fee;                            // Fee taken by this hop
    uint32_t cltv_delta;
};

/**
 * @brief Complete multi-hop swap route
 *
 * Enables swaps across multiple channels/nodes.
 * Each hop may convert between different assets.
 */
struct SwapRoute {
    std::vector<SwapHop> hops;
    AssetID initial_asset;
    uint64_t initial_amount;
    AssetID final_asset;
    uint64_t final_amount;
    uint64_t total_fees;
    uint32_t total_timelock;

    // Validate route consistency
    bool validate() const;

    // Check if route performs a conversion
    bool isConversion() const;

    // Get effective exchange rate across full route
    double getEffectiveRate() const;
};

// ============================================================================
// Swap HTLC Manager Interface
// ============================================================================

/**
 * @brief Interface for managing Swap HTLCs
 */
class ISwapHTLCManager {
public:
    virtual ~ISwapHTLCManager() = default;

    // Swap lifecycle
    virtual Result<SwapHTLC> initiateSwap(const SwapRequest& request) = 0;
    virtual Result<SwapResponse> respondToSwap(const std::string& swap_id,
                                                bool accept,
                                                uint64_t offered_amount = 0) = 0;
    virtual Result<void> executeSwap(const std::string& swap_id,
                                      const std::array<uint8_t, 32>& preimage) = 0;
    virtual Result<void> cancelSwap(const std::string& swap_id,
                                     const std::string& reason) = 0;

    // Multi-hop swaps
    virtual Result<SwapRoute> findSwapRoute(
        const AssetID& asset_from,
        const AssetID& asset_to,
        uint64_t amount,
        uint32_t max_hops = 10) = 0;

    virtual Result<std::string> executeMultiHopSwap(
        const SwapRoute& route,
        const std::array<uint8_t, 32>& payment_hash) = 0;

    // Rate queries
    virtual Result<uint64_t> getQuote(
        const std::string& channel_id,
        const AssetID& asset_from,
        const AssetID& asset_to,
        uint64_t amount_in) = 0;

    virtual std::vector<OracleRate> getOracleRates() const = 0;

    // Queries
    virtual std::optional<SwapHTLC> getSwap(const std::string& swap_id) const = 0;
    virtual std::vector<SwapHTLC> getSwapsByChannel(const std::string& channel_id) const = 0;
    virtual std::vector<SwapHTLC> getPendingSwaps() const = 0;
};

// ============================================================================
// Swap Script Generation
// ============================================================================

/**
 * @brief Generate CCV script for atomic swap
 *
 * Creates a script enforcing:
 * 1. Preimage reveals both legs atomically
 * 2. Timeout returns assets to original owners
 * 3. Rate constraints are satisfied
 */
std::vector<uint8_t> GenerateSwapScript(
    const SwapHTLC& swap,
    const std::vector<uint8_t>& initiator_pubkey,
    const std::vector<uint8_t>& responder_pubkey);

/**
 * @brief Generate CTV template for swap commitment
 */
std::vector<uint8_t> GenerateSwapTemplate(
    const SwapHTLC& swap,
    const MultiAssetChannel& channel);

/**
 * @brief Generate witness for swap execution
 */
std::vector<uint8_t> GenerateSwapWitness(
    const SwapHTLC& swap,
    const std::array<uint8_t, 32>& preimage,
    const std::vector<uint8_t>& signature);

// ============================================================================
// Swap Validation Functions
// ============================================================================

/**
 * @brief Validate swap against channel state
 */
bool ValidateSwap(
    const SwapHTLC& swap,
    const MultiAssetChannel& channel,
    uint64_t current_height);

/**
 * @brief Check swap conservation (no asset creation)
 */
bool CheckSwapConservation(const SwapHTLC& swap);

/**
 * @brief Validate oracle rate signature
 */
bool ValidateOracleRate(
    const OracleRate& rate,
    const std::array<uint8_t, 32>& trusted_oracle_pubkey);

/**
 * @brief Check if swap rate is within acceptable slippage
 */
bool CheckSwapSlippage(
    const SwapHTLC& swap,
    uint64_t market_rate_num,
    uint64_t market_rate_denom);

// ============================================================================
// DEX Integration
// ============================================================================

/**
 * @brief DEX order for the Lightning order book
 */
struct DEXOrder {
    std::string order_id;
    std::string channel_id;                  // Channel providing liquidity
    std::string node_id;                     // Node ID of maker

    // Order parameters
    AssetID asset_sell;
    uint64_t amount_sell;
    AssetID asset_buy;
    uint64_t amount_buy;                     // Minimum acceptable

    // Price limit (asset_buy per asset_sell)
    uint64_t price_numerator;
    uint64_t price_denominator;

    // Order type
    enum class Type {
        LIMIT,           // Fill at price or better
        MARKET,          // Fill at any price
        FILL_OR_KILL     // Fill completely or cancel
    } type;

    // Validity
    uint64_t expires_at;                     // Unix timestamp
    bool is_active;

    // Statistics
    uint64_t filled_sell;                    // Amount already filled
    uint64_t filled_buy;

    DEXOrder()
        : amount_sell(0), amount_buy(0),
          price_numerator(1), price_denominator(1),
          type(Type::LIMIT), expires_at(0), is_active(true),
          filled_sell(0), filled_buy(0) {
        asset_sell.fill(0);
        asset_buy.fill(0);
    }

    // Remaining to fill
    uint64_t remainingSell() const { return amount_sell - filled_sell; }
    uint64_t remainingBuy() const { return amount_buy - filled_buy; }

    // Check if fully filled
    bool isFilled() const { return filled_sell >= amount_sell; }
};

/**
 * @brief DEX order book interface
 */
class IDEXOrderBook {
public:
    virtual ~IDEXOrderBook() = default;

    // Order management
    virtual Result<std::string> placeOrder(const DEXOrder& order) = 0;
    virtual Result<void> cancelOrder(const std::string& order_id) = 0;

    // Order matching
    virtual std::vector<DEXOrder> getMatchingOrders(
        const AssetID& asset_buy,
        const AssetID& asset_sell,
        uint64_t amount,
        uint64_t max_price_num = 0,
        uint64_t max_price_denom = 1) = 0;

    // Queries
    virtual std::optional<DEXOrder> getOrder(const std::string& order_id) const = 0;
    virtual std::vector<DEXOrder> getOrdersByAssetPair(
        const AssetID& asset_a,
        const AssetID& asset_b) const = 0;
    virtual std::vector<DEXOrder> getOrdersByNode(const std::string& node_id) const = 0;
};

} // namespace lightning
} // namespace dinero
