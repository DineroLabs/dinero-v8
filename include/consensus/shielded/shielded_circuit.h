#pragma once
/**
 * Shielded pool ZK circuits — spend and output proofs.
 *
 * Spend proof (published with each ShieldedSpend):
 *   Public inputs:  nullifier, anchor (commitment tree root)
 *   Private inputs: secret_key, leaf_index, value, randomness,
 *                   merkle_path[TREE_DEPTH] siblings
 *
 *   The circuit proves:
 *     1. public_key = Poseidon(secret_key, 0)
 *     2. commitment = Poseidon(Poseidon(value, public_key), randomness)
 *     3. Merkle path from commitment to anchor is valid
 *     4. nullifier = Poseidon(secret_key, leaf_index)
 *
 *   Constraint count: ~240 × (1 + 1 + TREE_DEPTH + 1) ≈ 8,400
 *
 * Output proof (published with each ShieldedOutput):
 *   Public inputs:  commitment
 *   Private inputs: value, public_key, randomness
 *
 *   The circuit proves:
 *     commitment = Poseidon(Poseidon(value, public_key), randomness)
 *
 *   Constraint count: ~480
 *
 * Both circuits use Poseidon-2 over secp256k1's scalar field, matching
 * the native evaluator in commitment_tree.cpp. The R1CS gadget
 * (poseidon2_gadget) constrains the same permutation — bit-for-bit.
 */

#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_tx.h"  // ValueCommitment (33-byte cv)

#include <array>
#include <cstdint>
#include <vector>

// Forward declarations — avoid pulling full ZK headers into consensus.
namespace dinero::zk::zkvm {
    class R1CS;
    struct SpartanProof;
    class GeneratorSet;
}

struct secp256k1_context_struct;

namespace dinero::consensus::shielded {

// ── Spend proof ──────────────────────────────────────────────────────

struct SpendWitness {
    Hash                                secret_key;
    uint64_t                            leaf_index;
    Hash                                value;       ///< value encoded as scalar
    Hash                                randomness;
    Hash                                d;           ///< diversifier (Phase 2 wave 5)
    Hash                                rcv;         ///< Pedersen blinding (Phase 3 wave 1)
    std::array<Hash, TREE_DEPTH>        merkle_path; ///< sibling hashes, leaf-to-root
};

struct SpendPublicInputs {
    Hash     nullifier;
    Hash     anchor;     ///< commitment tree root
    /// Audit Critical #1 (cv-binding): the bundle's 33-byte Pedersen value
    /// commitment for this spend. Bound to the in-circuit note value when
    /// `cv_bound` is enabled (post-activation). Ignored by the legacy
    /// (pre-activation) circuit, which leaves it default-zero.
    ValueCommitment cv{};
};

/**
 * Build the R1CS for a spend proof. Allocates variables, adds
 * constraints, returns the constraint system ready for Spartan.
 *
 * `cv_bound` selects the circuit version:
 *   - false: the LEGACY circuit (audit Critical #1 still open). Emits a
 *     byte-for-byte identical R1CS to the pre-fix circuit so pre-activation
 *     proofs keep verifying under the same verifying key.
 *   - true:  the cv-BOUND circuit. Adds `cv` (the Pedersen value commitment)
 *     as a public-input EC point and `rcv` (its blinding) as a witness, and
 *     constrains cv == val·V + rcv·G with the SAME `val` that feeds the note
 *     commitment and 64-bit range check. Closes the mint-from-nothing hole.
 */
zk::zkvm::R1CS BuildSpendCircuit(const SpendWitness& witness,
                                  const SpendPublicInputs& pub,
                                  bool cv_bound = false);

/**
 * Generate a Spartan proof for a spend.
 * Returns serialized proof bytes. `cv_bound` must match the consensus rule
 * at the spending height (see BuildSpendCircuit). cv-bound proofs carry a
 * distinct version byte from legacy proofs.
 */
std::vector<uint8_t> ProveSpend(const SpendWitness& witness,
                                 const SpendPublicInputs& pub,
                                 secp256k1_context_struct* ctx,
                                 bool bind_public_inputs = true,
                                 bool cv_bound = false);

/**
 * Verify a spend proof against public inputs.
 *
 * `bind_public_inputs` selects the consensus rule: true (default) = the public-input-
 * bound rule (CONFIRMED-CRIT-05 fix); false = the pre-fix unbound rule, used ONLY when
 * validating blocks below the shielded-input-binding activation height. Must match the
 * rule the proof was produced under.
 */
bool VerifySpend(const std::vector<uint8_t>& proof_bytes,
                 const SpendPublicInputs& pub,
                 secp256k1_context_struct* ctx,
                 bool bind_public_inputs = true,
                 bool cv_bound = false);

// NOTE: audit-only transcript-desync provers (ProveSpend_AuditDesync /
// ProveOutput_AuditDesync) used by the public-input-binding regression tests are
// declared in the test-only header src/test/shielded_audit_desync.h — deliberately
// kept OUT of this production consensus API.

// ── Output proof ─────────────────────────────────────────────────────

struct OutputWitness {
    Hash value;
    Hash public_key;
    Hash randomness;
    Hash d;          ///< diversifier (Phase 2 wave 5 — bound into commitment)
    Hash rcv;        ///< Pedersen blinding (Phase 3 wave 1)
};

struct OutputPublicInputs {
    Hash commitment;
    /// Audit Critical #1 (cv-binding): the bundle's 33-byte Pedersen value
    /// commitment for this output. See SpendPublicInputs::cv.
    ValueCommitment cv{};
};

zk::zkvm::R1CS BuildOutputCircuit(const OutputWitness& witness,
                                   const OutputPublicInputs& pub,
                                   bool cv_bound = false);

std::vector<uint8_t> ProveOutput(const OutputWitness& witness,
                                  const OutputPublicInputs& pub,
                                  secp256k1_context_struct* ctx,
                                  bool bind_public_inputs = true,
                                  bool cv_bound = false);

bool VerifyOutput(const std::vector<uint8_t>& proof_bytes,
                  const OutputPublicInputs& pub,
                  secp256k1_context_struct* ctx,
                  bool bind_public_inputs = true,
                  bool cv_bound = false);

} // namespace dinero::consensus::shielded
