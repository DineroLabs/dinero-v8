#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Bulletproofs FFI - C/C++ Interface to Dalek Bulletproofs
// ============================================================================
//
// This header provides a C-compatible interface to the Rust Dalek Bulletproofs
// library for use in Dinero's confidential transactions.
//
// The Dalek implementation is the gold standard for Bulletproofs:
// - Used by Grin, Monero, MobileCoin, Zcash prototypes
// - Formally verified and battle-tested
// - High performance with optimized curve25519-dalek
//
// References:
// - https://github.com/dalek-cryptography/bulletproofs
// - https://eprint.iacr.org/2017/1066.pdf (Bulletproofs paper)

// ============================================================================
// Constants
// ============================================================================

/// Maximum number of bits for range proofs
#define BULLETPROOFS_MAX_RANGE_BITS 64

/// Maximum size of a 64-bit range proof in bytes (~674 bytes typical)
#define BULLETPROOFS_MAX_PROOF_SIZE 2048

/// Commitment size (32 bytes for Ristretto255 point)
#define BULLETPROOFS_COMMITMENT_SIZE 32

/// Blinding factor size (32 bytes scalar)
#define BULLETPROOFS_BLINDING_SIZE 32

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize Bulletproofs library
 * Must be called once before using any other functions
 *
 * @return 0 on success, -1 on error
 *
 * @note This function is not thread-safe and should be called once at startup
 */
int bp_init(void);

/**
 * Check if Bulletproofs library is initialized
 *
 * @return 1 if initialized, 0 if not
 */
int bp_is_initialized(void);

// ============================================================================
// Range Proof Generation
// ============================================================================

/**
 * Generate a Bulletproof range proof for a value
 *
 * Creates a zero-knowledge proof that `value` is in the range [0, 2^64-1]
 * without revealing the actual value. The proof size is ~674 bytes for 64-bit proofs.
 *
 * @param value         The value to prove (0 to 2^64-1)
 * @param blind_ptr     32-byte blinding factor for the commitment
 * @param proof_out     Output buffer for the proof (must be at least 2048 bytes)
 * @param proof_len_out Output length of the proof in bytes
 * @return 0 on success, -1 on error
 *
 * @note The blinding factor should be cryptographically random
 * @note The same blinding factor must be used for commitment creation
 */
int bp_generate(
    uint64_t value,
    const uint8_t* blind_ptr,
    uint8_t* proof_out,
    size_t* proof_len_out
);

// ============================================================================
// Range Proof Verification
// ============================================================================

/**
 * Verify a Bulletproof range proof
 *
 * Verifies that a commitment opens to some value in [0, 2^64-1] without
 * learning the value itself. This is a zero-knowledge verification.
 *
 * @param commitment_ptr 32-byte Ristretto255 commitment point
 * @param proof_ptr      Serialized Bulletproof
 * @param proof_len      Length of the proof in bytes
 * @return 1 if proof is valid, 0 if invalid, -1 on error
 *
 * @note The commitment format must match Ristretto255 encoding
 * @note Verification is significantly faster than proof generation
 */
int bp_verify(
    const uint8_t* commitment_ptr,
    const uint8_t* proof_ptr,
    size_t proof_len
);

// ============================================================================
// Batch Verification (Optimization)
// ============================================================================

/**
 * Verify multiple Bulletproofs in a batch
 *
 * Batch verification is ~2-3x faster than verifying proofs individually
 * due to multi-scalar multiplication optimizations. Use this for block
 * validation where multiple confidential outputs are present.
 *
 * @param commitments_ptr Array of pointers to 32-byte commitments
 * @param proofs_ptr      Array of pointers to serialized proofs
 * @param proof_lens_ptr  Array of proof lengths
 * @param count           Number of proofs to verify
 * @return 1 if all proofs are valid, 0 if any invalid, -1 on error
 *
 * @note All arrays must have `count` elements
 * @note If any proof is invalid, the entire batch fails
 */
int bp_verify_batch(
    const uint8_t** commitments_ptr,
    const uint8_t** proofs_ptr,
    const size_t* proof_lens_ptr,
    size_t count
);

// ============================================================================
// Rewind Functions (Amount Recovery)
// ============================================================================

/**
 * Generate a Bulletproof with rewind capability
 *
 * Creates a range proof with encrypted value/blind prepended, allowing
 * anyone with the nonce to recover both the amount and blinding factor.
 *
 * Output format: [encrypted_value (8 bytes) | encrypted_blind (32 bytes) | proof]
 * Total size: ~714 bytes (674 + 40 overhead)
 *
 * @param value         The value to prove (0 to 2^64-1)
 * @param blind_ptr     32-byte blinding factor
 * @param nonce_ptr     32-byte rewind nonce (typically ECDH shared secret)
 * @param proof_out     Output buffer for proof (must be at least 2048 bytes)
 * @param proof_len_out Output proof length
 * @return 0 on success, -1 on error
 *
 * @note The nonce should be derived from ECDH(sender_ephemeral, recipient_view)
 * @note Use this instead of bp_generate() for confidential transactions
 */
int bp_generate_with_nonce(
    uint64_t value,
    const uint8_t* blind_ptr,
    const uint8_t* nonce_ptr,
    uint8_t* proof_out,
    size_t* proof_len_out
);

/**
 * Rewind a Bulletproof to recover amount and blinding factor
 *
 * Decrypts the embedded value data using the rewind nonce, allowing
 * recipients to learn what amount they received.
 *
 * @param commitment_ptr 32-byte Ristretto commitment
 * @param proof_ptr      Serialized proof (from bp_generate_with_nonce)
 * @param proof_len      Length of proof in bytes
 * @param nonce_ptr      32-byte rewind nonce (must match generation)
 * @param value_out      OUT: Recovered amount
 * @param blind_out      OUT: Recovered blinding factor (32 bytes)
 * @return 1 if rewound successfully, 0 if wrong nonce, -1 on error
 *
 * @note Returns 0 if the nonce doesn't match (output not yours)
 * @note Returns -1 on malformed proof or other errors
 */
int bp_rewind(
    const uint8_t* commitment_ptr,
    const uint8_t* proof_ptr,
    size_t proof_len,
    const uint8_t* nonce_ptr,
    uint64_t* value_out,
    uint8_t* blind_out
);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Get the maximum size of a Bulletproof for the given bit length
 *
 * @param n_bits Number of bits in the range proof (typically 64)
 * @return Maximum proof size in bytes, or 0 on error
 *
 * @note For 64-bit proofs with rewind, this returns ~712 bytes + 40 = ~752 bytes
 */
size_t bp_max_proof_size(size_t n_bits);

/**
 * Get library version string
 *
 * @return Pointer to static version string (do not free)
 */
const char* bp_version(void);

// ============================================================================
// Commitment Arithmetic (For Balance Verification)
// ============================================================================

/**
 * Add two Ristretto255 commitments
 *
 * Computes: result = commitment_a + commitment_b
 * Used for summing commitments in balance verification.
 *
 * @param commitment_a_ptr First commitment (32 bytes compressed Ristretto)
 * @param commitment_b_ptr Second commitment (32 bytes compressed Ristretto)
 * @param result_out       Output buffer for result (must be 32 bytes)
 * @return 1 on success, 0 on failure (invalid commitments), -1 on error
 */
int commitment_add(
    const uint8_t* commitment_a_ptr,
    const uint8_t* commitment_b_ptr,
    uint8_t* result_out
);

/**
 * Subtract two Ristretto255 commitments
 *
 * Computes: result = commitment_a - commitment_b
 *
 * @param commitment_a_ptr First commitment (32 bytes)
 * @param commitment_b_ptr Second commitment (32 bytes)
 * @param result_out       Output buffer (32 bytes)
 * @return 1 on success, 0 on failure, -1 on error
 */
int commitment_sub(
    const uint8_t* commitment_a_ptr,
    const uint8_t* commitment_b_ptr,
    uint8_t* result_out
);

/**
 * Create a commitment from a transparent value
 *
 * Creates: commitment = value * H + 0 * G
 * (Pedersen commitment with zero blinding factor)
 *
 * Used for converting transparent outputs to commitments for balance verification.
 *
 * @param value          Transparent value (uint64)
 * @param commitment_out Output buffer (32 bytes)
 * @return 1 on success, -1 on error
 */
int commitment_from_value(
    uint64_t value,
    uint8_t* commitment_out
);

/**
 * Create a full Pedersen commitment from value and blinding factor
 *
 * Creates: commitment = value * H + blinding * G
 *
 * This is the FULL Pedersen commitment used in confidential transactions.
 * The blinding factor hides the value and enables commitment arithmetic.
 *
 * @param value          Value to commit (uint64)
 * @param blinding_ptr   32-byte blinding factor (canonical scalar)
 * @param commitment_out Output buffer (32 bytes)
 * @return 1 on success, -1 on error (null pointer, invalid blinding)
 *
 * @note The blinding factor must be a canonical Curve25519 scalar
 * @note Use generate_random_blinding() to create valid blinding factors
 *
 * Example:
 * @code
 * uint8_t blinding[32];
 * generate_random_blinding(blinding);
 *
 * uint8_t commitment[32];
 * commitment_create(1000, blinding, commitment);
 * @endcode
 */
int commitment_create(
    uint64_t value,
    const uint8_t* blinding_ptr,
    uint8_t* commitment_out
);

/**
 * Check if a commitment is the identity point (zero)
 *
 * @param commitment_ptr Commitment to check (32 bytes)
 * @return 1 if commitment is identity, 0 if not, -1 on error
 */
int commitment_is_identity(
    const uint8_t* commitment_ptr
);

// ============================================================================
// Test Utilities
// ============================================================================

/**
 * Generate a random canonical Curve25519 scalar (blinding factor)
 *
 * This function generates a cryptographically secure random scalar that is
 * guaranteed to be canonical (< curve order). Useful for testing and for
 * wallet implementations that need random blinding factors.
 *
 * @param blind_out Output buffer for the 32-byte scalar (must not be null)
 * @return 0 on success, -1 on error (null pointer)
 *
 * @note The output is a canonical scalar suitable for use with bp_generate()
 * @note Uses OsRng for cryptographically secure randomness
 */
int generate_random_blinding(uint8_t* blind_out);

// ============================================================================
// Error Codes
// ============================================================================

#define BULLETPROOFS_OK              0   // Success
#define BULLETPROOFS_ERROR          -1   // Generic error
#define BULLETPROOFS_NOT_INIT       -2   // Library not initialized
#define BULLETPROOFS_INVALID_INPUT  -3   // Invalid input parameters
#define BULLETPROOFS_PROOF_INVALID  -4   // Proof verification failed

#ifdef __cplusplus
}
#endif

// ============================================================================
// C++ Wrapper (RAII)
// ============================================================================

#ifdef __cplusplus

#include <vector>
#include <stdexcept>
#include <memory>

namespace dinero {
namespace crypto {

/**
 * RAII wrapper for Bulletproofs library
 * Automatically initializes on first use
 */
class BulletproofsLibrary {
public:
    /**
     * Get singleton instance and ensure initialization
     */
    static BulletproofsLibrary& instance() {
        static BulletproofsLibrary lib;
        return lib;
    }

    /**
     * Check if library is initialized
     */
    bool isInitialized() const {
        return bp_is_initialized() == 1;
    }

private:
    BulletproofsLibrary() {
        if (bp_init() != 0) {
            throw std::runtime_error("Failed to initialize Bulletproofs library");
        }
    }

    ~BulletproofsLibrary() = default;

    // Non-copyable
    BulletproofsLibrary(const BulletproofsLibrary&) = delete;
    BulletproofsLibrary& operator=(const BulletproofsLibrary&) = delete;
};

/**
 * C++ wrapper for Bulletproof range proof generation
 */
class BulletproofRangeProof {
public:
    /**
     * Generate a range proof
     *
     * @param value Value to prove (0 to 2^64-1)
     * @param blinding 32-byte blinding factor
     * @return Serialized proof
     * @throws std::runtime_error on failure
     */
    static std::vector<uint8_t> generate(uint64_t value, const std::vector<uint8_t>& blinding) {
        // Ensure library is initialized
        BulletproofsLibrary::instance();

        if (blinding.size() != BULLETPROOFS_BLINDING_SIZE) {
            throw std::invalid_argument("Blinding factor must be 32 bytes");
        }

        std::vector<uint8_t> proof(BULLETPROOFS_MAX_PROOF_SIZE);
        size_t proof_len = 0;

        int result = bp_generate(
            value,
            blinding.data(),
            proof.data(),
            &proof_len
        );

        if (result != 0) {
            throw std::runtime_error("Failed to generate Bulletproof");
        }

        proof.resize(proof_len);
        return proof;
    }

    /**
     * Verify a range proof
     *
     * @param commitment 32-byte Ristretto255 commitment
     * @param proof Serialized proof
     * @return true if valid, false if invalid
     * @throws std::runtime_error on error
     */
    static bool verify(const std::vector<uint8_t>& commitment, const std::vector<uint8_t>& proof) {
        // Ensure library is initialized
        BulletproofsLibrary::instance();

        if (commitment.size() != BULLETPROOFS_COMMITMENT_SIZE) {
            throw std::invalid_argument("Commitment must be 32 bytes");
        }

        if (proof.empty() || proof.size() > BULLETPROOFS_MAX_PROOF_SIZE) {
            throw std::invalid_argument("Invalid proof size");
        }

        int result = bp_verify(
            commitment.data(),
            proof.data(),
            proof.size()
        );

        if (result == -1) {
            throw std::runtime_error("Bulletproof verification error");
        }

        return result == 1;
    }

    /**
     * Batch verify multiple proofs (faster than individual verification)
     *
     * @param commitments Vector of 32-byte commitments
     * @param proofs Vector of serialized proofs
     * @return true if all valid, false if any invalid
     * @throws std::runtime_error on error
     */
    static bool verifyBatch(
        const std::vector<std::vector<uint8_t>>& commitments,
        const std::vector<std::vector<uint8_t>>& proofs
    ) {
        // Ensure library is initialized
        BulletproofsLibrary::instance();

        if (commitments.size() != proofs.size() || commitments.empty()) {
            throw std::invalid_argument("Commitments and proofs must have same non-zero size");
        }

        // Build pointer arrays
        std::vector<const uint8_t*> commitment_ptrs;
        std::vector<const uint8_t*> proof_ptrs;
        std::vector<size_t> proof_lens;

        for (const auto& commitment : commitments) {
            if (commitment.size() != BULLETPROOFS_COMMITMENT_SIZE) {
                throw std::invalid_argument("All commitments must be 32 bytes");
            }
            commitment_ptrs.push_back(commitment.data());
        }

        for (const auto& proof : proofs) {
            if (proof.empty() || proof.size() > BULLETPROOFS_MAX_PROOF_SIZE) {
                throw std::invalid_argument("Invalid proof size");
            }
            proof_ptrs.push_back(proof.data());
            proof_lens.push_back(proof.size());
        }

        int result = bp_verify_batch(
            commitment_ptrs.data(),
            proof_ptrs.data(),
            proof_lens.data(),
            commitments.size()
        );

        if (result == -1) {
            throw std::runtime_error("Batch verification error");
        }

        return result == 1;
    }

    /**
     * Generate a range proof with rewind capability
     *
     * @param value Value to prove (0 to 2^64-1)
     * @param blinding 32-byte blinding factor
     * @param nonce 32-byte rewind nonce (ECDH shared secret)
     * @return Serialized proof with embedded encrypted data
     * @throws std::runtime_error on failure
     */
    static std::vector<uint8_t> generateWithNonce(
        uint64_t value,
        const std::vector<uint8_t>& blinding,
        const std::vector<uint8_t>& nonce
    ) {
        // Ensure library is initialized
        BulletproofsLibrary::instance();

        if (blinding.size() != BULLETPROOFS_BLINDING_SIZE) {
            throw std::invalid_argument("Blinding factor must be 32 bytes");
        }

        if (nonce.size() != 32) {
            throw std::invalid_argument("Nonce must be 32 bytes");
        }

        std::vector<uint8_t> proof(BULLETPROOFS_MAX_PROOF_SIZE);
        size_t proof_len = 0;

        int result = bp_generate_with_nonce(
            value,
            blinding.data(),
            nonce.data(),
            proof.data(),
            &proof_len
        );

        if (result != 0) {
            throw std::runtime_error("Failed to generate Bulletproof with nonce");
        }

        proof.resize(proof_len);
        return proof;
    }

    /**
     * Rewind a range proof to recover the amount and blinding factor
     *
     * @param commitment 32-byte Ristretto255 commitment
     * @param proof Serialized proof (from generateWithNonce)
     * @param nonce 32-byte rewind nonce (must match generation)
     * @param value_out OUT: Recovered amount
     * @param blind_out OUT: Recovered blinding factor
     * @return true if rewound successfully, false if wrong nonce
     * @throws std::runtime_error on error
     */
    static bool rewind(
        const std::vector<uint8_t>& commitment,
        const std::vector<uint8_t>& proof,
        const std::vector<uint8_t>& nonce,
        uint64_t& value_out,
        std::vector<uint8_t>& blind_out
    ) {
        // Ensure library is initialized
        BulletproofsLibrary::instance();

        if (commitment.size() != BULLETPROOFS_COMMITMENT_SIZE) {
            throw std::invalid_argument("Commitment must be 32 bytes");
        }

        if (nonce.size() != 32) {
            throw std::invalid_argument("Nonce must be 32 bytes");
        }

        if (proof.empty() || proof.size() > BULLETPROOFS_MAX_PROOF_SIZE) {
            throw std::invalid_argument("Invalid proof size");
        }

        blind_out.resize(32);

        int result = bp_rewind(
            commitment.data(),
            proof.data(),
            proof.size(),
            nonce.data(),
            &value_out,
            blind_out.data()
        );

        if (result == -1) {
            throw std::runtime_error("Bulletproof rewind error");
        }

        return result == 1;  // 1 = success, 0 = wrong nonce
    }
};

} // namespace crypto
} // namespace dinero

#endif // __cplusplus
