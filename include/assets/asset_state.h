/**
 * Phase 30: Taproot Asset Layer - Asset State
 *
 * Asset state is tracked via UTXOs. Each asset-bearing output contains:
 * - AssetID identifying the asset type
 * - Amount of the asset
 * - State commitment for contract validation
 *
 * State transitions are validated via TXHASH introspection.
 */

#pragma once

#include "assets/asset_id.h"
#include <vector>
#include <string>
#include <optional>
#include <map>

namespace dinero {
namespace assets {

// ============================================================================
// Asset UTXO - Single asset-bearing output
// ============================================================================

/**
 * @brief An asset-bearing UTXO
 */
struct AssetUTXO {
    // Location in blockchain
    std::string txid;                           // Transaction ID
    uint32_t vout;                              // Output index

    // Asset information
    AssetID asset_id;                           // Asset type
    uint64_t amount;                            // Amount in base units
    std::array<uint8_t, 32> state_hash;         // State commitment

    // Ownership
    std::vector<uint8_t> script_pubkey;         // Output script
    std::string owner_address;                  // Decoded address

    // Blockchain position
    uint32_t height;                            // Block height (0 = unconfirmed)
    uint64_t timestamp;                         // Block timestamp

    // Spending info
    bool is_spent;
    std::string spending_txid;
    uint32_t spending_input_index;

    // Generate outpoint string "txid:vout"
    std::string outpoint() const;

    // Serialize/deserialize
    std::vector<uint8_t> serialize() const;
    static std::optional<AssetUTXO> deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Asset Balance - Aggregated view
// ============================================================================

/**
 * @brief Aggregated balance for one asset type
 */
struct AssetBalance {
    AssetID asset_id;
    uint64_t confirmed_balance;                 // Confirmed amount
    uint64_t unconfirmed_balance;               // Unconfirmed incoming
    uint64_t pending_spend;                     // Unconfirmed outgoing
    uint32_t utxo_count;                        // Number of UTXOs

    uint64_t available() const {
        return confirmed_balance - pending_spend;
    }

    uint64_t total() const {
        return confirmed_balance + unconfirmed_balance;
    }
};

// ============================================================================
// Asset State Transition
// ============================================================================

/**
 * @brief Type of state transition
 */
enum class TransitionType : uint8_t {
    TRANSFER = 0,           // Simple transfer to new owner
    MINT = 1,               // New supply created
    BURN = 2,               // Supply destroyed
    CONTRACT_CALL = 3,      // Contract state update
    SPLIT = 4,              // Single UTXO split into multiple
    MERGE = 5               // Multiple UTXOs merged into one
};

/**
 * @brief A state transition for asset validation
 */
struct AssetStateTransition {
    TransitionType type;

    // Inputs (assets being spent)
    struct Input {
        std::string txid;
        uint32_t vout;
        AssetID asset_id;
        uint64_t amount;
        std::array<uint8_t, 32> prev_state_hash;
    };
    std::vector<Input> inputs;

    // Outputs (assets being created)
    struct Output {
        AssetID asset_id;
        uint64_t amount;
        std::array<uint8_t, 32> new_state_hash;
        std::vector<uint8_t> script_pubkey;
    };
    std::vector<Output> outputs;

    // Validation data
    std::vector<uint8_t> authorization;         // CSFS signature for mint/burn
    std::array<uint8_t, 32> transition_hash;    // Hash of this transition

    // Validate the transition
    bool validate() const;

    // Check conservation (inputs >= outputs for transfers)
    bool checkConservation() const;

    // Compute transition hash
    std::array<uint8_t, 32> computeHash() const;
};

// ============================================================================
// Asset State Machine
// ============================================================================

/**
 * @brief State machine for contract-controlled assets
 */
struct AssetStateMachine {
    AssetID asset_id;
    std::array<uint8_t, 32> code_hash;          // Hash of contract code
    std::array<uint8_t, 32> current_state;      // Current state root
    uint64_t transition_count;                  // Number of transitions

    // State data (key-value store)
    std::map<std::string, std::vector<uint8_t>> state_data;

    // Compute new state after transition
    std::array<uint8_t, 32> computeNewState(
        const std::vector<uint8_t>& input_data) const;

    // Verify a state transition is valid
    bool verifyTransition(
        const std::array<uint8_t, 32>& old_state,
        const std::array<uint8_t, 32>& new_state,
        const std::vector<uint8_t>& proof) const;
};

// ============================================================================
// Asset Output Selection
// ============================================================================

/**
 * @brief Result of UTXO selection for asset spending
 */
struct AssetCoinSelection {
    std::vector<AssetUTXO> selected_utxos;
    uint64_t total_selected;
    uint64_t target_amount;
    uint64_t change_amount;                     // Change to return

    // Fee information (native DIN)
    uint64_t estimated_fee;
    uint64_t fee_rate;

    bool sufficient() const {
        return total_selected >= target_amount;
    }
};

/**
 * @brief Select UTXOs for spending a specific asset
 *
 * @param available Available UTXOs
 * @param asset_id Asset to spend
 * @param target_amount Amount needed
 * @return Selection result
 */
AssetCoinSelection SelectAssetCoins(
    const std::vector<AssetUTXO>& available,
    const AssetID& asset_id,
    uint64_t target_amount);

// ============================================================================
// Multi-Asset Transaction
// ============================================================================

/**
 * @brief A transaction involving multiple asset types
 */
struct MultiAssetTransaction {
    // Asset movements
    struct AssetMovement {
        AssetID asset_id;
        uint64_t amount;
        std::string from_address;               // Empty for mint
        std::string to_address;                 // Empty for burn
    };
    std::vector<AssetMovement> movements;

    // Native coin for fees
    uint64_t fee_amount;

    // Build the raw transaction
    std::vector<uint8_t> buildRawTx() const;

    // Estimate transaction size
    uint64_t estimateVSize() const;
};

} // namespace assets
} // namespace dinero
