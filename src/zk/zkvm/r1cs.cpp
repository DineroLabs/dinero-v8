// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/r1cs.h"
#include <cassert>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// LinearCombination
// ---------------------------------------------------------------------------

LinearCombination LinearCombination::operator+(const LinearCombination& other) const {
    LinearCombination result = *this;
    for (const auto& t : other.terms_) {
        result.terms_.push_back(t);
    }
    return result;
}

LinearCombination LinearCombination::operator-(const LinearCombination& other) const {
    LinearCombination result = *this;
    for (const auto& t : other.terms_) {
        result.terms_.push_back({-t.coeff, t.var});
    }
    return result;
}

LinearCombination LinearCombination::operator*(const Scalar& s) const {
    LinearCombination result;
    for (const auto& t : terms_) {
        result.terms_.push_back({t.coeff * s, t.var});
    }
    return result;
}

Scalar LinearCombination::evaluate(const std::vector<Scalar>& z) const {
    Scalar sum = Scalar::zero();
    for (const auto& t : terms_) {
        assert(t.var.index < z.size());
        sum += t.coeff * z[t.var.index];
    }
    return sum;
}

// ---------------------------------------------------------------------------
// R1CS
// ---------------------------------------------------------------------------

R1CS::R1CS() {
    // Index 0 is always the constant ONE
    witness_.push_back(Scalar::one());
}

Variable R1CS::alloc(const Scalar& value) {
    Variable v{witness_.size()};
    witness_.push_back(value);
    return v;
}

Variable R1CS::alloc_input(const Scalar& value) {
    // Public inputs go right after the constant ONE
    // Insert at position (1 + num_inputs_)
    size_t idx = 1 + num_inputs_;
    witness_.insert(witness_.begin() + idx, value);
    num_inputs_++;

    // Fix: all existing constraints reference variable indices.
    // Inserting in the middle shifts auxiliary variable indices.
    // Since inputs are allocated before aux variables in practice,
    // this is safe as long as inputs are allocated first.
    return Variable{idx};
}

void R1CS::constrain(LinearCombination a, LinearCombination b, LinearCombination c,
                     const std::string& label) {
    constraints_.push_back({std::move(a), std::move(b), std::move(c), label});
}

void R1CS::enforce_equal(const LinearCombination& a, const LinearCombination& b,
                         const std::string& label) {
    // (a - b) * 1 = 0
    constrain(a - b, LinearCombination(VAR_ONE), LinearCombination::constant(Scalar::zero()), label);
}

void R1CS::enforce_zero(const LinearCombination& lc, const std::string& label) {
    enforce_equal(lc, LinearCombination::constant(Scalar::zero()), label);
}

bool R1CS::is_satisfied() const {
    std::string dummy;
    return is_satisfied(dummy);
}

bool R1CS::is_satisfied(std::string& failing_constraint) const {
    for (size_t i = 0; i < constraints_.size(); ++i) {
        const auto& c = constraints_[i];
        Scalar a_val = c.a.evaluate(witness_);
        Scalar b_val = c.b.evaluate(witness_);
        Scalar c_val = c.c.evaluate(witness_);

        // Standard R1CS: A*B = C
        // Relaxed R1CS: A*B = u*C + E[i]
        Scalar lhs = a_val * b_val;
        Scalar rhs = c_val;

        if (relaxed_) {
            rhs = u_ * rhs;
            if (i < error_.size()) {
                rhs += error_[i];
            }
        }

        if (lhs != rhs) {
            failing_constraint = c.label.empty()
                ? "constraint_" + std::to_string(i)
                : c.label;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sparse matrix extraction
// ---------------------------------------------------------------------------

void R1CS::extract_entries(const LinearCombination& lc, size_t row,
                           std::vector<SparseEntry>& out) {
    for (const auto& t : lc.terms()) {
        if (!t.coeff.is_zero()) {
            out.push_back({row, t.var.index, t.coeff});
        }
    }
}

std::vector<R1CS::SparseEntry> R1CS::matrix_a() const {
    std::vector<SparseEntry> entries;
    for (size_t i = 0; i < constraints_.size(); ++i) {
        extract_entries(constraints_[i].a, i, entries);
    }
    return entries;
}

std::vector<R1CS::SparseEntry> R1CS::matrix_b() const {
    std::vector<SparseEntry> entries;
    for (size_t i = 0; i < constraints_.size(); ++i) {
        extract_entries(constraints_[i].b, i, entries);
    }
    return entries;
}

std::vector<R1CS::SparseEntry> R1CS::matrix_c() const {
    std::vector<SparseEntry> entries;
    for (size_t i = 0; i < constraints_.size(); ++i) {
        extract_entries(constraints_[i].c, i, entries);
    }
    return entries;
}

// ---- Cached structural constants ----

Variable R1CS::const_zero() {
    if (cached_zero_.index == SIZE_MAX) {
        cached_zero_ = alloc(Scalar::zero());
        constrain(LinearCombination(cached_zero_),
                  LinearCombination(VAR_ONE),
                  LinearCombination::constant(Scalar::zero()),
                  "const_zero");
    }
    return cached_zero_;
}

Variable R1CS::const_one() {
    if (cached_one_.index == SIZE_MAX) {
        cached_one_ = alloc(Scalar::one());
        constrain(LinearCombination(cached_one_),
                  LinearCombination(VAR_ONE),
                  LinearCombination::constant(Scalar::one()),
                  "const_one");
    }
    return cached_one_;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
