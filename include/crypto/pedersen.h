#pragma once

#include <stdint.h>
#include <stddef.h>
#include <secp256k1.h>
#include <secp256k1_generator.h>
// Note: secp256k1_pedersen_commitment is defined in secp256k1_generator.h

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Pedersen Commitments - secp256k1 Integration
// ============================================================================
//
// This header provides Pedersen commitment functionality using secp256k1,
// which is already integrated in Dinero. We use secp256k1's native
// Pedersen commitment support for maximum compatibility.
//
// A Pedersen commitment is: C = blind·G + amount·H
// where G and H are generator points, blind is a random scalar, and amount
// is the value being committed to.
//
// References:
// - https://github.com/ElementsProject/secp256k1-zkp
// - https://en.wikipedia.org/wiki/Commitment_scheme#Pedersen_commitment

// ============================================================================
// Constants
// ============================================================================

/// Commitment size (33 bytes for compressed secp256k1 point)
#define PEDERSEN_COMMITMENT_SIZE 33

/// Blinding factor size (32 bytes scalar)
#define PEDERSEN_BLINDING_SIZE 32

/// Generator size (33 bytes for compressed point)
#define PEDERSEN_GENERATOR_SIZE 33

// ============================================================================
// Context Management
// ============================================================================

/**
 * Get global secp256k1 context for Pedersen operations
 *
 * @return secp256k1 context pointer (never NULL)
 *
 * @note This returns a singleton context that should not be freed
 */
secp256k1_context* pedersen_get_context(void);

// ============================================================================
// Commitment Creation
// ============================================================================

/**
 * Create a Pedersen commitment: C = blind·G + amount·H
 *
 * @param commitment_out Output buffer for 33-byte commitment
 * @param blinding       32-byte blinding factor (random scalar)
 * @param amount         Amount to commit to (0 to 2^64-1)
 * @return 1 on success, 0 on failure
 *
 * @note The blinding factor should be cryptographically random
 * @note The same blinding factor is needed to open the commitment
 */
int pedersen_commit(
    uint8_t* commitment_out,
    const uint8_t* blinding,
    uint64_t amount
);

/**
 * Create a blinded commitment with explicit generator
 *
 * @param commitment_out Output buffer for 33-byte commitment
 * @param blinding       32-byte blinding factor
 * @param amount         Amount to commit to
 * @param generator      33-byte generator point (or NULL for default H)
 * @return 1 on success, 0 on failure
 */
int pedersen_commit_with_generator(
    uint8_t* commitment_out,
    const uint8_t* blinding,
    uint64_t amount,
    const uint8_t* generator
);

// ============================================================================
// Commitment Verification
// ============================================================================

/**
 * Verify commitment sum: sum(positive) - sum(negative) = 0
 *
 * This is used to verify transaction balance:
 * sum(input_commitments) - sum(output_commitments) - fee_commitment = 0
 *
 * @param positive    Array of positive commitments (inputs)
 * @param n_positive  Number of positive commitments
 * @param negative    Array of negative commitments (outputs + fee)
 * @param n_negative  Number of negative commitments
 * @return 1 if sum is zero, 0 otherwise
 *
 * @note Each commitment is 33 bytes
 * @note Arrays are NOT pointers to pointers, but contiguous 33-byte chunks
 */
int pedersen_verify_commitment_sum(
    const uint8_t* positive,
    size_t n_positive,
    const uint8_t* negative,
    size_t n_negative
);

/**
 * Parse a commitment from bytes
 *
 * @param commitment_out Parsed commitment structure
 * @param input          33-byte serialized commitment
 * @return 1 on success, 0 on failure
 */
int pedersen_parse_commitment(
    secp256k1_pedersen_commitment* commitment_out,
    const uint8_t* input
);

/**
 * Serialize a commitment to bytes
 *
 * @param output     Output buffer for 33-byte commitment
 * @param commitment Commitment structure to serialize
 * @return 1 on success, 0 on failure
 */
int pedersen_serialize_commitment(
    uint8_t* output,
    const secp256k1_pedersen_commitment* commitment
);

// ============================================================================
// Blinding Factor Operations
// ============================================================================

/**
 * Generate a random blinding factor
 *
 * @param blinding_out Output buffer for 32-byte blinding factor
 * @return 1 on success, 0 on failure
 *
 * @note Uses cryptographically secure random number generation
 */
int pedersen_generate_blinding(uint8_t* blinding_out);

/**
 * Add two blinding factors: result = a + b (mod n)
 *
 * @param result_out Output buffer for 32-byte result
 * @param a          First blinding factor
 * @param b          Second blinding factor
 * @return 1 on success, 0 on failure
 */
int pedersen_blind_add(
    uint8_t* result_out,
    const uint8_t* a,
    const uint8_t* b
);

/**
 * Subtract blinding factors: result = a - b (mod n)
 *
 * @param result_out Output buffer for 32-byte result
 * @param a          First blinding factor
 * @param b          Second blinding factor
 * @return 1 on success, 0 on failure
 */
int pedersen_blind_subtract(
    uint8_t* result_out,
    const uint8_t* a,
    const uint8_t* b
);

/**
 * Calculate blinding factor sum for transaction balance
 *
 * For a balanced transaction:
 * sum(input_blinds) - sum(output_blinds) = 0
 *
 * @param result_out Output buffer for 32-byte result
 * @param positive   Array of positive blinds (inputs)
 * @param n_positive Number of positive blinds
 * @param negative   Array of negative blinds (outputs)
 * @param n_negative Number of negative blinds
 * @return 1 on success, 0 on failure
 */
int pedersen_blind_sum(
    uint8_t* result_out,
    const uint8_t** positive,
    size_t n_positive,
    const uint8_t** negative,
    size_t n_negative
);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Get library version string
 *
 * @return Pointer to static version string (do not free)
 */
const char* pedersen_version(void);

#ifdef __cplusplus
}
#endif

// ============================================================================
// C++ Wrapper (RAII)
// ============================================================================

#ifdef __cplusplus

#include <vector>
#include <stdexcept>
#include <cstring>

namespace dinero {
namespace crypto {

/**
 * C++ wrapper for Pedersen commitments
 */
class PedersenCommitment {
public:
    /**
     * Create a Pedersen commitment
     *
     * @param blinding 32-byte blinding factor
     * @param amount Amount to commit to
     * @return 33-byte commitment
     * @throws std::runtime_error on failure
     */
    static std::vector<uint8_t> commit(const std::vector<uint8_t>& blinding, uint64_t amount) {
        if (blinding.size() != PEDERSEN_BLINDING_SIZE) {
            throw std::invalid_argument("Blinding factor must be 32 bytes");
        }

        std::vector<uint8_t> commitment(PEDERSEN_COMMITMENT_SIZE);

        if (pedersen_commit(commitment.data(), blinding.data(), amount) != 1) {
            throw std::runtime_error("Failed to create Pedersen commitment");
        }

        return commitment;
    }

    /**
     * Verify commitment sum (transaction balance check)
     *
     * @param inputs Input commitments
     * @param outputs Output commitments
     * @return true if balanced, false otherwise
     * @throws std::invalid_argument on invalid input
     */
    static bool verifySum(
        const std::vector<std::vector<uint8_t>>& inputs,
        const std::vector<std::vector<uint8_t>>& outputs
    ) {
        // Validate sizes
        for (const auto& input : inputs) {
            if (input.size() != PEDERSEN_COMMITMENT_SIZE) {
                throw std::invalid_argument("Invalid commitment size");
            }
        }
        for (const auto& output : outputs) {
            if (output.size() != PEDERSEN_COMMITMENT_SIZE) {
                throw std::invalid_argument("Invalid commitment size");
            }
        }

        // Flatten arrays (secp256k1 expects contiguous memory)
        std::vector<uint8_t> positive_flat;
        for (const auto& input : inputs) {
            positive_flat.insert(positive_flat.end(), input.begin(), input.end());
        }

        std::vector<uint8_t> negative_flat;
        for (const auto& output : outputs) {
            negative_flat.insert(negative_flat.end(), output.begin(), output.end());
        }

        return pedersen_verify_commitment_sum(
            positive_flat.empty() ? nullptr : positive_flat.data(),
            inputs.size(),
            negative_flat.empty() ? nullptr : negative_flat.data(),
            outputs.size()
        ) == 1;
    }

    /**
     * Generate a random blinding factor
     *
     * @return 32-byte blinding factor
     * @throws std::runtime_error on failure
     */
    static std::vector<uint8_t> generateBlinding() {
        std::vector<uint8_t> blinding(PEDERSEN_BLINDING_SIZE);

        if (pedersen_generate_blinding(blinding.data()) != 1) {
            throw std::runtime_error("Failed to generate blinding factor");
        }

        return blinding;
    }
};

} // namespace crypto
} // namespace dinero

#endif // __cplusplus
