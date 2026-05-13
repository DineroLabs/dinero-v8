/**
 * Phase 30: Taproot Asset Layer - Proof System
 *
 * Assets use Taproot-style Merkle proofs for:
 * - Inclusion proofs (asset exists in output)
 * - State proofs (asset state is valid)
 * - Transition proofs (state change is authorized)
 *
 * Proofs are compact and can be verified without full state.
 */

#pragma once

#include "assets/asset_id.h"
#include <vector>
#include <array>
#include <optional>

namespace dinero {
namespace assets {

// ============================================================================
// Merkle Proof Types
// ============================================================================

/**
 * @brief A node in the Merkle tree
 */
struct MerkleNode {
    std::array<uint8_t, 32> hash;
    bool is_left;                               // Position in parent

    std::vector<uint8_t> serialize() const;
    static std::optional<MerkleNode> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Merkle inclusion proof
 */
struct MerkleProof {
    std::array<uint8_t, 32> leaf_hash;          // Hash of the leaf
    std::vector<MerkleNode> path;               // Path to root
    std::array<uint8_t, 32> root;               // Expected root hash

    // Verify the proof
    bool verify() const;

    // Compute root from leaf and path
    std::array<uint8_t, 32> computeRoot() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<MerkleProof> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Asset Inclusion Proof
// ============================================================================

/**
 * @brief Proof that an asset commitment is in a Taproot output
 */
struct AssetInclusionProof {
    // Asset being proven
    AssetID asset_id;
    uint64_t amount;
    std::array<uint8_t, 32> state_hash;

    // Taproot proof
    std::array<uint8_t, 32> internal_key;       // Taproot internal pubkey
    std::vector<uint8_t> control_block;         // Taproot control block
    MerkleProof script_path_proof;              // Proof to script in tree

    // Output reference
    std::string txid;
    uint32_t vout;

    // Verify the proof against output scriptPubKey
    bool verify(const std::vector<uint8_t>& script_pubkey) const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetInclusionProof> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// State Transition Proof
// ============================================================================

/**
 * @brief Proof that a state transition is valid
 */
struct StateTransitionProof {
    // Previous state
    std::array<uint8_t, 32> prev_state_root;
    MerkleProof prev_state_proof;

    // New state
    std::array<uint8_t, 32> new_state_root;

    // Transition data
    std::vector<uint8_t> transition_data;       // Input to state machine
    std::array<uint8_t, 32> transition_hash;    // Hash of transition

    // Authorization
    std::vector<uint8_t> signature;             // CSFS signature if required
    std::vector<uint8_t> pubkey;                // Authorizing pubkey

    // CTV template used
    std::array<uint8_t, 32> ctv_hash;           // Template hash

    // Verify the transition
    bool verify() const;

    // Verify authorization signature (CSFS)
    bool verifyAuthorization() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<StateTransitionProof> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Asset Transfer Proof
// ============================================================================

/**
 * @brief Complete proof for an asset transfer
 */
struct AssetTransferProof {
    // Source UTXOs
    struct SourceProof {
        AssetInclusionProof inclusion;          // Proves asset in source
        std::vector<uint8_t> witness;           // Spending witness
    };
    std::vector<SourceProof> sources;

    // Destination outputs
    struct DestProof {
        AssetID asset_id;
        uint64_t amount;
        std::array<uint8_t, 32> commitment;     // Taproot commitment
    };
    std::vector<DestProof> destinations;

    // Conservation proof (sum of inputs = sum of outputs)
    bool verifyConservation() const;

    // Full verification
    bool verify() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetTransferProof> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Mint/Burn Proof
// ============================================================================

/**
 * @brief Proof for minting new asset supply
 */
struct MintProof {
    AssetID asset_id;
    uint64_t mint_amount;

    // Genesis reference
    std::string genesis_txid;
    uint32_t genesis_vout;

    // Authorization
    std::vector<uint8_t> mint_authority_pubkey;
    std::vector<uint8_t> authorization_sig;     // CSFS signature

    // Verify mint is authorized
    bool verifyAuthorization() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<MintProof> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @brief Proof for burning asset supply
 */
struct BurnProof {
    AssetID asset_id;
    uint64_t burn_amount;

    // Source being burned
    AssetInclusionProof source;

    // Authorization (optional for permissionless burn)
    std::vector<uint8_t> burn_authority_pubkey;
    std::vector<uint8_t> authorization_sig;

    // Verify burn is valid
    bool verify() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<BurnProof> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Proof Aggregation
// ============================================================================

/**
 * @brief Aggregate multiple proofs into one
 */
struct AggregateProof {
    std::vector<AssetTransferProof> transfers;
    std::vector<MintProof> mints;
    std::vector<BurnProof> burns;

    // Batch verification
    bool verifyAll() const;

    // Compute aggregate commitment
    std::array<uint8_t, 32> computeCommitment() const;
};

// ============================================================================
// Proof Building Utilities
// ============================================================================

/**
 * @brief Build a Merkle tree from leaves
 *
 * @param leaves Leaf hashes
 * @return Root hash
 */
std::array<uint8_t, 32> BuildMerkleTree(
    const std::vector<std::array<uint8_t, 32>>& leaves);

/**
 * @brief Generate a proof for a specific leaf
 *
 * @param leaves All leaves in tree
 * @param leaf_index Index of leaf to prove
 * @return Merkle proof
 */
MerkleProof GenerateMerkleProof(
    const std::vector<std::array<uint8_t, 32>>& leaves,
    size_t leaf_index);

/**
 * @brief Compute tagged hash (BIP-340 style)
 *
 * @param tag Tag string
 * @param data Data to hash
 * @return Tagged hash
 */
std::array<uint8_t, 32> TaggedHash(
    const std::string& tag,
    const std::vector<uint8_t>& data);

/**
 * @brief Compute Taproot leaf hash
 *
 * @param version Leaf version
 * @param script Script bytes
 * @return Leaf hash
 */
std::array<uint8_t, 32> TaprootLeafHash(
    uint8_t version,
    const std::vector<uint8_t>& script);

/**
 * @brief Compute Taproot branch hash
 *
 * @param left Left child hash
 * @param right Right child hash
 * @return Branch hash
 */
std::array<uint8_t, 32> TaprootBranchHash(
    const std::array<uint8_t, 32>& left,
    const std::array<uint8_t, 32>& right);

} // namespace assets
} // namespace dinero
