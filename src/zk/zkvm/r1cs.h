// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Rank-1 Constraint System (R1CS) for Zero-Knowledge Proofs
 *
 * R1CS encodes arithmetic circuits as a set of rank-1 constraints:
 *   A_i · z  *  B_i · z  =  C_i · z    for all i
 *
 * where z = (1, x_1, ..., x_l, w_1, ..., w_m) is the witness vector.
 * x_1..x_l are public inputs, w_1..w_m are private witness values.
 *
 * This is the foundation for:
 * - Bulletproofs R1CS proofs on secp256k1
 * - Nova IVC folding for Tapscript VM execution
 * - The complete ZK Tapscript proving stack
 *
 * R1CS is the standard intermediate representation for zero-knowledge circuits.
 * Arithmetic operations (add, multiply) become constraints. Non-arithmetic
 * operations (SHA256, conditionals, comparisons) are decomposed into
 * R1CS-compatible constraint sets via gadgets.
 */

#include "zk/zkvm/scalar.h"
#include <cassert>
#include <vector>
#include <string>
#include <cstddef>

namespace dinero {
namespace zk {
namespace zkvm {

/**
 * A variable in the constraint system.
 * Index 0 is reserved for the constant ONE.
 * Indices 1..num_inputs are public inputs.
 * Indices (num_inputs+1)..num_vars are private witness values.
 */
struct Variable {
    size_t index;

    bool operator==(const Variable& other) const { return index == other.index; }
    bool operator!=(const Variable& other) const { return index != other.index; }
};

// The constant ONE variable (always index 0)
constexpr Variable VAR_ONE = {0};

/**
 * A linear combination: sum of (coefficient * variable) pairs.
 *
 * Represents expressions like: 3*x + 5*y + 1
 * as: [(3, x), (5, y), (1, ONE)]
 */
class LinearCombination {
public:
    struct Term {
        Scalar coeff;
        Variable var;
    };

    LinearCombination() = default;

    // Single variable with coefficient 1
    explicit LinearCombination(Variable v) {
        terms_.push_back({Scalar::one(), v});
    }

    // Single variable with given coefficient
    LinearCombination(const Scalar& coeff, Variable v) {
        terms_.push_back({coeff, v});
    }

    // Constant value (coefficient of VAR_ONE)
    static LinearCombination constant(const Scalar& c) {
        return LinearCombination(c, VAR_ONE);
    }

    // Add a term
    LinearCombination& add(const Scalar& coeff, Variable var) {
        terms_.push_back({coeff, var});
        return *this;
    }

    // Arithmetic with linear combinations
    LinearCombination operator+(const LinearCombination& other) const;
    LinearCombination operator-(const LinearCombination& other) const;
    LinearCombination operator*(const Scalar& s) const;

    // Evaluate given a full witness assignment z
    Scalar evaluate(const std::vector<Scalar>& z) const;

    const std::vector<Term>& terms() const { return terms_; }
    bool is_empty() const { return terms_.empty(); }

private:
    std::vector<Term> terms_;
};

// Convenience: scalar * linear combination
inline LinearCombination operator*(const Scalar& s, const LinearCombination& lc) {
    return lc * s;
}

/**
 * A single R1CS constraint: A · z * B · z = C · z
 *
 * This is the fundamental building block. Every arithmetic relationship
 * must be expressible as one or more rank-1 constraints.
 *
 * Examples:
 *   Multiplication: a * b = c          → A=[a], B=[b], C=[c]
 *   Addition: a + b = c                → A=[a+b], B=[1], C=[c]
 *   Boolean: x * (1-x) = 0            → A=[x], B=[1-x], C=[0]
 *   Equality: a - b = 0               → A=[a-b], B=[1], C=[0]
 */
struct R1CSConstraint {
    LinearCombination a, b, c;
    std::string label; // Debug label (empty in production)
};

/**
 * The R1CS constraint system.
 *
 * Manages variable allocation, witness assignment, and constraint tracking.
 * Used by gadgets to build circuits and by the prover/verifier for proof
 * generation and verification.
 *
 * Relaxed R1CS (for Nova folding):
 *   A · z * B · z = u * C · z + E
 * where u is a scalar (1 for standard R1CS) and E is an error vector.
 * set_relaxed(true) enables this mode.
 */
class R1CS {
public:
    R1CS();

    // ---- Variable allocation ----

    // Allocate a private witness variable with the given value
    Variable alloc(const Scalar& value);

    // Allocate a public input variable
    Variable alloc_input(const Scalar& value);

    // Get the constant ONE variable
    Variable one() const { return VAR_ONE; }

    // ---- Constraint management ----

    // Add constraint: a * b = c
    void constrain(LinearCombination a, LinearCombination b, LinearCombination c,
                   const std::string& label = "");

    // Enforce a == b (shorthand)
    void enforce_equal(const LinearCombination& a, const LinearCombination& b,
                       const std::string& label = "");

    // Enforce lc == 0
    void enforce_zero(const LinearCombination& lc, const std::string& label = "");

    // ---- Satisfaction check ----

    // Check if the current witness satisfies all constraints
    bool is_satisfied() const;

    // Check satisfaction and return the first failing constraint label
    bool is_satisfied(std::string& failing_constraint) const;

    // ---- Witness access ----

    // Get the value of a variable
    const Scalar& get_value(Variable v) const { return witness_[v.index]; }

    // Set the value of a variable (for witness updates during folding)
    void set_value(Variable v, const Scalar& val) { witness_[v.index] = val; }

    // Full witness vector: z = (1, x_1, ..., x_l, w_1, ..., w_m)
    const std::vector<Scalar>& witness() const { return witness_; }

    // Replace the entire witness vector (for Nova decider: replace synthesized
    // witness with the FOLDED running witness from Nova's running_witness_.W).
    // The new witness must have the same size as the current one.
    void set_witness(const std::vector<Scalar>& w) {
        assert(w.size() == witness_.size());
        witness_ = w;
    }

    // ---- Dimensions ----

    size_t num_constraints() const { return constraints_.size(); }
    size_t num_variables() const { return witness_.size(); }
    size_t num_inputs() const { return num_inputs_; }
    size_t num_aux() const { return witness_.size() - num_inputs_ - 1; }

    const std::vector<R1CSConstraint>& constraints() const { return constraints_; }

    // ---- Relaxed R1CS (for Nova IVC) ----

    void set_relaxed(bool relaxed) { relaxed_ = relaxed; }
    bool is_relaxed() const { return relaxed_; }

    // u scalar (1 for standard R1CS, variable for relaxed)
    void set_u(const Scalar& u) { u_ = u; }
    const Scalar& u() const { return u_; }

    // Error vector E (zero for standard R1CS)
    void set_error(std::vector<Scalar> e) { error_ = std::move(e); }
    const std::vector<Scalar>& error() const { return error_; }

    // ---- Cached structural constants ----
    // Lazily allocated zero and one variables, reusable by gadgets (SHA256, etc.)
    // Avoids re-allocating per-bit constants for every Word32 constant.
    Variable const_zero();
    Variable const_one();

    // ---- Sparse matrix representation ----
    // For the prover: extract A, B, C as sparse matrices
    struct SparseEntry {
        size_t row;    // constraint index
        size_t col;    // variable index
        Scalar value;  // coefficient
    };

    std::vector<SparseEntry> matrix_a() const;
    std::vector<SparseEntry> matrix_b() const;
    std::vector<SparseEntry> matrix_c() const;

private:
    std::vector<R1CSConstraint> constraints_;
    std::vector<Scalar> witness_; // z = (1, inputs..., aux...)
    size_t num_inputs_ = 0;

    // Relaxed R1CS state
    bool relaxed_ = false;
    Scalar u_ = Scalar::one();
    std::vector<Scalar> error_; // Error vector E

    // Cached constants (lazy, per-instance)
    Variable cached_zero_ = {SIZE_MAX};
    Variable cached_one_ = {SIZE_MAX};

    // Extract sparse entries from linear combination for a given constraint row
    static void extract_entries(const LinearCombination& lc, size_t row,
                                std::vector<SparseEntry>& out);
};

} // namespace zkvm
} // namespace zk
} // namespace dinero
