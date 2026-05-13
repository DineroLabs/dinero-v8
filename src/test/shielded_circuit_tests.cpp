// Copyright (c) 2026 Dinero Labs.
//
// Phase 0 wave 2 — prover/verifier round-trip tests for the shielded
// pool's ZK circuits. Constructs self-consistent witnesses using the
// canonical `NoteCommitment` and `ComputeNullifier` helpers, runs
// `ProveSpend`/`VerifySpend` and `ProveOutput`/`VerifyOutput` end to
// end, and verifies that tampered public inputs are rejected.
//
// These tests are slow (Spartan proving) — measured single-digit
// seconds per case on Mac M-series. Foundation tests in wave 1 cover
// the fast contracts; this suite is the slow regression net.

#include <gtest/gtest.h>

#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_circuit.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::CommitmentTree;
using shielded::Hash;
using shielded::NoteCommitment;
using shielded::ComputeNullifier;
using shielded::PoseidonHash2;
using shielded::OutputPublicInputs;
using shielded::OutputWitness;
using shielded::ProveOutput;
using shielded::ProveSpend;
using shielded::SpendPublicInputs;
using shielded::SpendWitness;
using shielded::VerifyOutput;
using shielded::VerifySpend;
using shielded::TREE_DEPTH;

Hash MakeHash(uint8_t seed, uint8_t tail = 0xCD) {
    Hash h{};
    h[0]  = seed;
    h[31] = tail;
    return h;
}

// Phase 2 wave 4: value witnesses must satisfy the 64-bit range check
// inside the spend/output circuits. Encodes a uint64 into the low 8
// bytes of a Hash matching the Scalar big-endian layout (h[24..31]).
Hash ValueAsHash(uint64_t v) {
    Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

// ── Output proof round-trips ────────────────────────────────────────

TEST(ShieldedOutputCircuitTest, ValidOutputProofVerifies) {
    OutputWitness w;
    w.value      = ValueAsHash(100'000'000);  // 1 DIN — fits in 64 bits
    w.public_key = MakeHash(0x02, 0x10);
    w.randomness = MakeHash(0x03, 0x10);

    OutputPublicInputs pub;
    pub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);

    auto proof = ProveOutput(w, pub, nullptr);
    ASSERT_FALSE(proof.empty());
    EXPECT_TRUE(VerifyOutput(proof, pub, nullptr));
}

TEST(ShieldedOutputCircuitTest, DifferentDiversifierProducesDifferentCommitment) {
    // Phase 2 wave 5: same value/pk_d/randomness with two different
    // diversifiers must yield distinct commitments. Otherwise the
    // address-binding tag is degenerate.
    Hash pk        = MakeHash(0x12, 0x10);
    Hash value     = ValueAsHash(42);
    Hash randomness = MakeHash(0x13, 0x10);

    Hash d_a{};                    // diversifier 0
    Hash d_b{};
    d_b[31] = 0x01;                // diversifier 1

    EXPECT_NE(NoteCommitment(d_a, pk, value, randomness),
              NoteCommitment(d_b, pk, value, randomness));
}

TEST(ShieldedOutputCircuitTest, OutOfRangeValueRejected) {
    // Value scalar with high bytes non-zero → exceeds 2^64. Range check
    // inside the output circuit must refuse to satisfy.
    OutputWitness w;
    w.value      = MakeHash(0xFF, 0xFF);  // ~2^248-sized scalar
    w.public_key = MakeHash(0x02, 0x10);
    w.randomness = MakeHash(0x03, 0x10);

    OutputPublicInputs pub;
    pub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);

    // ProveOutput returns empty bytes when the witness fails to satisfy
    // the constraint system — this is the in-circuit range-check signal.
    EXPECT_TRUE(ProveOutput(w, pub, nullptr).empty());
}

TEST(ShieldedOutputCircuitTest, TamperedCommitmentRejected) {
    OutputWitness w;
    w.value      = ValueAsHash(50'000'000);
    w.public_key = MakeHash(0x12, 0x20);
    w.randomness = MakeHash(0x13, 0x20);

    OutputPublicInputs pub;
    pub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);

    auto proof = ProveOutput(w, pub, nullptr);
    ASSERT_FALSE(proof.empty());

    // Flip one byte of the public commitment — verifier must reject.
    OutputPublicInputs tampered = pub;
    tampered.commitment[5] ^= 0x01;
    EXPECT_FALSE(VerifyOutput(proof, tampered, nullptr));
}

TEST(ShieldedOutputCircuitTest, EmptyProofRejected) {
    OutputPublicInputs pub;
    pub.commitment = MakeHash(0x99);
    EXPECT_FALSE(VerifyOutput({}, pub, nullptr));
}

TEST(ShieldedOutputCircuitTest, GarbageProofRejected) {
    OutputPublicInputs pub;
    pub.commitment = MakeHash(0x42);
    std::vector<uint8_t> garbage(128, 0xAB);
    EXPECT_FALSE(VerifyOutput(garbage, pub, nullptr));
}

// ── Spend proof round-trips ─────────────────────────────────────────

// Build a self-consistent SpendWitness/SpendPublicInputs pair: the
// note's commitment is appended to a CommitmentTree, the auth path
// is harvested, the nullifier is computed canonically, and the
// anchor is set to the tree's root at the time of insertion.
struct SpendFixture {
    Hash sk         = MakeHash(0xA1, 0xF0);
    Hash value      = ValueAsHash(123'456'789);  // ~1.23 DIN, fits in 64 bits
    Hash randomness = MakeHash(0xA4, 0xF0);

    CommitmentTree tree;
    SpendWitness witness{};
    SpendPublicInputs pub{};

    void Build() {
        // Spend circuit derives pk internally as Poseidon(sk, 0); the
        // witness must agree, so the commitment we insert into the tree
        // uses the same derived pk — not a random one.
        const Hash pk = PoseidonHash2(sk, Hash{});
        // Phase 2 wave 5: diversifier — zeros for tests not exercising
        // address-binding semantics.
        const Hash d{};

        // Append a couple of decoy commitments so our note isn't at
        // index 0 — exercises non-trivial Merkle paths (direction bits).
        tree.Append(MakeHash(0x10));
        tree.Append(MakeHash(0x11));
        const Hash cm = NoteCommitment(d, pk, value, randomness);
        const uint64_t idx = tree.Append(cm);
        ASSERT_GT(idx, 0u);

        const auto path = tree.GetAuthPath(idx);
        ASSERT_TRUE(path.has_value());

        witness.secret_key = sk;
        witness.leaf_index = idx;
        witness.value      = value;
        witness.randomness = randomness;
        witness.d          = d;
        witness.merkle_path = path->siblings;

        pub.nullifier = ComputeNullifier(sk, idx);
        pub.anchor    = tree.Root();
    }
};

TEST(ShieldedSpendCircuitTest, ValidSpendProofVerifies) {
    SpendFixture fx;
    fx.Build();

    auto proof = ProveSpend(fx.witness, fx.pub, nullptr);
    ASSERT_FALSE(proof.empty());
    EXPECT_TRUE(VerifySpend(proof, fx.pub, nullptr));
}

TEST(ShieldedSpendCircuitTest, TamperedNullifierRejected) {
    SpendFixture fx;
    fx.Build();

    auto proof = ProveSpend(fx.witness, fx.pub, nullptr);
    ASSERT_FALSE(proof.empty());

    SpendPublicInputs tampered = fx.pub;
    tampered.nullifier[3] ^= 0x80;
    EXPECT_FALSE(VerifySpend(proof, tampered, nullptr));
}

TEST(ShieldedSpendCircuitTest, TamperedAnchorRejected) {
    SpendFixture fx;
    fx.Build();

    auto proof = ProveSpend(fx.witness, fx.pub, nullptr);
    ASSERT_FALSE(proof.empty());

    SpendPublicInputs tampered = fx.pub;
    tampered.anchor[10] ^= 0x40;
    EXPECT_FALSE(VerifySpend(proof, tampered, nullptr));
}

TEST(ShieldedSpendCircuitTest, EmptyProofRejected) {
    SpendFixture fx;
    fx.Build();
    EXPECT_FALSE(VerifySpend({}, fx.pub, nullptr));
}

TEST(ShieldedSpendCircuitTest, GarbageProofRejected) {
    SpendFixture fx;
    fx.Build();
    std::vector<uint8_t> garbage(256, 0xCC);
    EXPECT_FALSE(VerifySpend(garbage, fx.pub, nullptr));
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
