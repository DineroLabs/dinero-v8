// Copyright (c) 2026 Dinero Labs.
//
// Audit Critical #1 — shielded cv-binding regression suite.
//
// The shielded Spend/Output circuits prove the Poseidon note commitment and
// range-check `val` to 64 bits, but historically NEVER constrained the
// Pedersen value commitment `cv` to equal the note's value. An attacker could
// publish cv = commit(0) (range proof + binding sig pass, value_balance == 0)
// while the note encodes a huge value, then later unshield real coins —
// mint-from-nothing inflation.
//
// The fix adds, in BOTH circuits, the constraint cv == val·V + rcv·G using the
// SAME `val` Variable that feeds the range check and note commitment, gated by
// the `shielded_cv_binding_activation_height` consensus param (distinct proof
// version bytes 0x03/0x04).
//
// These tests prove the KNOWN exploit is CLOSED post-activation and that the
// honest cv-bound path verifies. A passing test does NOT prove soundness — it
// proves the known attack is closed and the construction is satisfiable.
//
// NOTE: cv-bound proofs add ~430k EC constraints, so the honest prove/verify
// cases are slow (tens of seconds each). The exploit cases are cheap (no full
// Spartan proving — the unsatisfiable circuit is rejected before proving).

#include <gtest/gtest.h>

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/shielded_validation.h"

#include <array>
#include <cstdint>
#include <vector>
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::CommitmentTree;
using shielded::ComputeNullifier;
using shielded::Hash;
using shielded::NoteCommitment;
using shielded::OutputPublicInputs;
using shielded::OutputWitness;
using shielded::PoseidonHash2;
using shielded::ProveOutput;
using shielded::ProveSpend;
using shielded::SpendPublicInputs;
using shielded::SpendWitness;
using shielded::ValueCommitment;
using shielded::VerifyOutput;
using shielded::VerifySpend;

Hash MakeHash(uint8_t seed, uint8_t tail = 0xCD) {
    Hash h{};
    h[0] = seed;
    h[31] = tail;
    return h;
}

// Encode a uint64 into the low 8 bytes of a Hash, matching the Scalar
// big-endian layout (h[24..31]) so HashToScalar(h) == v.
Hash ValueAsHash(uint64_t v) {
    Hash h{};
    for (int i = 0; i < 8; ++i) h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    return h;
}

// A small, valid Pedersen blind (well below the curve order n).
Hash MakeBlind(uint8_t seed) {
    Hash h{};
    h[31] = seed;
    h[30] = 0x11;
    h[20] = 0x22;
    return h;
}

// Compute the bundle cv = blind·G + value·V (libsecp Pedersen commitment).
ValueCommitment Commit(const Hash& blind, uint64_t value) {
    ValueCommitment cv{};
    EXPECT_EQ(PedersenCommit(blind, value, cv), PedersenResult::Ok);
    return cv;
}

// ─────────────────────────────────────────────────────────────────────────
// 1. Honest cv-bound OUTPUT proof verifies (validates the construction is
//    satisfiable AND the in-circuit cv == native-reconstructed cv, i.e. the
//    generator/coordinate/parity reconstruction is byte-exact).
// ─────────────────────────────────────────────────────────────────────────
TEST(ShieldedCvBinding, HonestCvBoundOutputVerifies) {
    ASSERT_TRUE(PedersenGeneratorsReady());
    const uint64_t value = 100'000'000;  // 1 DIN
    OutputWitness w{};
    w.value = ValueAsHash(value);
    w.public_key = MakeHash(0x02, 0x10);
    w.randomness = MakeHash(0x03, 0x10);
    w.rcv = MakeBlind(0x07);

    OutputPublicInputs pub{};
    pub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);
    pub.cv = Commit(w.rcv, value);  // cv = rcv·G + value·V

    auto proof = ProveOutput(w, pub, nullptr, /*bind_public_inputs=*/true,
                             /*cv_bound=*/true);
    ASSERT_FALSE(proof.empty()) << "honest cv-bound output proof must be producible";
    EXPECT_EQ(proof[0], 0x04) << "cv-bound output proof carries version 0x04";
    EXPECT_TRUE(VerifyOutput(proof, pub, nullptr, true, /*cv_bound=*/true));
    // Cross-check: it must NOT verify as a legacy proof (version mismatch).
    EXPECT_FALSE(VerifyOutput(proof, pub, nullptr, true, /*cv_bound=*/false));
}

// ─────────────────────────────────────────────────────────────────────────
// 2. The exploit: cv commits to 0 while the note encodes 1,000,000.
//    (a) The cv-bound circuit is UNSATISFIABLE — the attacker cannot even
//        produce a cv-bound proof (ProveOutput returns empty).
//    (b) NEUTER CHECK: without the cv constraint (the legacy circuit), the
//        same note/cv combination is ACCEPTED — demonstrating the open hole.
//        If the cv-binding constraint were removed, (a) would produce a valid
//        proof and this test would fail.
// ─────────────────────────────────────────────────────────────────────────
TEST(ShieldedCvBinding, CvCommitsToZeroButNoteHugeIsRejected) {
    ASSERT_TRUE(PedersenGeneratorsReady());
    const uint64_t note_value = 1'000'000;
    OutputWitness w{};
    w.value = ValueAsHash(note_value);  // note encodes 1,000,000
    w.public_key = MakeHash(0x05, 0x10);
    w.randomness = MakeHash(0x06, 0x10);
    w.rcv = MakeBlind(0x09);

    OutputPublicInputs pub{};
    pub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);
    pub.cv = Commit(w.rcv, 0);  // cv commits to 0 — the lie

    // (a) cv-bound prover refuses: cv_circ = 1e6·V + rcv·G != rcv·G = cv.
    auto forged = ProveOutput(w, pub, nullptr, true, /*cv_bound=*/true);
    EXPECT_TRUE(forged.empty())
        << "cv-bound circuit must be UNSATISFIABLE for cv=0 / note=1e6";

    // (b) Neuter check: the legacy circuit ignores cv and accepts the note.
    auto legacy = ProveOutput(w, pub, nullptr, true, /*cv_bound=*/false);
    ASSERT_FALSE(legacy.empty());
    EXPECT_TRUE(VerifyOutput(legacy, pub, nullptr, true, /*cv_bound=*/false))
        << "without the cv constraint, mint-from-nothing succeeds (the hole)";
}

// ─────────────────────────────────────────────────────────────────────────
// 3. Honest cv-bound SPEND proof verifies (validates the spend circuit's
//    cv binding, which shares the same `val` that feeds the Merkle/nullifier
//    machinery).
// ─────────────────────────────────────────────────────────────────────────
TEST(ShieldedCvBinding, HonestCvBoundSpendVerifies) {
    ASSERT_TRUE(PedersenGeneratorsReady());
    const uint64_t value = 123'456'789;
    const Hash sk = MakeHash(0xA1, 0xF0);
    const Hash randomness = MakeHash(0xA4, 0xF0);
    const Hash pk = PoseidonHash2(sk, Hash{});
    const Hash d{};

    CommitmentTree tree;
    tree.Append(MakeHash(0x10));
    tree.Append(MakeHash(0x11));
    const Hash cm = NoteCommitment(d, pk, ValueAsHash(value), randomness);
    const uint64_t idx = tree.Append(cm);
    const auto path = tree.GetAuthPath(idx);
    ASSERT_TRUE(path.has_value());

    SpendWitness w{};
    w.secret_key = sk;
    w.leaf_index = idx;
    w.value = ValueAsHash(value);
    w.randomness = randomness;
    w.d = d;
    w.rcv = MakeBlind(0x0B);
    w.merkle_path = path->siblings;

    SpendPublicInputs pub{};
    pub.nullifier = ComputeNullifier(sk, idx);
    pub.anchor = tree.Root();
    pub.cv = Commit(w.rcv, value);

    auto proof = ProveSpend(w, pub, nullptr, true, /*cv_bound=*/true);
    ASSERT_FALSE(proof.empty());
    EXPECT_EQ(proof[0], 0x03) << "cv-bound spend proof carries version 0x03";
    EXPECT_TRUE(VerifySpend(proof, pub, nullptr, true, /*cv_bound=*/true));

    // A cv-bound spend whose cv commits to a DIFFERENT value must be rejected.
    SpendPublicInputs wrong = pub;
    wrong.cv = Commit(w.rcv, value + 1);
    EXPECT_FALSE(VerifySpend(proof, wrong, nullptr, true, /*cv_bound=*/true));
}

// ─────────────────────────────────────────────────────────────────────────
// 4. Consensus-level exploit closure + migration boundary.
//    A legacy (non-cv-bound) bundle where the note encodes 1,000,000 but cv
//    commits to 0 and value_balance == 0 (mint-from-nothing):
//      - pre-activation  → ValidateShieldedBundle == Ok      (hole open)
//      - post-activation → ValidateShieldedBundle == ProofInvalid (hole closed)
//    Uses only the cheap legacy proof; post-activation rejection is the
//    version-byte gate (legacy 0x02 presented where 0x04 is required).
// ─────────────────────────────────────────────────────────────────────────
TEST(ShieldedCvBinding, ConsensusRejectsLegacyMintPostActivation) {
    ASSERT_TRUE(PedersenGeneratorsReady());
    const uint64_t note_value = 1'000'000;
    const Hash tx_sighash = MakeHash(0xEE, 0x77);

    OutputWitness w{};
    w.value = ValueAsHash(note_value);  // note encodes 1,000,000
    w.public_key = MakeHash(0x21, 0x10);
    w.randomness = MakeHash(0x22, 0x10);
    w.rcv = MakeBlind(0x33);

    OutputPublicInputs opub{};
    opub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);
    // Legacy proof ignores cv; build it (note proves cm with val=1e6).
    auto legacy_proof = ProveOutput(w, opub, nullptr, true, /*cv_bound=*/false);
    ASSERT_FALSE(legacy_proof.empty());

    // Build a validation-ready bundle whose cv/range/binding all say value 0.
    PlannedOutput po{};
    po.commitment = opub.commitment;
    po.value_una = 0;            // the lie: cv = commit(rcv, 0), value_balance = 0
    po.rcv = w.rcv;
    po.nonce = MakeHash(0x44, 0x55);
    po.output_proof = legacy_proof;

    ShieldedBundle bundle;
    ASSERT_EQ(BuildShieldedBundle({}, {po}, tx_sighash, bundle),
              BundleBuildResult::Ok);
    ASSERT_EQ(bundle.value_balance, 0);

    NullifierSet nullifiers;

    auto make_ctx = [&](uint32_t cv_height) {
        ValidationContext ctx(
            &nullifiers,
            /*commitment_tree=*/nullptr,  // outputs-only: no anchor check
            /*block_height=*/100,
            /*transparent_value_delta=*/0,  // == bundle.value_balance
            /*shielded_activation_height=*/0,
            /*anchor_history=*/nullptr,
            tx_sighash);
        ctx.shielded_input_binding_activation_height = 0;  // range/binding mandatory
        ctx.shielded_cv_binding_activation_height = cv_height;
        return ctx;
    };

    // Pre-activation: legacy rule accepts the forgery — value minted from nothing.
    EXPECT_EQ(ValidateShieldedBundle(bundle, make_ctx(UINT32_MAX)),
              ShieldedValidationError::Ok);

    // Post-activation: cv-binding required; legacy proof rejected.
    EXPECT_EQ(ValidateShieldedBundle(bundle, make_ctx(0)),
              ShieldedValidationError::ProofInvalid);
}

// ─────────────────────────────────────────────────────────────────────────
// 5. Consensus-level honest acceptance: a correctly cv-bound bundle
//    (cv = val·V + rcv·G, value_balance == val) is ACCEPTED post-activation.
// ─────────────────────────────────────────────────────────────────────────
TEST(ShieldedCvBinding, ConsensusAcceptsHonestCvBoundBundle) {
    ASSERT_TRUE(PedersenGeneratorsReady());
    const uint64_t value = 100'000'000;  // 1 DIN
    const Hash tx_sighash = MakeHash(0xAB, 0xCD);

    OutputWitness w{};
    w.value = ValueAsHash(value);
    w.public_key = MakeHash(0x31, 0x10);
    w.randomness = MakeHash(0x32, 0x10);
    w.rcv = MakeBlind(0x3D);

    OutputPublicInputs opub{};
    opub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);
    opub.cv = Commit(w.rcv, value);  // honest cv

    auto proof = ProveOutput(w, opub, nullptr, true, /*cv_bound=*/true);
    ASSERT_FALSE(proof.empty());

    PlannedOutput po{};
    po.commitment = opub.commitment;
    po.value_una = value;  // honest: cv/range/binding all agree with the note
    po.rcv = w.rcv;
    po.nonce = MakeHash(0x46, 0x57);
    po.output_proof = proof;

    ShieldedBundle bundle;
    ASSERT_EQ(BuildShieldedBundle({}, {po}, tx_sighash, bundle),
              BundleBuildResult::Ok);
    ASSERT_EQ(bundle.value_balance, static_cast<int64_t>(value));
    // The builder must reproduce the exact cv the proof was bound to.
    ASSERT_EQ(bundle.outputs[0].cv, opub.cv);

    NullifierSet nullifiers;
    ValidationContext ctx(
        &nullifiers, /*commitment_tree=*/nullptr, /*block_height=*/100,
        /*transparent_value_delta=*/static_cast<int64_t>(value),
        /*shielded_activation_height=*/0, /*anchor_history=*/nullptr, tx_sighash);
    ctx.shielded_input_binding_activation_height = 0;
    ctx.shielded_cv_binding_activation_height = 0;  // cv-binding active

    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx), ShieldedValidationError::Ok);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
