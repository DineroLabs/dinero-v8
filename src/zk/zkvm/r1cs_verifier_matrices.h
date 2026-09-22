#pragma once
// Research (claude/shielded-v2 Task 7): per-shape compressed sparse row form of the R1CS
// matrices for the verifier's M~(rx, ry) evaluation. Built once per trusted circuit shape,
// immutable afterwards, shared read-only by every verify of that shape. Coefficients equal to
// +1 / -1 are tagged so the walk adds/subtracts instead of multiplying. The result is the same
// field element the constraint walk in r1cs_spartan_verify computes (commutative/associative
// field arithmetic); the compat regression pins the verdict equality.
#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/scalar.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dinero { namespace zk { namespace zkvm {

struct SparseMatrixCSR {
    std::vector<uint32_t> row_ptr;   // num_constraints + 1
    std::vector<uint32_t> col;       // nnz, already filtered to < n_zp
    std::vector<Scalar>   coeff;     // nnz (unused entries for kind 1/2 hold the value anyway)
    std::vector<uint8_t>  kind;      // 0 = general, 1 = +1, 2 = -1
    size_t nnz() const { return col.size(); }
};

// Immutable trusted verifier context for one circuit: identity (structure hash) + matrices.
// Built only through Build() from a trusted R1CS; no public mutable state. A verify call that
// receives one requires proof.circuit_hash == circuit_hash() and, when the caller also supplies an
// expected hash, expected == circuit_hash(). Dimensions alone never identify a circuit.
class R1CSVerifierMatrices {
public:
    static R1CSVerifierMatrices Build(const R1CS& cs);
    // Kept for existing callers; identical to Build().
    static R1CSVerifierMatrices FromR1CS(const R1CS& cs) { return Build(cs); }

    const std::vector<uint8_t>& circuit_hash() const { return circuit_hash_; }
    const SparseMatrixCSR& a() const { return a_; }
    const SparseMatrixCSR& b() const { return b_; }
    const SparseMatrixCSR& c() const { return c_; }
    size_t num_constraints() const { return num_constraints_; }
    size_t num_variables() const { return num_variables_; }
    size_t n_zp() const { return n_zp_; }
    size_t nnz_total() const { return nnz_total_; }
    size_t nnz_one() const { return nnz_one_; }
    size_t nnz_neg_one() const { return nnz_neg_one_; }

    // Test access type: declared unconditionally so every translation unit sees the same class
    // definition (ODR). It is DEFINED only in test sources; the library never defines or uses it.
    friend struct R1CSVerifierMatricesTestAccess;

private:
    R1CSVerifierMatrices() = default;
    std::vector<uint8_t> circuit_hash_;
    SparseMatrixCSR a_, b_, c_;
    size_t num_constraints_ = 0, num_variables_ = 0, n_zp_ = 0;
    size_t nnz_total_ = 0, nnz_one_ = 0, nnz_neg_one_ = 0;
    friend void AppendCsrTerms(SparseMatrixCSR&, const LinearCombination&, size_t, const Scalar&, const Scalar&, R1CSVerifierMatrices&);
};

// M~_combined(rx, ry) = sum_i eq_rx[i] * (a_i + rho*b_i + rho^2*c_i), a_i = sum_j A_ij * eq_ry[j].
// threads = 1 is serial; more splits the rows into contiguous chunks with private partial sums.
Scalar EvalMatrixCombinationCSR(const R1CSVerifierMatrices& m,
                                const std::vector<Scalar>& eq_rx,
                                const std::vector<Scalar>& eq_ry,
                                const Scalar& rho, const Scalar& rho2,
                                size_t nc_check, size_t threads);

}}}  // namespace dinero::zk::zkvm
