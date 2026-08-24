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
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"
#include "shielded_audit_desync.h"  // test-only transcript-desync provers

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::CommitmentTree;
using shielded::Hash;
using shielded::NoteCommitment;
using shielded::ComputeNullifier;
using shielded::PoseidonHash2;
using shielded::PedersenCommit;
using shielded::PedersenResult;
using shielded::OutputPublicInputs;
using shielded::OutputWitness;
using shielded::ProveOutput;
using shielded::ProveOutput_AuditDesync;
using shielded::ProveSpend;
using shielded::ProveSpend_AuditDesync;
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

// Companion to the spend anchor/nullifier binding PoCs (CONFIRMED-CRIT-05): prove the
// OUTPUT commitment is bound too. TamperedCommitmentRejected above tampers an honest
// proof and so can reject on transcript mismatch (false confidence); this desyncs the
// commitment at proof-generation time (transcript bound to the presented C2), so the
// only thing that can reject a committed≠presented proof is genuine binding. A sound
// system MUST reject. Regression for the z=(1,io,W) split's public-input binding.
TEST(ShieldedOutputCircuitTest, CommitmentBindingPoC) {
    OutputWitness w;
    w.value      = ValueAsHash(50'000'000);
    w.public_key = MakeHash(0x12, 0x20);
    w.randomness = MakeHash(0x13, 0x20);

    OutputPublicInputs pub_committed;
    pub_committed.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);

    // Sanity: honest round-trip verifies.
    auto honest = ProveOutput(w, pub_committed, nullptr);
    ASSERT_FALSE(honest.empty());
    ASSERT_TRUE(VerifyOutput(honest, pub_committed, nullptr));

    // C2: a different presented commitment.
    OutputPublicInputs pub_present = pub_committed;
    pub_present.commitment[5] ^= 0x01;
    ASSERT_NE(pub_present.commitment, pub_committed.commitment);

    // Circuit committed to C1; transcript + presentation bound to C2.
    auto forged = ProveOutput_AuditDesync(w, /*pub_committed=*/pub_committed,
                                          /*pub_present=*/pub_present, nullptr);
    ASSERT_FALSE(forged.empty());

    const bool accepted = VerifyOutput(forged, pub_present, nullptr);
    std::cerr << "\n[OutputCommitmentBindingPoC] committed C1, presented+transcript C2; "
              << "VerifyOutput(forged, C2) accepted = "
              << (accepted ? "TRUE  ==> COMMITMENT UNBOUND (bug)" : "false ==> commitment bound (sound)")
              << "\n\n";
    EXPECT_FALSE(accepted);  // sound system MUST reject
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

// SUSPECTED-01 PoC (2026-05-30): does the verifier bind the on-chain anchor to the
// COMMITTED witness, or only to the Fiat-Shamir transcript?
//
// The existing TamperedAnchorRejected test above proves nothing about binding: it
// reuses an honest proof (transcript bound to A1) and verifies with A2, so it fails
// on the *transcript mismatch*, not on anchor-to-witness binding. This test isolates
// binding correctly: the proof's circuit is committed to the REAL anchor A1 (so it is
// satisfiable), but BOTH the transcript AND the presented public input are A2. The
// transcripts therefore MATCH (no Fiat-Shamir mismatch), so the only thing that could
// reject is a genuine binding of the committed anchor (A1) to the presented one (A2).
//
//   sound system  -> VerifySpend rejects (A1 in witness != A2 presented)
//   unbound system -> VerifySpend ACCEPTS (verifier never checks z-io == presented pub)
TEST(ShieldedSpendCircuitTest, SUSPECTED01_AnchorBindingPoC) {
    SpendFixture fx;
    fx.Build();

    // Sanity: the honest round-trip works.
    auto honest = ProveSpend(fx.witness, fx.pub, nullptr);
    ASSERT_FALSE(honest.empty());
    ASSERT_TRUE(VerifySpend(honest, fx.pub, nullptr));

    // A2: a different "root" (one byte flipped from the real anchor A1). In a real
    // attack this would be any real historical root the attacker does not have a path
    // to; for the binding test any value != A1 suffices.
    SpendPublicInputs pub_present = fx.pub;
    pub_present.anchor[10] ^= 0x40;
    ASSERT_NE(pub_present.anchor, fx.pub.anchor);

    // Forge: circuit committed to A1 (fx.pub), transcript + presentation bound to A2.
    auto forged = ProveSpend_AuditDesync(fx.witness, /*pub_committed=*/fx.pub,
                                         /*pub_present=*/pub_present, nullptr);
    ASSERT_FALSE(forged.empty());  // committed circuit is the real, satisfiable A1 spend

    const bool accepted = VerifySpend(forged, pub_present, nullptr);
    std::cerr << "\n========================================================\n"
              << "[SUSPECTED-01 PoC] committed anchor A1, presented+transcript anchor A2\n"
              << "  VerifySpend(forged, A2) accepted = "
              << (accepted ? "TRUE  ==> PUBLIC-INPUT BINDING ABSENT (forgery confirmed)"
                           : "false ==> anchor IS bound to the witness (sound)")
              << "\n========================================================\n\n";

    // REGRESSION TEST for the CONFIRMED-CRIT-05 fix (Spartan z=(1,io,W) split): a sound
    // proof system MUST reject this forgery. Before the fix this accepted (=TRUE) and the
    // assertion failed, exposing the bug; with the fix it must reject (=false). If this
    // ever fails again, the public-input binding has regressed.
    EXPECT_FALSE(accepted);
}

// Companion to the anchor PoC: prove the NULLIFIER is bound too. The nullifier sits at
// witness index 1, the anchor at index 2 — so a slot-specific layout bug could bind one
// and not the other. This desyncs the nullifier (not the anchor) at proof-generation
// time; a sound system must reject. (The existing TamperedNullifierRejected only catches
// the transcript-mismatch case, like the old anchor test did.)
TEST(ShieldedSpendCircuitTest, NullifierBindingPoC) {
    SpendFixture fx;
    fx.Build();

    SpendPublicInputs pub_present = fx.pub;
    pub_present.nullifier[3] ^= 0x80;  // N2 != N1
    ASSERT_NE(pub_present.nullifier, fx.pub.nullifier);

    // Circuit committed to the real nullifier N1; transcript + presentation bound to N2.
    auto forged = ProveSpend_AuditDesync(fx.witness, /*pub_committed=*/fx.pub,
                                         /*pub_present=*/pub_present, nullptr);
    ASSERT_FALSE(forged.empty());

    const bool accepted = VerifySpend(forged, pub_present, nullptr);
    std::cerr << "\n[NullifierBindingPoC] committed nullifier N1, presented+transcript N2; "
              << "VerifySpend(forged, N2) accepted = "
              << (accepted ? "TRUE  ==> NULLIFIER UNBOUND (bug)" : "false ==> nullifier bound (sound)")
              << "\n\n";
    EXPECT_FALSE(accepted);  // sound system MUST reject
}

// Activation-gate behavior (CONFIRMED-CRIT-05): proves the height gate's two branches
// select different rules. Below the binding-activation height (bind=false) the proof
// system reproduces the pre-fix UNBOUND rule and ACCEPTS the anchor forgery; at/above it
// (bind=true) the same forgery is REJECTED. This documents the known pre-activation
// weakness and verifies the gate flips behavior. Honest proofs round-trip under each
// rule when prover and verifier agree (which the height gate guarantees).
TEST(ShieldedSpendCircuitTest, ActivationGate_OldRuleAcceptsForgery_NewRuleRejects) {
    SpendFixture fx;
    fx.Build();

    SpendPublicInputs pub_present = fx.pub;
    pub_present.anchor[10] ^= 0x40;

    // Pre-activation (bind=false): the forgery is ACCEPTED — the rule is unbound.
    auto forged_old = ProveSpend_AuditDesync(fx.witness, fx.pub, pub_present, nullptr,
                                             /*bind_public_inputs=*/false);
    ASSERT_FALSE(forged_old.empty());
    EXPECT_TRUE(VerifySpend(forged_old, pub_present, nullptr, /*bind_public_inputs=*/false));

    // Post-activation (bind=true): the same forgery is REJECTED.
    auto forged_new = ProveSpend_AuditDesync(fx.witness, fx.pub, pub_present, nullptr,
                                             /*bind_public_inputs=*/true);
    ASSERT_FALSE(forged_new.empty());
    EXPECT_FALSE(VerifySpend(forged_new, pub_present, nullptr, /*bind_public_inputs=*/true));

    // Honest proofs round-trip under BOTH rules (prover/verifier agree).
    auto honest_old = ProveSpend(fx.witness, fx.pub, nullptr, /*bind_public_inputs=*/false);
    auto honest_new = ProveSpend(fx.witness, fx.pub, nullptr, /*bind_public_inputs=*/true);
    EXPECT_TRUE(VerifySpend(honest_old, fx.pub, nullptr, /*bind_public_inputs=*/false));
    EXPECT_TRUE(VerifySpend(honest_new, fx.pub, nullptr, /*bind_public_inputs=*/true));
}

// ── Spend authority (shielded_spend_auth_activation_height) ─────────────
//
// Below the gate, the note's committed key is pk = Poseidon(sk, 0) with `sk`
// invented by the SENDER, so the sender can spend the note they sent, forever.
// At and above it, the note commits to pk_d = s·G and the circuit proves
// knowledge of dlog(pk_d) — a value the sender never learns.
//
// pk_d is derived here with raw libsecp256k1 rather than the wallet's
// DeriveDiversifiedSpendKey, so the circuit is checked against an INDEPENDENT
// oracle rather than against our own derivation helper.

// s -> (even-y normalised s, x-only pk_d). Mirrors NormalizeScalarToEvenY.
struct AuthKey {
    Hash s{};
    Hash pk_d{};
};

static bool MakeAuthKey(const Hash& candidate, AuthKey& out) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (ctx == nullptr) return false;
    bool ok = false;
    secp256k1_keypair kp{};
    if (secp256k1_ec_seckey_verify(ctx, candidate.data()) == 1 &&
        secp256k1_keypair_create(ctx, &kp, candidate.data()) == 1) {
        secp256k1_xonly_pubkey xonly{};
        int parity = 0;
        if (secp256k1_keypair_sec(ctx, out.s.data(), &kp) == 1 &&
            secp256k1_keypair_xonly_pub(ctx, &xonly, &parity, &kp) == 1 &&
            secp256k1_xonly_pubkey_serialize(ctx, out.pk_d.data(), &xonly) == 1) {
            // Even-y normalisation: if the x-only path negated to even y,
            // negate the scalar too so s·G == pk_d with even y.
            ok = (parity != 1) ||
                 (secp256k1_ec_seckey_negate(ctx, out.s.data()) == 1);
        }
    }
    secp256k1_context_destroy(ctx);
    return ok;
}

// A note committed to pk_d = s·G, spendable only by the holder of s.
struct AuthFixture {
    AuthKey key{};
    Hash value      = ValueAsHash(123'456'789);
    Hash randomness = MakeHash(0xC7, 0x31);
    Hash d          = MakeHash(0xB0, 0x0B);

    CommitmentTree tree;
    SpendWitness witness{};
    SpendPublicInputs pub{};

    void Build() {
        ASSERT_TRUE(MakeAuthKey(MakeHash(0x5A, 0x9E), key));

        tree.Append(MakeHash(0x10));
        tree.Append(MakeHash(0x11));
        // Commit to pk_d — NOT to a sender-invented Poseidon(sk, 0).
        const Hash cm = NoteCommitment(d, key.pk_d, value, randomness);
        const uint64_t idx = tree.Append(cm);
        ASSERT_GT(idx, 0u);
        const auto path = tree.GetAuthPath(idx);
        ASSERT_TRUE(path.has_value());

        witness.secret_key  = key.s;   // `s` occupies the secret_key slot
        witness.leaf_index  = idx;
        witness.value       = value;
        witness.randomness  = randomness;
        witness.d           = d;
        witness.rcv         = MakeHash(0x09, 0x11);
        witness.merkle_path = path->siblings;

        pub.nullifier = ComputeNullifier(key.s, idx);  // nf from s
        pub.anchor    = tree.Root();
        ASSERT_EQ(PedersenCommit(witness.rcv, 123'456'789ULL, pub.cv), PedersenResult::Ok);
    }
};

TEST(ShieldedSpendAuthTest, ValidAuthProofVerifies) {
    AuthFixture fx;
    fx.Build();
    auto proof = ProveSpend(fx.witness, fx.pub, nullptr, true,
                            /*cv_bound=*/true, /*spend_auth=*/true);
    ASSERT_FALSE(proof.empty());
    EXPECT_TRUE(VerifySpend(proof, fx.pub, nullptr, true, true, /*spend_auth=*/true));
}

// ★ THE DOUBLE-SPEND HOLE THIS GUARDS.
//
// pk_d is committed X-ONLY, so a circuit constraining only x(s·G) == pk_d_x
// admits BOTH s and (q - s): (q-s)·G = -(s·G) shares the same x, hence the
// same commitment, the same Merkle path and the same anchor. But
// nf = Poseidon(s, idx) != Poseidon(q-s, idx), so ONE note would yield TWO
// valid nullifiers — the owner spends it twice.
//
// The circuit's even-y constraint makes the representative unique, matching
// what NormalizeScalarToEvenY guarantees wallet-side. Neuter that constraint
// and this test fails: the negated proof is produced and accepted.
TEST(ShieldedSpendAuthTest, NegatedScalarCannotProduceSecondNullifier) {
    AuthFixture fx;
    fx.Build();

    // q - s. Same public key x, so the same note commitment.
    Hash neg_s = fx.key.s;
    {
        secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        ASSERT_NE(ctx, nullptr);
        const int rc = secp256k1_ec_seckey_negate(ctx, neg_s.data());
        secp256k1_context_destroy(ctx);
        ASSERT_EQ(rc, 1);
    }
    ASSERT_NE(neg_s, fx.key.s);

    // The negated scalar yields a DIFFERENT nullifier for the SAME note —
    // that is exactly what makes it a double-spend if the proof succeeds.
    SpendPublicInputs second = fx.pub;
    second.nullifier = ComputeNullifier(neg_s, fx.witness.leaf_index);
    ASSERT_NE(second.nullifier, fx.pub.nullifier);

    SpendWitness w = fx.witness;
    w.secret_key = neg_s;

    auto proof = ProveSpend(w, second, nullptr, true,
                            /*cv_bound=*/true, /*spend_auth=*/true);
    // Odd-y ⇒ the circuit is unsatisfiable ⇒ no proof. Belt and braces: even
    // if one were produced, it must not verify.
    EXPECT_TRUE(proof.empty()) << "negated scalar produced a proof — the note "
                                  "has two spendable nullifiers";
    if (!proof.empty()) {
        EXPECT_FALSE(VerifySpend(proof, second, nullptr, true, true, true));
    }
}

// The whole point: knowing the recipient's ADDRESS (d, pk_d) is not enough to
// spend. Only the holder of `s` can satisfy the circuit.
TEST(ShieldedSpendAuthTest, WrongScalarCannotSpend) {
    AuthFixture fx;
    fx.Build();

    AuthKey other{};
    ASSERT_TRUE(MakeAuthKey(MakeHash(0x77, 0x42), other));
    ASSERT_NE(other.s, fx.key.s);

    SpendWitness w = fx.witness;
    w.secret_key = other.s;

    SpendPublicInputs pub = fx.pub;
    pub.nullifier = ComputeNullifier(other.s, fx.witness.leaf_index);

    auto proof = ProveSpend(w, pub, nullptr, true, true, /*spend_auth=*/true);
    EXPECT_TRUE(proof.empty());
    if (!proof.empty()) {
        EXPECT_FALSE(VerifySpend(proof, pub, nullptr, true, true, true));
    }
}

// The sender-retained-authority bug itself: under the LEGACY rule the note's
// key is Poseidon(sk, 0), which the sender chose. Against an auth-committed
// note that key is worthless — proving the gate actually changes who can spend.
TEST(ShieldedSpendAuthTest, LegacySenderKeyCannotSpendAuthNote) {
    AuthFixture fx;
    fx.Build();

    // A sender-invented sk whose legacy pk = Poseidon(sk, 0) is NOT pk_d.
    const Hash sender_sk = MakeHash(0xA1, 0xF0);
    ASSERT_NE(PoseidonHash2(sender_sk, Hash{}), fx.key.pk_d);

    SpendWitness w = fx.witness;
    w.secret_key = sender_sk;
    SpendPublicInputs pub = fx.pub;
    pub.nullifier = ComputeNullifier(sender_sk, fx.witness.leaf_index);

    auto proof = ProveSpend(w, pub, nullptr, true, true, /*spend_auth=*/true);
    EXPECT_TRUE(proof.empty());
}

// s == 0 maps to the identity, whose (x, y) = (0, 0) packs to pk == 0 and would
// pass an even-y check. A note committed to pk == 0 is not something an honest
// wallet can produce (0 is not a valid x-only key), but it IS constructible
// here, so pin the property directly rather than trusting the gadget's
// is_identity flag to be set. Asserts the SECURITY property, not the mechanism.
TEST(ShieldedSpendAuthTest, ZeroScalarCannotSpendZeroKeyNote) {
    const Hash zero{};
    const Hash value = ValueAsHash(123'456'789);
    const Hash randomness = MakeHash(0xD1, 0x55);
    const Hash d = MakeHash(0xB0, 0x0B);

    CommitmentTree tree;
    tree.Append(MakeHash(0x10));
    tree.Append(MakeHash(0x11));
    // A note whose committed key is literally zero — what s = 0 would derive.
    const Hash cm = NoteCommitment(d, zero, value, randomness);
    const uint64_t idx = tree.Append(cm);
    const auto path = tree.GetAuthPath(idx);
    ASSERT_TRUE(path.has_value());

    SpendWitness w{};
    w.secret_key  = zero;
    w.leaf_index  = idx;
    w.value       = value;
    w.randomness  = randomness;
    w.d           = d;
    w.rcv         = MakeHash(0x09, 0x11);
    w.merkle_path = path->siblings;

    SpendPublicInputs pub{};
    pub.nullifier = ComputeNullifier(zero, idx);
    pub.anchor    = tree.Root();
    ASSERT_EQ(PedersenCommit(w.rcv, 123'456'789ULL, pub.cv), PedersenResult::Ok);

    auto proof = ProveSpend(w, pub, nullptr, true, true, /*spend_auth=*/true);
    EXPECT_TRUE(proof.empty()) << "s = 0 produced a spendable proof";
    if (!proof.empty()) {
        EXPECT_FALSE(VerifySpend(proof, pub, nullptr, true, true, true));
    }
}

// Version bytes must keep the variants from being presented for one another.
TEST(ShieldedSpendAuthTest, AuthProofRejectedUnderCvOnlyRuleAndViceVersa) {
    AuthFixture fx;
    fx.Build();

    auto auth_proof = ProveSpend(fx.witness, fx.pub, nullptr, true, true,
                                 /*spend_auth=*/true);
    ASSERT_FALSE(auth_proof.empty());
    // Presented where only cv-binding is required: version mismatch ⇒ reject.
    EXPECT_FALSE(VerifySpend(auth_proof, fx.pub, nullptr, true, /*cv_bound=*/true,
                             /*spend_auth=*/false));

    // And a cv-only proof presented where auth is required must also fail.
    SpendFixture cv;
    cv.Build();
    ASSERT_EQ(PedersenCommit(cv.witness.rcv, 123'456'789ULL, cv.pub.cv), PedersenResult::Ok);
    auto cv_proof = ProveSpend(cv.witness, cv.pub, nullptr, true, /*cv_bound=*/true);
    ASSERT_FALSE(cv_proof.empty());
    EXPECT_FALSE(VerifySpend(cv_proof, cv.pub, nullptr, true, true, /*spend_auth=*/true));
}

// spend_auth is a strict superset of cv_bound. The invalid pair must be
// rejected outright, not silently resolved — an auth-without-cv circuit would
// prove ownership while leaving value unbound (mint-from-nothing).
TEST(ShieldedSpendAuthTest, AuthWithoutCvBoundRejected) {
    AuthFixture fx;
    fx.Build();
    EXPECT_TRUE(ProveSpend(fx.witness, fx.pub, nullptr, true,
                           /*cv_bound=*/false, /*spend_auth=*/true).empty());
    EXPECT_FALSE(VerifySpend(std::vector<uint8_t>(256, 0xCC), fx.pub, nullptr, true,
                             /*cv_bound=*/false, /*spend_auth=*/true));
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
