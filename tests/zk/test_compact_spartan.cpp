// Native codec tests use genuine proofs and the unchanged Spartan verifier.
#include "../../contrib/benchmarks/compact_spartan_codec.h"
#include "zk/zkvm/r1cs_spartan.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace dinero::zk::zkvm;
using dinero::experimental::CompactSpartanCodec;
namespace {
class CompactSpartan : public ::testing::Test {
  protected:
    std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> ctx{
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY),
        secp256k1_context_destroy};
    R1CS cs;
    void SetUp() override {
        auto x = cs.alloc(Scalar(1));
        for (int i = 0; i < 5; ++i)
            cs.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x));
    }
    std::vector<uint8_t> Proof(const R1CS &circuit, const std::vector<Scalar> &e) {
        const auto &gens = GeneratorSet::cached(8, ctx.get());
        Transcript tp("compact.prototype.test");
        auto proof = r1cs_spartan_prove(circuit, e, Scalar::one(), gens, tp, ctx.get());
        auto bytes = proof.serialize(ctx.get());
        bytes.insert(bytes.begin(), 6);
        return bytes;
    }
    std::vector<uint8_t> Honest() {
        return Proof(cs, std::vector<Scalar>(cs.num_constraints(), Scalar::zero()));
    }
    bool Verify(const std::vector<uint8_t> &bytes) {
        SpartanProof proof;
        if (bytes.empty() || bytes[0] != 6 ||
            !SpartanProof::deserialize({bytes.begin() + 1, bytes.end()}, proof, ctx.get()))
            return false;
        Transcript tv("compact.prototype.test");
        return r1cs_spartan_verify(proof, cs, cs.num_constraints(), cs.num_variables(),
                                   spartan_hash_r1cs_structure(cs), Scalar::one(),
                                   GeneratorSet::cached(8, ctx.get()), tv, ctx.get());
    }
};

TEST_F(CompactSpartan, ExactBytesCanonicalRepackAndOriginalVerifier) {
    const auto original = Honest();
    CompactSpartanCodec codec(6, cs);
    auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    EXPECT_EQ(packed->size(),
              original.size() - 33 * HyraxParams::from_n(cs.num_constraints()).n_rows + 4);
    auto restored = codec.Expand(*packed);
    ASSERT_TRUE(restored);
    EXPECT_EQ(*restored, original);
    EXPECT_EQ(codec.Pack(*restored), packed);
    EXPECT_TRUE(Verify(*restored));
}

TEST_F(CompactSpartan, NonzeroErrorCannotBeDiscarded) {
    const auto original = Honest();
    auto changed = original;
    const auto offset = 1 + 24 + 33 * HyraxParams::from_n(cs.num_variables()).n_rows + 24;
    changed[offset] = 2;
    EXPECT_FALSE(CompactSpartanCodec(6, cs).Pack(changed));
}

TEST_F(CompactSpartan, RejectsEveryTruncatedLengthAndTrailingBytes) {
    CompactSpartanCodec codec(6, cs);
    const auto original = Honest();
    auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    for (size_t n = 0; n < original.size(); ++n)
        EXPECT_FALSE(codec.Pack(std::span(original).first(n))) << n;
    for (size_t n = 0; n < packed->size(); ++n)
        EXPECT_FALSE(codec.Expand(std::span(*packed).first(n))) << n;
    auto extra = *packed;
    extra.push_back(0);
    EXPECT_FALSE(codec.Expand(extra));
    extra = original;
    extra.push_back(0);
    EXPECT_FALSE(codec.Pack(extra));
}

TEST_F(CompactSpartan, CircuitDimensionsAndRoundCountsAreNotWireControlled) {
    CompactSpartanCodec codec(6, cs);
    const auto original = Honest();
    auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    const auto hw = HyraxParams::from_n(cs.num_variables());
    const size_t eheader = 4 + 1 + 24 + 33 * hw.n_rows;
    const size_t outer = eheader + 24 + 32;
    const size_t inner = outer + 8 + 3 * 128 + 128;
    const size_t evalw = inner + 8 + 1 * 96;
    const size_t evale = evalw + 32 + 4 + 1 * 66 + 64;
    for (const size_t offset : {size_t(5), size_t(13), size_t(21), eheader, eheader + 8,
                                eheader + 16, outer, inner, evalw + 32, evale + 32}) {
        auto bad = *packed;
        bad[offset] = 255;
        EXPECT_FALSE(codec.Expand(bad)) << offset;
    }
    auto wrong_hash = *packed;
    wrong_hash[eheader + 24] ^= 1;
    EXPECT_FALSE(codec.Expand(wrong_hash));
}

TEST_F(CompactSpartan, RejectsUnassignedTagsProfilesAndWrongCircuit) {
    const auto original = Honest();
    CompactSpartanCodec codec(6, cs);
    auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    for (size_t i = 0; i < 5; ++i) {
        auto bad = *packed;
        bad[i] ^= 0xff;
        EXPECT_FALSE(codec.Expand(bad));
    }
    EXPECT_FALSE(codec.Expand(original));
    EXPECT_FALSE(codec.Pack(*packed));
    for (uint8_t v : {0, 1, 3, 5, 7, 255})
        EXPECT_THROW(CompactSpartanCodec(v, cs), std::invalid_argument);
    R1CS different;
    auto y = different.alloc(Scalar(1));
    different.constrain(LinearCombination(y), LinearCombination(y), LinearCombination(y));
    EXPECT_FALSE(CompactSpartanCodec(6, different).Expand(*packed));
}

TEST_F(CompactSpartan, PreservesTamperingForTheVerifierToReject) {
    auto original = Honest();
    const auto h = HyraxParams::from_n(cs.num_variables());
    const auto e = HyraxParams::from_n(cs.num_constraints());
    // Az claim: changing a scalar retains structural validity but breaks the proof.
    const size_t az = 1 + 24 + 33 * h.n_rows + 24 + 33 * e.n_rows + 32 + 8 + 3 * 128;
    original[az + 31] ^= 1;
    CompactSpartanCodec codec(6, cs);
    auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    auto restored = codec.Expand(*packed);
    ASSERT_TRUE(restored);
    EXPECT_EQ(*restored, original);
    EXPECT_FALSE(Verify(*restored));
}

TEST_F(CompactSpartan, ForgedRelaxedProofCannotCrossTheCodec) {
    R1CS bad;
    auto x = bad.alloc(Scalar(5));
    for (int i = 0; i < 5; ++i)
        bad.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x));
    auto forged = Proof(bad, std::vector<Scalar>(5, Scalar(20)));
    EXPECT_FALSE(CompactSpartanCodec(6, bad).Pack(forged));
}

TEST_F(CompactSpartan, RejectsEmptyTrustedCircuit) {
    R1CS empty;
    EXPECT_THROW(CompactSpartanCodec(6, empty), std::invalid_argument);
}

TEST_F(CompactSpartan, AcceptedByteMutationsHaveExactlyOneRepresentation) {
    CompactSpartanCodec codec(6, cs);
    auto packed = codec.Pack(Honest());
    ASSERT_TRUE(packed);
    // Byte mutations are either rejected or preserved exactly; this codec
    // must not normalize attacker-controlled fields into a different proof.
    for (size_t i = 0; i < packed->size(); ++i) {
        auto changed = *packed;
        changed[i] ^= 0xff;
        auto expanded = codec.Expand(changed);
        if (expanded)
            EXPECT_EQ(codec.Pack(*expanded), std::optional(changed)) << i;
    }
}

TEST_F(CompactSpartan, HandlesCircuitDerivedZeroRoundSections) {
    R1CS small;
    auto x = small.alloc(Scalar(1));
    small.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x));
    const auto proof = Proof(small, {Scalar::zero()});
    CompactSpartanCodec codec(6, small);
    auto packed = codec.Pack(proof);
    ASSERT_TRUE(packed);
    EXPECT_EQ(codec.Expand(*packed), std::optional(proof));
}
} // namespace
