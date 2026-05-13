/**
 * Phase 30: Taproot Asset Layer - Transfer Protocol
 *
 * Asset transfers are UTXO-based with consensus validation:
 * - Input AssetID must match Output AssetID
 * - Conservation: sum(inputs) >= sum(outputs)
 * - State transitions validated via TXHASH introspection
 * - CTV templates enforce deterministic outputs
 */

#pragma once

#include "assets/asset_id.h"
#include "assets/asset_state.h"
#include "assets/asset_proof.h"
#include <vector>
#include <string>
#include <optional>
#include <functional>

namespace dinero {
namespace assets {

// ============================================================================
// Transfer Validation Error Codes
// ============================================================================

enum class TransferError : int {
    SUCCESS = 0,

    // Input errors
    INPUT_NOT_FOUND = 100,
    INPUT_ALREADY_SPENT = 101,
    INPUT_ASSET_MISMATCH = 102,
    INPUT_AMOUNT_MISMATCH = 103,
    INPUT_STATE_MISMATCH = 104,

    // Output errors
    OUTPUT_ASSET_INVALID = 200,
    OUTPUT_AMOUNT_ZERO = 201,
    OUTPUT_SCRIPT_INVALID = 202,

    // Conservation errors
    CONSERVATION_VIOLATION = 300,
    ASSET_TYPE_MISMATCH = 301,
    INFLATION_DETECTED = 302,

    // Authorization errors
    MINT_UNAUTHORIZED = 400,
    BURN_UNAUTHORIZED = 401,
    CONTRACT_UNAUTHORIZED = 402,

    // Proof errors
    PROOF_INVALID = 500,
    PROOF_MISSING = 501,
    TAPROOT_INVALID = 502,

    // Template errors
    CTV_MISMATCH = 600,
    TXHASH_MISMATCH = 601
};

/**
 * @brief Convert error code to string
 */
std::string TransferErrorToString(TransferError err);

// ============================================================================
// Transfer Request
// ============================================================================

/**
 * @brief Request to transfer assets
 */
struct AssetTransferRequest {
    // Source UTXOs to spend
    struct Source {
        std::string txid;
        uint32_t vout;
        AssetID asset_id;
        uint64_t amount;
    };
    std::vector<Source> sources;

    // Destination outputs
    struct Destination {
        AssetID asset_id;
        uint64_t amount;
        std::string address;                    // Recipient address
        std::vector<uint8_t> script_pubkey;     // Or raw scriptPubKey
    };
    std::vector<Destination> destinations;

    // Fee configuration
    uint64_t fee_rate;                          // sat/vB
    std::string change_address;                 // For asset change

    // Validate the request (pre-build check)
    TransferError validate() const;

    // Check that sources cover destinations
    bool checkCoverage() const;
};

// ============================================================================
// Transfer Builder
// ============================================================================

/**
 * @brief Builds an asset transfer transaction
 */
class AssetTransferBuilder {
public:
    AssetTransferBuilder();

    // Add inputs
    AssetTransferBuilder& addInput(
        const std::string& txid,
        uint32_t vout,
        const AssetID& asset_id,
        uint64_t amount,
        const std::vector<uint8_t>& script_pubkey);

    // Add outputs
    AssetTransferBuilder& addOutput(
        const AssetID& asset_id,
        uint64_t amount,
        const std::string& address);

    AssetTransferBuilder& addOutputScript(
        const AssetID& asset_id,
        uint64_t amount,
        const std::vector<uint8_t>& script_pubkey);

    // Set fee
    AssetTransferBuilder& setFeeRate(uint64_t sat_per_vb);
    AssetTransferBuilder& setChangeAddress(const std::string& address);

    // Build the transaction
    struct BuildResult {
        bool success;
        TransferError error;
        std::vector<uint8_t> raw_tx;            // Unsigned transaction
        std::array<uint8_t, 32> ctv_hash;       // Template hash
        uint64_t total_fee;
        uint64_t change_amount;
    };
    BuildResult build();

    // Reset builder
    void reset();

private:
    struct Input {
        std::string txid;
        uint32_t vout;
        AssetID asset_id;
        uint64_t amount;
        std::vector<uint8_t> script_pubkey;
    };
    std::vector<Input> inputs_;

    struct Output {
        AssetID asset_id;
        uint64_t amount;
        std::vector<uint8_t> script_pubkey;
    };
    std::vector<Output> outputs_;

    uint64_t fee_rate_ = 1;
    std::string change_address_;
};

// ============================================================================
// Transfer Validator
// ============================================================================

/**
 * @brief Validates asset transfers at consensus level
 */
class AssetTransferValidator {
public:
    /**
     * @brief Validate a complete transfer
     *
     * @param transition State transition to validate
     * @param proof Proof of validity
     * @param utxo_lookup Function to lookup UTXOs
     * @return Validation result
     */
    static TransferError validate(
        const AssetStateTransition& transition,
        const AssetTransferProof& proof,
        std::function<std::optional<AssetUTXO>(const std::string&, uint32_t)> utxo_lookup);

    /**
     * @brief Validate asset conservation (no inflation)
     *
     * @param inputs Input assets
     * @param outputs Output assets
     * @return SUCCESS if conserved, error otherwise
     */
    static TransferError validateConservation(
        const std::vector<std::pair<AssetID, uint64_t>>& inputs,
        const std::vector<std::pair<AssetID, uint64_t>>& outputs);

    /**
     * @brief Validate CTV template match
     *
     * @param expected_hash Expected template hash
     * @param actual_tx Actual transaction bytes
     * @param input_index Which input is being validated
     * @return SUCCESS if matches, error otherwise
     */
    static TransferError validateCTVTemplate(
        const std::array<uint8_t, 32>& expected_hash,
        const std::vector<uint8_t>& actual_tx,
        uint32_t input_index);

    /**
     * @brief Validate TXHASH introspection
     *
     * @param expected_hash Expected TXHASH
     * @param tx Transaction being validated
     * @param flags TXHASH flags
     * @return SUCCESS if matches, error otherwise
     */
    static TransferError validateTXHASH(
        const std::array<uint8_t, 32>& expected_hash,
        const std::vector<uint8_t>& tx,
        uint32_t flags);
};

// ============================================================================
// Asset Transaction Signing
// ============================================================================

/**
 * @brief Signs an asset transfer transaction
 */
class AssetTransferSigner {
public:
    /**
     * @brief Sign a transfer with private key
     *
     * @param raw_tx Unsigned transaction
     * @param input_index Which input to sign
     * @param private_key 32-byte private key
     * @param asset_script Asset commitment script
     * @return Signed witness data
     */
    static std::vector<uint8_t> sign(
        const std::vector<uint8_t>& raw_tx,
        uint32_t input_index,
        const std::array<uint8_t, 32>& private_key,
        const std::vector<uint8_t>& asset_script);

    /**
     * @brief Create authorization signature for mint/burn (CSFS)
     *
     * @param asset_id Asset being minted/burned
     * @param amount Amount
     * @param authority_key Authority private key
     * @return CSFS signature
     */
    static std::vector<uint8_t> signAuthorization(
        const AssetID& asset_id,
        uint64_t amount,
        const std::array<uint8_t, 32>& authority_key);
};

// ============================================================================
// Transfer Script Generation
// ============================================================================

/**
 * @brief Generate asset commitment script
 *
 * Script structure:
 *   OP_PUSH(asset_id)
 *   OP_PUSH(amount)
 *   OP_PUSH(state_hash)
 *   OP_CHECKTEMPLATEVERIFY
 *
 * @param asset_id Asset type
 * @param amount Amount
 * @param state_hash State commitment
 * @return Script bytes
 */
std::vector<uint8_t> GenerateAssetScript(
    const AssetID& asset_id,
    uint64_t amount,
    const std::array<uint8_t, 32>& state_hash);

/**
 * @brief Generate Taproot output for asset
 *
 * @param internal_key Internal pubkey
 * @param asset_script Asset commitment script
 * @return P2TR scriptPubKey
 */
std::vector<uint8_t> GenerateAssetTaprootOutput(
    const std::array<uint8_t, 32>& internal_key,
    const std::vector<uint8_t>& asset_script);

/**
 * @brief Parse asset information from script
 *
 * @param script Script to parse
 * @return Parsed asset commitment, or nullopt if not an asset script
 */
std::optional<AssetCommitment> ParseAssetScript(
    const std::vector<uint8_t>& script);

} // namespace assets
} // namespace dinero
