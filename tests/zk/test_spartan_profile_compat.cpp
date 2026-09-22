// Differential regression for the research proof profile (claude/shielded-v2 Task 7):
// the legacy Spartan profile keeps its historical accepted input language and resource
// behaviour; strictness applies to the E-less profile only. Review finding 2026-09-22
// (MemoryMD/evidence/shielded-v2-task7-review-2026-09-22).
#include "zk/zkvm/r1cs_spartan.h"
#include "zk/zkvm/transcript.h"
#include <gtest/gtest.h>
#include <secp256k1.h>
#include <algorithm>

using namespace dinero::zk::zkvm;

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
