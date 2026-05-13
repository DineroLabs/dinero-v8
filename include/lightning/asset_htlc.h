#pragma once

/**
 * Phase 31: Multi-Asset Lightning Settlement Layer
 *
 * Asset HTLC (Hashed Time-Locked Contract) for multi-asset payments
 *
 * Extends standard Lightning HTLCs to support:
 * - Asset-specific payments
 * - Taproot state proofs
 * - CTV-enforced templates
 * - Multi-hop asset routing
 */

#include "lightning/lightning_types.h"
#include "lightning/asset_channel.h"
#include "assets/asset_id.h"
#include "assets/asset_proof.h"
#include <array>
#include <vector>
#include <optional>

namespace dinero {
namespace lightning {

using assets::AssetID;
using assets::MerkleProof;

// ============================================================================
// Asset HTLC Types
// ============================================================================

/**
 * @brief HTLC type enumeration
 */
enum class HTLCType : uint8_t {
    STANDARD = 0,      // Standard DIN HTLC (backward compatible)
    ASSET = 1,         // Single-asset HTLC
    SWAP = 2,          // Cross-asset swap HTLC
    MULTI_ASSET = 3    // Multi-asset bundle HTLC
};

inline std::string htlcTypeToString(HTLCType type) {
    switch (type) {
        case HTLCType::STANDARD: return "STANDARD";
        case HTLCType::ASSET: return "ASSET";
        case HTLCType::SWAP: return "SWAP";
        case HTLCType::MULTI_ASSET: return "MULTI_ASSET";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Asset-specific HTLC for single-asset payments
 *
 * Extends the standard HTLC with:
 * - Asset identification
 * - Taproot state proof
 * - CTV commitment
 */
struct AssetHTLC {
    // Standard HTLC fields
    std::string htlc_id;                     // Unique identifier (32-byte hex)
    std::string channel_id;                  // Channel this HTLC belongs to
    std::array<uint8_t, 32> payment_hash;    // SHA256(preimage)
    uint32_t cltv_expiry;                    // Absolute block height timeout
    bool is_incoming;                        // Direction

    // Asset-specific fields
    AssetID asset_id;                        // Which asset is locked
    uint64_t amount;                         // Amount in asset base units

    // Taproot state proof
    MerkleProof state_proof;                 // Proof of asset in channel state
    std::array<uint8_t, 32> prev_state_hash; // Previous channel state

    // CTV enforcement
    std::array<uint8_t, 32> ctv_hash;        // CheckTemplateVerify commitment
    std::vector<uint8_t> ctv_template;       // Serialized transaction template

    // Routing information
    std::string next_hop_channel;            // Next channel in route
    std::string prev_hop_channel;            // Previous channel in route

    // Onion routing data
    std::vector<uint8_t> onion_packet;       // Encrypted routing blob
    std::array<uint8_t, 32> shared_secret;   // ECDH shared secret for this hop

    // State
    enum class State {
        PENDING,       // Offered, awaiting settlement
        SETTLED,       // Preimage revealed
        FAILED,        // Payment failed
        TIMED_OUT      // CLTV expiry reached
    } state;

    // Timestamps
    uint64_t created_at;
    uint64_t updated_at;

    AssetHTLC()
        : cltv_expiry(0), is_incoming(false), amount(0),
          state(State::PENDING), created_at(0), updated_at(0) {
        payment_hash.fill(0);
        asset_id.fill(0);
        prev_state_hash.fill(0);
        ctv_hash.fill(0);
        shared_secret.fill(0);
    }

    // Validation
    bool validate() const;
    bool validateStateProof() const;
    bool validateCTV() const;

    // Serialization
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetHTLC> deserialize(const std::vector<uint8_t>& data);

    // Compute HTLC hash (for commitment)
    std::array<uint8_t, 32> computeHash() const;
};

inline std::string assetHTLCStateToString(AssetHTLC::State state) {
    switch (state) {
        case AssetHTLC::State::PENDING: return "PENDING";
        case AssetHTLC::State::SETTLED: return "SETTLED";
        case AssetHTLC::State::FAILED: return "FAILED";
        case AssetHTLC::State::TIMED_OUT: return "TIMED_OUT";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// Multi-Asset Bundle HTLC
// ============================================================================

/**
 * @brief HTLC that locks multiple assets atomically
 *
 * Used for complex payments involving multiple asset types.
 * All assets are released together or none at all.
 */
struct MultiAssetHTLC {
    std::string htlc_id;
    std::string channel_id;
    std::array<uint8_t, 32> payment_hash;
    uint32_t cltv_expiry;
    bool is_incoming;

    // Multiple assets locked
    struct LockedAsset {
        AssetID asset_id;
        uint64_t amount;
        MerkleProof state_proof;
    };
    std::vector<LockedAsset> locked_assets;

    // Master state proof (covers all assets)
    std::array<uint8_t, 32> master_state_hash;
    std::array<uint8_t, 32> ctv_hash;

    // Routing
    std::string next_hop_channel;
    std::string prev_hop_channel;
    std::vector<uint8_t> onion_packet;

    // State
    AssetHTLC::State state;
    uint64_t created_at;
    uint64_t updated_at;

    MultiAssetHTLC()
        : cltv_expiry(0), is_incoming(false),
          state(AssetHTLC::State::PENDING), created_at(0), updated_at(0) {
        payment_hash.fill(0);
        master_state_hash.fill(0);
        ctv_hash.fill(0);
    }

    // Total value in each asset
    uint64_t totalAmount(const AssetID& asset_id) const;

    // Validation
    bool validate() const;

    // Serialization
    std::vector<uint8_t> serialize() const;
    static std::optional<MultiAssetHTLC> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Asset HTLC Offer/Accept Messages
// ============================================================================

/**
 * @brief Message to offer an Asset HTLC to peer
 */
struct AssetHTLCOffer {
    std::string channel_id;
    std::string htlc_id;
    AssetID asset_id;
    uint64_t amount;
    std::array<uint8_t, 32> payment_hash;
    uint32_t cltv_expiry;

    // Proofs
    MerkleProof state_proof;
    std::array<uint8_t, 32> ctv_hash;

    // Onion
    std::vector<uint8_t> onion_packet;

    // Signature over offer
    std::vector<uint8_t> signature;

    std::vector<uint8_t> serialize() const;
    static std::optional<AssetHTLCOffer> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Message to accept an Asset HTLC
 */
struct AssetHTLCAccept {
    std::string channel_id;
    std::string htlc_id;
    bool accepted;
    std::string rejection_reason;          // If not accepted

    // New channel state after HTLC
    std::array<uint8_t, 32> new_state_hash;

    // Signature
    std::vector<uint8_t> signature;

    std::vector<uint8_t> serialize() const;
    static std::optional<AssetHTLCAccept> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Message to settle an Asset HTLC with preimage
 */
struct AssetHTLCSettle {
    std::string channel_id;
    std::string htlc_id;
    std::array<uint8_t, 32> preimage;      // Payment preimage

    // New state after settlement
    std::array<uint8_t, 32> new_state_hash;
    std::vector<uint8_t> signature;

    std::vector<uint8_t> serialize() const;
    static std::optional<AssetHTLCSettle> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Message to fail an Asset HTLC
 */
struct AssetHTLCFail {
    std::string channel_id;
    std::string htlc_id;
    uint16_t failure_code;                 // BOLT #4 failure code
    std::vector<uint8_t> failure_data;     // Additional failure data

    // Encrypted failure for onion return
    std::vector<uint8_t> encrypted_failure;

    std::vector<uint8_t> serialize() const;
    static std::optional<AssetHTLCFail> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Asset HTLC Manager Interface
// ============================================================================

/**
 * @brief Interface for managing Asset HTLCs
 */
class IAssetHTLCManager {
public:
    virtual ~IAssetHTLCManager() = default;

    // HTLC lifecycle
    virtual Result<AssetHTLC> createAssetHTLC(
        const std::string& channel_id,
        const AssetID& asset_id,
        uint64_t amount,
        const std::array<uint8_t, 32>& payment_hash,
        uint32_t cltv_expiry,
        const std::string& next_hop = "") = 0;

    virtual Result<void> acceptAssetHTLC(
        const std::string& channel_id,
        const AssetHTLCOffer& offer) = 0;

    virtual Result<void> settleAssetHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        const std::array<uint8_t, 32>& preimage) = 0;

    virtual Result<void> failAssetHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        uint16_t failure_code,
        const std::vector<uint8_t>& failure_data = {}) = 0;

    virtual Result<uint64_t> timeoutExpiredAssetHTLCs(uint64_t block_height) = 0;

    // Multi-Asset HTLCs
    virtual Result<MultiAssetHTLC> createMultiAssetHTLC(
        const std::string& channel_id,
        const std::vector<MultiAssetHTLC::LockedAsset>& assets,
        const std::array<uint8_t, 32>& payment_hash,
        uint32_t cltv_expiry) = 0;

    // Queries
    virtual std::optional<AssetHTLC> getAssetHTLC(const std::string& htlc_id) const = 0;
    virtual std::vector<AssetHTLC> getAssetHTLCsByChannel(const std::string& channel_id) const = 0;
    virtual std::vector<AssetHTLC> getAssetHTLCsByAsset(const AssetID& asset_id) const = 0;
    virtual std::vector<AssetHTLC> getPendingAssetHTLCs() const = 0;
};

// ============================================================================
// Asset HTLC Validation Functions
// ============================================================================

/**
 * @brief Validate asset HTLC against channel state
 */
bool ValidateAssetHTLC(
    const AssetHTLC& htlc,
    const MultiAssetChannel& channel,
    uint64_t current_height);

/**
 * @brief Validate HTLC state proof
 */
bool ValidateHTLCStateProof(
    const AssetHTLC& htlc,
    const std::array<uint8_t, 32>& channel_state_root);

/**
 * @brief Validate CTV template matches HTLC
 */
bool ValidateHTLCTemplate(
    const AssetHTLC& htlc,
    const std::vector<uint8_t>& tx_template);

/**
 * @brief Check if HTLC can be accepted (sufficient balance, etc.)
 */
bool CanAcceptAssetHTLC(
    const AssetHTLCOffer& offer,
    const MultiAssetChannel& channel,
    uint64_t current_height);

// ============================================================================
// Asset HTLC Script Generation
// ============================================================================

/**
 * @brief Generate Taproot script for Asset HTLC
 *
 * Creates a script that can be spent by:
 * 1. Preimage reveal + signature
 * 2. Timeout + signature (after CLTV)
 */
std::vector<uint8_t> GenerateAssetHTLCScript(
    const AssetHTLC& htlc,
    const std::vector<uint8_t>& local_pubkey,
    const std::vector<uint8_t>& remote_pubkey,
    const std::vector<uint8_t>& revocation_pubkey);

/**
 * @brief Generate CTV template for Asset HTLC commitment
 */
std::vector<uint8_t> GenerateAssetHTLCTemplate(
    const AssetHTLC& htlc,
    const MultiAssetChannel& channel);

/**
 * @brief Compute CTV hash for Asset HTLC
 */
std::array<uint8_t, 32> ComputeAssetHTLCCTVHash(
    const AssetHTLC& htlc,
    const MultiAssetChannel& channel);

} // namespace lightning
} // namespace dinero
