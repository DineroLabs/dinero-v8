// Native codec tests use genuine proofs and the unchanged Spartan verifier.
#include "consensus/shielded/compact_spartan_codec.h"
#include "zk/zkvm/r1cs_spartan.h"
#include <algorithm>
#include <array>
#include <bit>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>

using namespace dinero::zk::zkvm;
using dinero::consensus::shielded::CompactSpartanCodec;
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

// Independently enumerate every retained point/scalar and count field. Offsets
// are in the tagged compact form; the full form has the omitted E rows instead.
struct CompactFields {
    std::vector<size_t> scalars, points;
    std::vector<std::pair<size_t, size_t>> counts;
    size_t error_offset, omitted, size;
    explicit CompactFields(const R1CS &cs) {
        const auto w = HyraxParams::from_n(cs.num_variables());
        const auto e = HyraxParams::from_n(cs.num_constraints());
        size_t at = 5;
        auto count = [&](size_t width) {
            counts.emplace_back(at, width);
            at += width;
        };
        auto scalar = [&] {
            scalars.push_back(at);
            at += 32;
        };
        auto point = [&] {
            points.push_back(at);
            at += 33;
        };
        for (int i = 0; i < 3; ++i)
            count(8);
        for (size_t i = 0; i < w.n_rows; ++i)
            point();
        for (int i = 0; i < 3; ++i)
            count(8);
        error_offset = at;
        omitted = 33 * e.n_rows;
        at += 32; // Circuit hash is a digest, not a scalar.
        count(8);
        for (size_t i = 0; i < 4 * std::bit_width(cs.num_constraints() - 1) + 4; ++i)
            scalar();
        count(8);
        for (size_t i = 0; i < 3 * std::bit_width(cs.num_variables() - 1); ++i)
            scalar();
        for (size_t cols : {w.n_cols, e.n_cols}) {
            scalar();
            count(4);
            for (size_t i = 0; i < 2 * std::bit_width(cols - 1); ++i)
                point();
            scalar();
            scalar();
        }
        size = at;
    }
    size_t Full(size_t compact) const {
        return compact - 4 + (compact >= error_offset ? omitted : 0);
    }
};

TEST_F(CompactSpartan, EveryScalarRejectsNoncanonicalRepresentatives) {
    const auto original = Honest();
    CompactSpartanCodec codec(6, cs);
    const auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    const CompactFields fields(cs);
    ASSERT_EQ(fields.size, packed->size());
    // secp256k1 group order, independently specified in big-endian wire order.
    const std::array<uint8_t, 32> order = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                           0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
                                           0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
                                           0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};
    auto order_plus_one = order;
    ++order_plus_one.back();
    auto maximum = order;
    maximum.fill(0xff);
    auto order_minus_one = order;
    --order_minus_one.back();
    std::array<uint8_t, 32> zero{};
    for (size_t at : fields.scalars) {
        for (const auto &invalid : {order, order_plus_one, maximum}) {
            auto c = *packed, f = original;
            std::copy(invalid.begin(), invalid.end(), c.begin() + at);
            std::copy(invalid.begin(), invalid.end(), f.begin() + fields.Full(at));
            EXPECT_FALSE(codec.Expand(c)) << at;
            EXPECT_FALSE(codec.Pack(f)) << at;
        }
        // Canonical boundary values must stay byte-exact. Acceptance here does
        // not imply proof validity: the unchanged verifier is still mandatory.
        for (const auto &valid : {zero, order_minus_one}) {
            auto c = *packed, f = original;
            std::copy(valid.begin(), valid.end(), c.begin() + at);
            std::copy(valid.begin(), valid.end(), f.begin() + fields.Full(at));
            EXPECT_EQ(codec.Expand(c), std::optional(f)) << at;
            EXPECT_EQ(codec.Pack(f), std::optional(c)) << at;
        }
    }
}

TEST_F(CompactSpartan, EveryPointRequiresCanonicalIdentityOrCompressedCurvePoint) {
    const auto original = Honest();
    CompactSpartanCodec codec(6, cs);
    const auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    const CompactFields fields(cs);
    std::array<uint8_t, 33> generator{};
    ASSERT_TRUE(Point::generator(ctx.get()).serialize(generator, ctx.get()));
    std::vector<std::array<uint8_t, 33>> invalid;
    for (uint8_t prefix : {0, 1, 4, 6, 7, 255}) {
        auto p = generator;
        p[0] = prefix;
        invalid.push_back(p);
    }
    // x=0 has no secp256k1 curve point; x=2^256-1 is outside the field.
    for (uint8_t prefix : {2, 3}) {
        std::array<uint8_t, 33> p{};
        p[0] = prefix;
        invalid.push_back(p);
        p.fill(0xff);
        p[0] = prefix;
        invalid.push_back(p);
    }
    for (const auto &p : invalid) {
        Point decoded;
        ASSERT_FALSE(Point::parse(p.data(), p.size(), decoded, ctx.get()));
    }
    std::array<uint8_t, 33> identity{};
    for (size_t at : fields.points) {
        for (const auto &bad : invalid) {
            auto c = *packed, f = original;
            std::copy(bad.begin(), bad.end(), c.begin() + at);
            std::copy(bad.begin(), bad.end(), f.begin() + fields.Full(at));
            EXPECT_FALSE(codec.Expand(c)) << at;
            EXPECT_FALSE(codec.Pack(f)) << at;
        }
        for (const auto &valid : {identity, generator}) {
            auto c = *packed, f = original;
            std::copy(valid.begin(), valid.end(), c.begin() + at);
            std::copy(valid.begin(), valid.end(), f.begin() + fields.Full(at));
            EXPECT_EQ(codec.Expand(c), std::optional(f)) << at;
            EXPECT_EQ(codec.Pack(f), std::optional(c)) << at;
        }
    }
}

TEST_F(CompactSpartan, EveryCountRejectsBoundaryAndOversizedWireValues) {
    const auto original = Honest();
    CompactSpartanCodec codec(6, cs);
    const auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    const CompactFields fields(cs);
    for (const auto &[at, width] : fields.counts) {
        uint64_t expected = 0;
        for (size_t i = 0; i < width; ++i)
            expected = (expected << 8) | (*packed)[at + i];
        for (uint64_t value :
             {uint64_t(0), uint64_t(1), expected + 1, uint64_t(UINT32_MAX), uint64_t(UINT64_MAX)}) {
            if (value == expected)
                continue;
            auto c = *packed, f = original;
            for (size_t i = 0; i < width; ++i) {
                c[at + width - 1 - i] = (value >> (8 * i)) & 0xff;
                f[fields.Full(at) + width - 1 - i] = (value >> (8 * i)) & 0xff;
            }
            EXPECT_FALSE(codec.Expand(c)) << at << ':' << value;
            EXPECT_FALSE(codec.Pack(f)) << at << ':' << value;
        }
    }
    EXPECT_FALSE(codec.Expand(std::vector<uint8_t>(1 << 20, 0xff)));
    EXPECT_FALSE(codec.Pack(std::vector<uint8_t>(1 << 20, 0)));
}

TEST_F(CompactSpartan, RejectsSameShapeDifferentCircuitAndCrossProfile) {
    const auto original = Honest();
    CompactSpartanCodec spend(6, cs), output(4, cs);
    const auto packed = spend.Pack(original);
    ASSERT_TRUE(packed);
    EXPECT_FALSE(output.Expand(*packed));
    auto original_output = original;
    original_output[0] = 4;
    const auto packed_output = output.Pack(original_output);
    ASSERT_TRUE(packed_output);
    EXPECT_EQ(output.Expand(*packed_output), std::optional(original_output));
    EXPECT_FALSE(spend.Expand(*packed_output));
    R1CS different;
    auto x = different.alloc(Scalar(1));
    for (int i = 0; i < 5; ++i)
        different.constrain(LinearCombination(x), LinearCombination(x),
                            LinearCombination::constant(Scalar(1)));
    ASSERT_EQ(different.num_variables(), cs.num_variables());
    ASSERT_EQ(different.num_constraints(), cs.num_constraints());
    ASSERT_NE(spartan_hash_r1cs_structure(different), spartan_hash_r1cs_structure(cs));
    EXPECT_FALSE(CompactSpartanCodec(6, different).Expand(*packed));
}

TEST_F(CompactSpartan, DeterministicMalformedInputsStayBoundedAndByteExact) {
    std::mt19937 rng(0xD2E10001);
    const auto original = Honest();
    CompactSpartanCodec codec(6, cs);
    const auto packed = codec.Pack(original);
    ASSERT_TRUE(packed);
    size_t accepted = 0, rejected = 0;
    for (size_t i = 0; i < 4000; ++i) {
        auto bytes = i % 2 ? *packed : original;
        switch (i % 4) {
        case 0:
        case 1:
            for (size_t n = 0, count = 1 + rng() % 8; n < count; ++n)
                bytes[rng() % bytes.size()] ^= 1 + rng() % 255;
            break;
        case 2:
            bytes.resize(rng() % bytes.size());
            break;
        case 3:
            bytes.push_back(rng() & 0xff);
            break;
        }
        const auto before = bytes;
        const auto result = i % 2 ? codec.Expand(bytes) : codec.Pack(bytes);
        EXPECT_EQ(bytes, before); // Never rewrite the transaction's backing bytes.
        if (result) {
            ++accepted;
            EXPECT_EQ(i % 2 ? codec.Pack(*result) : codec.Expand(*result), std::optional(bytes));
        } else
            ++rejected;
    }
    EXPECT_GT(accepted, 0u);
    EXPECT_GT(rejected, 0u);
}
} // namespace
