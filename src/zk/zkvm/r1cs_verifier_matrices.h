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

struct R1CSVerifierMatrices {
    SparseMatrixCSR a, b, c;
    size_t num_constraints = 0;
    size_t num_variables = 0;
    size_t n_zp = 0;                 // next_pow2(num_variables): column bound used by the verifier
    size_t nnz_total = 0, nnz_one = 0, nnz_neg_one = 0;

    static R1CSVerifierMatrices FromR1CS(const R1CS& cs);
};

// M~_combined(rx, ry) = sum_i eq_rx[i] * (a_i + rho*b_i + rho^2*c_i), a_i = sum_j A_ij * eq_ry[j].
// threads = 1 is serial; more splits the rows into contiguous chunks with private partial sums.
Scalar EvalMatrixCombinationCSR(const R1CSVerifierMatrices& m,
                                const std::vector<Scalar>& eq_rx,
                                const std::vector<Scalar>& eq_ry,
                                const Scalar& rho, const Scalar& rho2,
                                size_t nc_check, size_t threads);

}}}  // namespace dinero::zk::zkvm
