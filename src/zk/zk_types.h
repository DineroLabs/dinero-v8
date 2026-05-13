// Copyright (c) 2025 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DINERO_ZK_TYPES_H
#define DINERO_ZK_TYPES_H

#include <secp256k1.h>
#include <secp256k1_rangeproof.h>
#include <secp256k1_generator.h>
#include <vector>
#include <cstdint>
#include <array>
#include <memory>
#include <string>

/**
 * Zero-Knowledge Privacy Types for Dinero
 *
 * This module provides confidential transactions using:
 * - Pedersen commitments (hide amounts)
 * - Bulletproofs (prove amounts in range without revealing value)
 * - Stealth addresses (unlinkable recipients)
 */

namespace dinero {
namespace zk {

// Standard sizes
constexpr size_t BLINDING_FACTOR_SIZE = 32;
constexpr size_t COMMITMENT_SIZE = 33;
constexpr size_t BULLETPROOF_MAX_SIZE = 5134; // ~5KB per proof
constexpr size_t EPHEMERAL_KEY_SIZE = 33;

// Range proof parameters
constexpr uint64_t RANGE_PROOF_BITS = 64;  // Prove 0 <= v < 2^64
constexpr uint64_t MAX_CONFIDENTIAL_VALUE = UINT64_MAX;

// Blinding factor (random secret for Pedersen commitment)
using BlindingFactor = std::array<uint8_t, BLINDING_FACTOR_SIZE>;

// Commitment result (33 bytes compressed public key)
using CommitmentBytes = std::array<uint8_t, COMMITMENT_SIZE>;

/**
 * Pedersen Commitment
 * C = v*G + r*H
 *
 * Where:
 *   v = amount (secret)
 *   r = blinding factor (secret)
 *   G, H = elliptic curve generators
 *   C = commitment (public)
 */
struct PedersenCommitment {
    secp256k1_pedersen_commitment commitment; // Public commitment
    uint64_t value;                           // Secret (only for wallet)
    BlindingFactor blinding_factor;           // Secret (only for wallet)

    PedersenCommitment() : value(0) {
        blinding_factor.fill(0);
    }

    // Serialize commitment to bytes
    CommitmentBytes Serialize() const;

    // Deserialize from bytes
    bool Deserialize(const CommitmentBytes& bytes);
};

/**
 * Range Proof (Bulletproof)
 * Proves 0 <= v < 2^64 without revealing v
 */
struct RangeProof {
    std::vector<uint8_t> proof;  // Bulletproof data (~5KB)
    uint64_t min_value;          // Minimum value (usually 0)
    uint64_t max_value;          // Maximum value (usually 2^64-1)
    std::array<uint8_t, 32> nonce;  // Nonce for rewinding (share with receiver!)

    RangeProof() : min_value(0), max_value(MAX_CONFIDENTIAL_VALUE) {
        nonce.fill(0);
    }

    // Get proof size
    size_t Size() const { return proof.size(); }

    // Is proof valid (non-empty)
    bool IsValid() const { return !proof.empty() && proof.size() <= BULLETPROOF_MAX_SIZE; }
};

/**
 * Confidential Transaction Input
 * References a previous confidential output
 */
struct ConfidentialInput {
    PedersenCommitment commitment;  // Commitment from previous output
    BlindingFactor blinding_factor; // Needed to prove ownership

    ConfidentialInput() = default;
};

/**
 * Confidential Transaction Output
 * Creates a new confidential output with hidden amount
 */
struct ConfidentialOutput {
    PedersenCommitment commitment;  // Public commitment
    RangeProof range_proof;         // Proof that amount is in valid range
    CommitmentBytes ephemeral_key;  // For stealth addresses (optional)

    ConfidentialOutput() = default;

    // Verify range proof
    bool VerifyRangeProof(secp256k1_context* ctx) const;
};

/**
 * RAII wrapper for secp256k1 context
 */
class Secp256k1Context {
public:
    Secp256k1Context();
    ~Secp256k1Context();

    // No copy
    Secp256k1Context(const Secp256k1Context&) = delete;
    Secp256k1Context& operator=(const Secp256k1Context&) = delete;

    // Get raw context
    secp256k1_context* Get() { return ctx_; }
    const secp256k1_context* Get() const { return ctx_; }

private:
    secp256k1_context* ctx_;
};

/**
 * ZK Error codes
 */
enum class ZKError {
    OK = 0,
    INVALID_COMMITMENT,
    INVALID_RANGE_PROOF,
    BALANCE_MISMATCH,
    INVALID_BLINDING_FACTOR,
    CONTEXT_ERROR,
    SERIALIZATION_ERROR
};

/**
 * Result type for ZK operations
 */
template<typename T>
struct ZKResult {
    T value;
    ZKError error;
    std::string error_message;

    ZKResult() : error(ZKError::OK) {}
    ZKResult(T val) : value(std::move(val)), error(ZKError::OK) {}
    ZKResult(ZKError err, std::string msg)
        : error(err), error_message(std::move(msg)) {}

    bool IsOk() const { return error == ZKError::OK; }
    operator bool() const { return IsOk(); }
};

} // namespace zk
} // namespace dinero

#endif // DINERO_ZK_TYPES_H
