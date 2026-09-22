// Differential regression for the research proof profile (claude/shielded-v2 Task 7):
// the legacy Spartan profile keeps its historical accepted input language and resource
// behaviour; strictness applies to the E-less profile only. Review finding 2026-09-22
// (MemoryMD/evidence/shielded-v2-task7-review-2026-09-22).
#include "zk/zkvm/r1cs_spartan.h"
#include "zk/zkvm/r1cs_verifier_matrices.h"
#include "zk/zkvm/transcript.h"
#include <gtest/gtest.h>
#include <secp256k1.h>
#include <algorithm>

using namespace dinero::zk::zkvm;

namespace dinero { namespace zk { namespace zkvm {
// Test-only definition of the friend declared in r1cs_verifier_matrices.h. Lets a test forge a
// context's identity to prove that the matrix evaluation itself rejects a mismatched circuit.
struct R1CSVerifierMatricesTestAccess {
    static void ForgeIdentity(R1CSVerifierMatrices& m, const std::vector<uint8_t>& h) { m.circuit_hash_ = h; }
};
}}}  // namespace dinero::zk::zkvm

namespace {
struct Fixture {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    R1CS cs;
    std::vector<uint8_t> hash;
    Fixture() {
        for (int i = 0; i < 2; ++i) {
            auto x = cs.alloc(Scalar::one());
            cs.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x), "boolean");
        }
        hash = spartan_hash_r1cs_structure(cs);
    }
    ~Fixture() { secp256k1_context_destroy(ctx); }
    const GeneratorSet& gens() const {
        return GeneratorSet::cached(std::max<size_t>(4, std::max(HyraxParams::from_n(cs.num_variables()).n_cols,
                                                                   HyraxParams::from_n(cs.num_constraints()).n_cols)), ctx);
    }
    std::vector<uint8_t> prove(bool omit, const R1CS& which) {
        Transcript t("spartan.profile.compat");
        return r1cs_spartan_prove(which, std::vector<Scalar>(which.num_constraints(), Scalar::zero()),
                                  Scalar::one(), gens(), t, ctx, true, omit).serialize(ctx);
    }
    std::pair<bool, bool> decode_verify(const std::vector<uint8_t>& bytes, bool omit, size_t threads = 1) {
        SpartanProof p;
        const bool decoded = SpartanProof::deserialize(bytes, p, ctx, omit);
        if (!decoded) return {false, false};
        Transcript t("spartan.profile.compat");
        return {true, r1cs_spartan_verify(p, cs, cs.num_constraints(), cs.num_variables(), hash,
                                          Scalar::one(), gens(), t, ctx, true, true, omit, threads)};
    }
};
}  // namespace

TEST(SpartanProfileCompat, LegacyProofWithTrailingByteStillDecodesAndVerifies) {
    Fixture f;
    auto bytes = f.prove(false, f.cs);
    EXPECT_EQ(f.decode_verify(bytes, false), (std::pair{true, true}));
    bytes.push_back(0x42);
    // Historical accepted language: a trailing byte was never rejected by the legacy decoder.
    EXPECT_EQ(f.decode_verify(bytes, false), (std::pair{true, true}));
}

TEST(SpartanProfileCompat, ElessProfileRejectsTrailingBytes) {
    Fixture f;
    auto bytes = f.prove(true, f.cs);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(f.decode_verify(bytes, true), (std::pair{true, true}));
    bytes.push_back(0x42);
    EXPECT_EQ(f.decode_verify(bytes, true).first, false);
}

TEST(SpartanProfileCompat, ProfilesAreMutuallyExclusive) {
    Fixture f;
    const auto legacy = f.prove(false, f.cs);
    const auto eless = f.prove(true, f.cs);
    EXPECT_FALSE(f.decode_verify(legacy, true).second);
    EXPECT_FALSE(f.decode_verify(eless, false).second);
    EXPECT_TRUE(f.decode_verify(legacy, false).second);
    EXPECT_TRUE(f.decode_verify(eless, true).second);
}

TEST(SpartanProfileCompat, ElessProverRefusesNonZeroErrorAndVerifierRejectsNonZeroClaim) {
    Fixture f;
    std::vector<Scalar> E(f.cs.num_constraints(), Scalar::zero());
    E[0] = Scalar::one();
    Transcript t("spartan.profile.compat");
    const auto p = r1cs_spartan_prove(f.cs, E, Scalar::one(), f.gens(), t, f.ctx, true, true);
    EXPECT_TRUE(p.comm_W.C.empty());
    EXPECT_TRUE(p.outer_sc.empty());
    SpartanProof good;
    ASSERT_TRUE(SpartanProof::deserialize(f.prove(true, f.cs), good, f.ctx, true));
    good.Ez_claim = Scalar::one();
    EXPECT_FALSE(f.decode_verify(good.serialize(f.ctx), true).second);
}

TEST(SpartanProfileCompat, UnsatisfiedWitnessIsRejectedUnderBothProfiles) {
    Fixture f;
    R1CS bad;
    for (int i = 0; i < 2; ++i) {
        auto x = bad.alloc(Scalar::one() + Scalar::one());   // x*x != x
        bad.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x), "boolean");
    }
    EXPECT_FALSE(bad.is_satisfied());
    EXPECT_FALSE(f.decode_verify(f.prove(false, bad), false).second);
    const auto eless = f.prove(true, bad);
    EXPECT_TRUE(eless.empty() || !f.decode_verify(eless, true).second);
}

TEST(SpartanProfileCompat, MatrixThreadBudgetDoesNotChangeTheVerdict) {
    Fixture f;
    const auto legacy = f.prove(false, f.cs);
    for (size_t threads : {size_t{1}, size_t{2}, size_t{8}}) {
        EXPECT_TRUE(f.decode_verify(legacy, false, threads).second) << threads;
    }
    auto tampered = legacy;
    tampered[tampered.size() / 2] ^= 0x01;
    for (size_t threads : {size_t{1}, size_t{8}}) {
        EXPECT_FALSE(f.decode_verify(tampered, false, threads).second) << threads;
    }
}

TEST(SpartanProfileCompat, CsrMatrixWalkGivesTheSameVerdictAsTheConstraintWalk) {
    Fixture f;
    const auto m = R1CSVerifierMatrices::FromR1CS(f.cs);
    EXPECT_EQ(m.num_constraints(), f.cs.num_constraints());
    EXPECT_EQ(m.nnz_total(), 3u * f.cs.num_constraints());
    for (bool omit : {false, true}) {
        auto bytes = f.prove(omit, f.cs);
        for (int tamper = 0; tamper <= 1; ++tamper) {
            auto in = bytes; if (tamper) in[in.size() / 2] ^= 0x01;
            SpartanProof p; ASSERT_TRUE(SpartanProof::deserialize(in, p, f.ctx, omit) || tamper);
            if (!SpartanProof::deserialize(in, p, f.ctx, omit)) continue;
            Transcript t1("spartan.profile.compat"), t2("spartan.profile.compat");
            const bool walk = r1cs_spartan_verify(p, f.cs, f.cs.num_constraints(), f.cs.num_variables(), f.hash, Scalar::one(), f.gens(), t1, f.ctx, true, true, omit, 1, nullptr);
            const bool csr  = r1cs_spartan_verify(p, f.cs, f.cs.num_constraints(), f.cs.num_variables(), f.hash, Scalar::one(), f.gens(), t2, f.ctx, true, true, omit, 1, &m);
            EXPECT_EQ(walk, csr) << "omit=" << omit << " tamper=" << tamper;
            EXPECT_EQ(walk, !tamper) << "omit=" << omit;
        }
    }
    // A context built for a different circuit with the SAME dimensions is refused by identity.
    {
        R1CS twin;
        for (int i = 0; i < 2; ++i) {
            auto x = twin.alloc(Scalar::one());
            twin.constrain(LinearCombination(x), LinearCombination(x),
                           i == 0 ? LinearCombination(x) : LinearCombination(Scalar::one() + Scalar::one(), x) - LinearCombination(x), "b");
        }
        ASSERT_EQ(twin.num_constraints(), f.cs.num_constraints());
        ASSERT_EQ(twin.num_variables(), f.cs.num_variables());
        const auto twin_ctx = R1CSVerifierMatrices::FromR1CS(twin);
        ASSERT_NE(twin_ctx.circuit_hash(), m.circuit_hash());
        SpartanProof p; ASSERT_TRUE(SpartanProof::deserialize(f.prove(false, f.cs), p, f.ctx, false));
        Transcript t("spartan.profile.compat");
        EXPECT_FALSE(r1cs_spartan_verify(p, f.cs, f.cs.num_constraints(), f.cs.num_variables(), {}, Scalar::one(), f.gens(), t, f.ctx, true, true, false, 1, &twin_ctx));
        Transcript t2("spartan.profile.compat");
        EXPECT_FALSE(r1cs_spartan_verify(p, f.cs, f.cs.num_constraints(), f.cs.num_variables(), twin_ctx.circuit_hash(), Scalar::one(), f.gens(), t2, f.ctx, true, true, false, 1, &m)) << "expected hash must match the context";
    }
    // A CSR built for a different shape is refused, never silently used.
    R1CS other; { auto x = other.alloc(Scalar::one()); other.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x), "b"); }
    const auto wrong = R1CSVerifierMatrices::FromR1CS(other);
    SpartanProof p; ASSERT_TRUE(SpartanProof::deserialize(f.prove(false, f.cs), p, f.ctx, false));
    Transcript t("spartan.profile.compat");
    EXPECT_FALSE(r1cs_spartan_verify(p, f.cs, f.cs.num_constraints(), f.cs.num_variables(), f.hash, Scalar::one(), f.gens(), t, f.ctx, true, true, false, 1, &wrong));
}

// Review point 2026-09-22: the parallel matrix branch only runs for >= 16384 constraints, so the
// budget-independence claim must be tested on a fixture that size, for valid AND invalid proofs,
// both profiles, walk and CSR.
TEST(SpartanProfileCompat, LargeFixtureParallelBranchIsVerdictIdenticalOnValidAndInvalidProofs) {
    Fixture f;
    R1CS big;
    std::vector<Variable> xs;
    for (int i = 0; i < 20000; ++i) {
        auto x = big.alloc(Scalar::one());
        big.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x), "boolean");
        xs.push_back(x);
    }
    ASSERT_GE(big.num_constraints(), 16384u);
    ASSERT_TRUE(big.is_satisfied());
    const auto hash = spartan_hash_r1cs_structure(big);
    const auto csr = R1CSVerifierMatrices::FromR1CS(big);
    const auto& gens = GeneratorSet::cached(std::max<size_t>(4, std::max(HyraxParams::from_n(big.num_variables()).n_cols,
                                                                            HyraxParams::from_n(big.num_constraints()).n_cols)), f.ctx);
    for (bool omit : {false, true}) {
        Transcript tp("spartan.profile.compat.large");
        auto bytes = r1cs_spartan_prove(big, std::vector<Scalar>(big.num_constraints(), Scalar::zero()), Scalar::one(), gens, tp, f.ctx, true, omit).serialize(f.ctx);
        ASSERT_FALSE(bytes.empty());
        for (int variant = 0; variant < 3; ++variant) {
            auto in = bytes;
            if (variant == 1) in[in.size() / 2] ^= 0x01;          // corrupt a sum-check / opening scalar
            if (variant == 2) in[40] ^= 0x01;                      // corrupt a commitment point byte
            SpartanProof p;
            if (!SpartanProof::deserialize(in, p, f.ctx, omit)) { EXPECT_NE(variant, 0); continue; }
            bool verdict[4]; int k = 0;
            for (size_t threads : {size_t{1}, size_t{8}}) for (const R1CSVerifierMatrices* m : {static_cast<const R1CSVerifierMatrices*>(nullptr), &csr}) {
                Transcript tv("spartan.profile.compat.large");
                verdict[k++] = r1cs_spartan_verify(p, big, big.num_constraints(), big.num_variables(), hash, Scalar::one(), gens, tv, f.ctx, true, true, omit, threads, m);
            }
            for (int j = 1; j < 4; ++j) EXPECT_EQ(verdict[0], verdict[j]) << "omit=" << omit << " variant=" << variant << " config=" << j;
            EXPECT_EQ(verdict[0], variant == 0) << "omit=" << omit << " variant=" << variant;
        }
    }
    // Invalid witness on the large fixture: rejected identically by every configuration.
    R1CS bad;
    for (int i = 0; i < 20000; ++i) { auto x = bad.alloc(i == 777 ? Scalar::one() + Scalar::one() : Scalar::one()); bad.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x), "boolean"); }
    ASSERT_FALSE(bad.is_satisfied());
    Transcript tp("spartan.profile.compat.large");
    auto bytes = r1cs_spartan_prove(bad, std::vector<Scalar>(bad.num_constraints(), Scalar::zero()), Scalar::one(), gens, tp, f.ctx, true, false).serialize(f.ctx);
    SpartanProof p; ASSERT_TRUE(SpartanProof::deserialize(bytes, p, f.ctx, false));
    for (size_t threads : {size_t{1}, size_t{8}}) for (const R1CSVerifierMatrices* m : {static_cast<const R1CSVerifierMatrices*>(nullptr), &csr}) {
        Transcript tv("spartan.profile.compat.large");
        EXPECT_FALSE(r1cs_spartan_verify(p, big, big.num_constraints(), big.num_variables(), hash, Scalar::one(), gens, tv, f.ctx, true, true, false, threads, m)) << threads << (m != nullptr);
    }
}

// Mixed coefficients (2, 3, -5, 7, -1) on a >= 16384-constraint fixture, and an invalid case that
// can only fail at the matrix evaluation: the proof is for circuit A, the verifier evaluates the
// matrices of circuit B (same dimensions, one coefficient changed). Sum-checks, transcript and
// Hyrax openings never read the verifier's matrices, so the inner final check
// (inner_final == M~_B(rx,ry) * z~(ry)) is the only check that can reject. For the CSR path the
// identity binding would reject first, so the test forges B's context hash to A's to reach it.
static void MixedFixture(R1CS& cs, bool flip_one_coefficient) {
    const Scalar two = Scalar::one() + Scalar::one(), three = two + Scalar::one(), five = three + two, seven = five + two;
    for (int i = 0; i < 17000; ++i) {
        // x = 2, y = 3: (2x + 3y - 5) * (7x - y) = z  with z = (4+9-5)*(14-3) = 8*11 = 88
        auto x = cs.alloc(two), y = cs.alloc(three);
        Scalar z = Scalar(uint64_t{88});
        auto zv = cs.alloc(z);
        LinearCombination lhs = LinearCombination(two, x) + LinearCombination(three, y) - LinearCombination::constant(five);
        LinearCombination rhs = LinearCombination(seven, x) - LinearCombination(y);
        if (flip_one_coefficient && i == 4242) rhs = LinearCombination(seven, x) - LinearCombination(two, y);
        cs.constrain(lhs, rhs, LinearCombination(zv), "mixed");
    }
}

TEST(SpartanProfileCompat, MixedCoefficientLargeFixtureAndMatrixEvaluationRejection) {
    Fixture f;
    R1CS A, B;
    MixedFixture(A, false); MixedFixture(B, true);
    ASSERT_GE(A.num_constraints(), 16384u);
    ASSERT_TRUE(A.is_satisfied());
    ASSERT_EQ(A.num_constraints(), B.num_constraints()); ASSERT_EQ(A.num_variables(), B.num_variables());
    const auto hashA = spartan_hash_r1cs_structure(A), hashB = spartan_hash_r1cs_structure(B);
    ASSERT_NE(hashA, hashB);
    const auto ctxA = R1CSVerifierMatrices::FromR1CS(A);
    EXPECT_GT(ctxA.nnz_total() - ctxA.nnz_one() - ctxA.nnz_neg_one(), 0u) << "fixture has general coefficients";
    EXPECT_GT(ctxA.nnz_neg_one(), 0u);
    const auto& gens = GeneratorSet::cached(std::max<size_t>(4, std::max(HyraxParams::from_n(A.num_variables()).n_cols, HyraxParams::from_n(A.num_constraints()).n_cols)), f.ctx);
    for (bool omit : {false, true}) {
        Transcript tp("spartan.profile.compat.mixed");
        auto bytes = r1cs_spartan_prove(A, std::vector<Scalar>(A.num_constraints(), Scalar::zero()), Scalar::one(), gens, tp, f.ctx, true, omit).serialize(f.ctx);
        SpartanProof p; ASSERT_TRUE(SpartanProof::deserialize(bytes, p, f.ctx, omit));
        // Valid: walk and CSR, 1 and 8 threads, agree and accept.
        for (size_t threads : {size_t{1}, size_t{8}}) {
            Transcript t1("spartan.profile.compat.mixed"), t2("spartan.profile.compat.mixed");
            EXPECT_TRUE(r1cs_spartan_verify(p, A, A.num_constraints(), A.num_variables(), hashA, Scalar::one(), gens, t1, f.ctx, true, true, omit, threads, nullptr)) << omit << threads;
            EXPECT_TRUE(r1cs_spartan_verify(p, A, A.num_constraints(), A.num_variables(), hashA, Scalar::one(), gens, t2, f.ctx, true, true, omit, threads, &ctxA)) << omit << threads;
        }
        // Invalid at the matrix evaluation only (walk path, expected hash left empty so the
        // fast-reject is skipped and the M~ check is reached; B's witness slots equal A's).
        for (size_t threads : {size_t{1}, size_t{8}}) {
            Transcript t("spartan.profile.compat.mixed");
            EXPECT_FALSE(r1cs_spartan_verify(p, B, B.num_constraints(), B.num_variables(), {}, Scalar::one(), gens, t, f.ctx, true, true, omit, threads, nullptr)) << "walk " << omit << threads;
        }
        // Same for the CSR path: forge B's identity to A's so binding passes and M~_B rejects.
        auto forged = R1CSVerifierMatrices::Build(B);
        R1CSVerifierMatricesTestAccess::ForgeIdentity(forged, hashA);
        for (size_t threads : {size_t{1}, size_t{8}}) {
            Transcript t("spartan.profile.compat.mixed");
            EXPECT_FALSE(r1cs_spartan_verify(p, A, A.num_constraints(), A.num_variables(), hashA, Scalar::one(), gens, t, f.ctx, true, true, omit, threads, &forged)) << "csr " << omit << threads;
        }
    }
}
