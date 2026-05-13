/**
 * Phase 30: Taproot Asset Layer - Asset Identification
 *
 * AssetID is a unique 32-byte identifier for each asset type.
 * Derived from: SHA256d(issuer_pubkey || creation_txid || metadata_hash)
 *
 * This ensures:
 * - Global uniqueness (tied to specific creation transaction)
 * - Issuer binding (cryptographically linked to issuer)
 * - Metadata commitment (hash of off-chain metadata)
 */

#pragma once

#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace dinero {
namespace assets {

// ============================================================================
// Asset ID - 32-byte unique identifier
// ============================================================================

using AssetID = std::array<uint8_t, 32>;

/**
 * @brief Compute AssetID from creation parameters
 *
 * AssetID = SHA256d(issuer_pubkey || creation_txid || metadata_hash)
 *
 * @param issuer_pubkey 32-byte x-only public key of issuer
 * @param creation_txid 32-byte transaction ID where asset was created
 * @param metadata_hash 32-byte hash of asset metadata
 * @return Computed AssetID
 */
AssetID ComputeAssetID(
    const std::array<uint8_t, 32>& issuer_pubkey,
    const std::array<uint8_t, 32>& creation_txid,
    const std::array<uint8_t, 32>& metadata_hash);

/**
 * @brief Check if AssetID represents the native coin (DIN)
 */
bool IsNativeAsset(const AssetID& id);

/**
 * @brief Get the null/zero AssetID (represents native DIN)
 */
AssetID NullAssetID();

// ============================================================================
// Asset Metadata
// ============================================================================

/**
 * @brief Asset metadata stored off-chain but committed on-chain
 */
struct AssetMetadata {
    std::string name;                           // Human-readable name (max 64 chars)
    std::string ticker;                         // Short symbol (max 8 chars)
    uint8_t decimals;                           // Decimal places (0-18)
    std::string description;                    // Optional description (max 256 chars)
    std::string icon_url;                       // Optional icon URL
    std::vector<uint8_t> extended_data;         // Application-specific data

    // Compute hash of metadata for inclusion in AssetID
    std::array<uint8_t, 32> hash() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetMetadata> deserialize(const std::vector<uint8_t>& data);

    // JSON conversion
    std::string toJSON() const;
    static std::optional<AssetMetadata> fromJSON(const std::string& json);
};

// ============================================================================
// Asset Supply Rules
// ============================================================================

/**
 * @brief Supply model for an asset
 */
enum class SupplyModel : uint8_t {
    FIXED = 0,              // Fixed supply, no minting after creation
    CAPPED = 1,             // Maximum supply cap, can mint up to cap
    UNLIMITED = 2,          // No supply cap (inflationary)
    ALGORITHMIC = 3         // Supply controlled by smart contract rules
};

/**
 * @brief Asset supply configuration
 */
struct AssetSupplyConfig {
    SupplyModel model;                          // Supply model
    uint64_t initial_supply;                    // Initial minted supply
    uint64_t max_supply;                        // Maximum supply (0 = unlimited)
    bool burn_enabled;                          // Can tokens be burned?

    // Authority pubkeys (32-byte x-only)
    std::vector<uint8_t> mint_authority;        // Who can mint (empty = no one)
    std::vector<uint8_t> burn_authority;        // Who can burn (empty = anyone)

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetSupplyConfig> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Asset Genesis (Creation Event)
// ============================================================================

/**
 * @brief Asset genesis block - defines the asset at creation
 */
struct AssetGenesis {
    AssetID asset_id;                           // Computed asset ID
    std::array<uint8_t, 32> issuer_pubkey;      // Issuer's pubkey
    std::string creation_txid;                  // Transaction that created this asset
    uint32_t creation_output_index;             // Output index in creation tx
    uint32_t creation_height;                   // Block height of creation

    AssetMetadata metadata;                     // Asset metadata
    AssetSupplyConfig supply;                   // Supply configuration

    uint64_t created_at;                        // Unix timestamp

    // Compute the asset ID from this genesis
    AssetID computeID() const;

    // Generate the CTV template script for this asset
    std::vector<uint8_t> generateGenesisScript() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetGenesis> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Asset Script Encoding
// ============================================================================

/**
 * @brief Asset commitment in Taproot leaf
 *
 * Structure:
 *   OP_PUSH(asset_id)         - 32 bytes
 *   OP_PUSH(amount)           - 8 bytes
 *   OP_PUSH(state_hash)       - 32 bytes
 *   OP_CHECKTEMPLATEVERIFY
 */
struct AssetCommitment {
    AssetID asset_id;                           // Asset type
    uint64_t amount;                            // Amount in base units
    std::array<uint8_t, 32> state_hash;         // Current state commitment

    // Generate the covenant script
    std::vector<uint8_t> toScript() const;

    // Parse from script
    static std::optional<AssetCommitment> fromScript(const std::vector<uint8_t>& script);

    // Compute commitment hash
    std::array<uint8_t, 32> hash() const;
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Convert AssetID to hex string
 */
std::string AssetIDToHex(const AssetID& id);

/**
 * @brief Parse AssetID from hex string
 */
std::optional<AssetID> AssetIDFromHex(const std::string& hex);

/**
 * @brief Compare two AssetIDs
 */
bool AssetIDEqual(const AssetID& a, const AssetID& b);

/**
 * @brief Get short display string for asset (first 8 chars of hex)
 */
std::string AssetIDShort(const AssetID& id);

} // namespace assets
} // namespace dinero
