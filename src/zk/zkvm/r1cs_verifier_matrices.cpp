#include "zk/zkvm/r1cs_verifier_matrices.h"
#include "zk/zkvm/r1cs_spartan.h"
#include <algorithm>
#include <thread>

namespace dinero { namespace zk { namespace zkvm {
void AppendCsrTerms(SparseMatrixCSR& m, const LinearCombination& lc, size_t n_zp, const Scalar& one,
                    const Scalar& neg_one, R1CSVerifierMatrices& stats) {
    for (const auto& t : lc.terms()) {
        if (t.var.index >= n_zp) continue;
        m.col.push_back(static_cast<uint32_t>(t.var.index));
        m.coeff.push_back(t.coeff);
        uint8_t k = 0;
        if (t.coeff == one) { k = 1; ++stats.nnz_one_; }
        else if (t.coeff == neg_one) { k = 2; ++stats.nnz_neg_one_; }
        m.kind.push_back(k);
        ++stats.nnz_total_;
    }
    m.row_ptr.push_back(static_cast<uint32_t>(m.col.size()));
}
namespace {
size_t next_pow2_local(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

inline Scalar RowDot(const SparseMatrixCSR& m, size_t i, const std::vector<Scalar>& eq_ry) {
    Scalar acc = Scalar::zero();
    const uint32_t b = m.row_ptr[i], e = m.row_ptr[i + 1];
    for (uint32_t k = b; k < e; ++k) {
        const Scalar& y = eq_ry[m.col[k]];
        switch (m.kind[k]) {
            case 1: acc += y; break;
            case 2: acc = acc - y; break;
            default: acc += m.coeff[k] * y; break;
        }
    }
    return acc;
}
}  // namespace

R1CSVerifierMatrices R1CSVerifierMatrices::Build(const R1CS& cs) {
    R1CSVerifierMatrices m;
    m.num_constraints_ = cs.num_constraints();
    m.num_variables_ = cs.num_variables();
    m.n_zp_ = next_pow2_local(m.num_variables_);
    m.circuit_hash_ = spartan_hash_r1cs_structure(cs);
    const Scalar one = Scalar::one();
    const Scalar neg_one = -Scalar::one();
    for (SparseMatrixCSR* s : {&m.a_, &m.b_, &m.c_}) s->row_ptr.push_back(0);
    for (const auto& c : cs.constraints()) {
        AppendCsrTerms(m.a_, c.a, m.n_zp_, one, neg_one, m);
        AppendCsrTerms(m.b_, c.b, m.n_zp_, one, neg_one, m);
        AppendCsrTerms(m.c_, c.c, m.n_zp_, one, neg_one, m);
    }
    return m;
}

Scalar EvalMatrixCombinationCSR(const R1CSVerifierMatrices& m, const std::vector<Scalar>& eq_rx,
                                const std::vector<Scalar>& eq_ry, const Scalar& rho, const Scalar& rho2,
                                size_t nc_check, size_t threads) {
    auto range = [&](size_t begin, size_t end) {
        Scalar acc = Scalar::zero();
        for (size_t i = begin; i < end; ++i) {
            const Scalar ai = RowDot(m.a(), i, eq_ry);
            const Scalar bi = RowDot(m.b(), i, eq_ry);
            const Scalar ci = RowDot(m.c(), i, eq_ry);
            acc += eq_rx[i] * (ai + rho * bi + rho2 * ci);
        }
        return acc;
    };
    const size_t nthreads = std::max<size_t>(1, std::min<size_t>(threads, 64));
    if (nc_check < 16384 || nthreads == 1) return range(0, nc_check);
    std::vector<Scalar> partial(nthreads, Scalar::zero());
    std::vector<std::thread> pool;
    const size_t chunk = (nc_check + nthreads - 1) / nthreads;
    for (size_t t = 0; t < nthreads; ++t) {
        const size_t b = t * chunk, e = std::min(nc_check, b + chunk);
        pool.emplace_back([&, t, b, e] { partial[t] = range(b, e); });
    }
    for (auto& th : pool) th.join();
    Scalar out = Scalar::zero();
    for (const Scalar& p : partial) out += p;
    return out;
}

}}}  // namespace dinero::zk::zkvm
