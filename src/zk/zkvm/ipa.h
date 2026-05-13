// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Inner Product Argument (IPA) on secp256k1
 *
 * Proves knowledge of vectors a, b such that:
 *   <a, b> = c   AND   P = <a, G> + <b, H> + c * Q
 *
 * where G, H are vectors of generator points and Q is a blinding generator.
 *
 * This is the core polynomial commitment used by:
 * - Bulletproofs R1CS proofs (proving constraint satisfaction)
 * - Nova final proofs (after folding all Tapscript steps)
 * - The complete ZK Tapscript proving stack
 *
 * The proof is O(log n) in size where n is the vector length.
 * No trusted setup. No pairings. Pure secp256k1.
 *
 * Based on: Bünz et al., "Bulletproofs: Short Proofs for Confidential
 * Transactions and More" (IEEE S&P 2018), Section 3.
 *
 * Modified for secp256k1 (no pairing) with Halo-style deferred verification
 * for efficient accumulation.
 */

#include "zk/zkvm/scalar.h"
#include "zk/zkvm/transcript.h"
#include <vector>

namespace dinero {
namespace zk {
namespace zkvm {

/**
 * NUMS generator set for IPA commitments.
 * Generated deterministically via hash_to_curve("Dinero_IPA_G_{i}")
 * so no trusted setup is needed.
 */
class GeneratorSet {
public:
    // Create n generators for G vector and n for H vector, plus Q.
    // Uses a global cache: generators are deterministic NUMS points,
    // so identical sizes return the same set without recomputation.
    static GeneratorSet create(size_t n, secp256k1_context* ctx);

    // Get a cached generator set. If the cache has >= n generators,
    // returns immediately. Otherwise creates and caches.
    static const GeneratorSet& cached(size_t n, secp256k1_context* ctx);

    // Clear the global cache (for testing)
    static void clear_cache();

    const std::vector<Point>& G() const { return G_; }
    const std::vector<Point>& H() const { return H_; }
    const Point& Q() const { return Q_; }
    size_t size() const { return G_.size(); }

private:
    std::vector<Point> G_; // Generators for "a" vector
    std::vector<Point> H_; // Generators for "b" vector
    Point Q_;              // Blinding generator for inner product
};

/**
 * IPA proof: O(log n) group elements + 2 scalars
 */
struct IPAProof {
    std::vector<Point> L; // Left fold commitments (log n)
    std::vector<Point> R; // Right fold commitments (log n)
    Scalar a;             // Final scalar a (after log n folds)
    Scalar b;             // Final scalar b (after log n folds)

    size_t num_rounds() const { return L.size(); }

    // Serialization
    std::vector<uint8_t> serialize(secp256k1_context* ctx) const;
    static bool deserialize(const std::vector<uint8_t>& data, IPAProof& out,
                            secp256k1_context* ctx);
};

/**
 * Pedersen vector commitment:
 *   C = <a, G> + <b, H> + <a,b> * Q
 *
 * Commits to vectors a, b and their inner product simultaneously.
 */
Point pedersen_vector_commit(
    const std::vector<Scalar>& a,
    const std::vector<Scalar>& b,
    const GeneratorSet& gens,
    secp256k1_context* ctx
);

/**
 * IPA Prover
 *
 * Given vectors a, b and generators G, H, Q, produces an O(log n) proof
 * that <a, b> = c and P = commit(a, b).
 *
 * Uses recursive halving: at each round, fold the vectors and generators
 * in half using a Fiat-Shamir challenge.
 */
IPAProof ipa_prove(
    const std::vector<Scalar>& a,
    const std::vector<Scalar>& b,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
);

/**
 * IPA Verifier
 *
 * Verifies that the proof is valid for commitment P and inner product c.
 * Runs in O(n) time (due to generator computation) but only needs
 * the O(log n) proof data.
 */
bool ipa_verify(
    const Point& commitment,
    const Scalar& inner_product,
    const IPAProof& proof,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
);

/**
 * Compute inner product of two scalar vectors: <a, b> = sum(a_i * b_i)
 */
Scalar inner_product(const std::vector<Scalar>& a, const std::vector<Scalar>& b);

} // namespace zkvm
} // namespace zk
} // namespace dinero
