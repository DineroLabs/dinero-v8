// Copyright (c) 2026 Dinero Labs.
//
// Phase 0 wave 3 — integration tests for `ValidateShieldedBundle`
// and `ApplyShieldedBundle`. Covers every `ShieldedValidationError`
// the validator can emit (Ok, BundleMalformed, NullifierDuplicate,
// AnchorInvalid, ProofInvalid, ValueBalanceMismatch) plus the
// state-mutation contract of Apply.
//
// Cheap rejection tests use placeholder zk_proofs because the
// validator runs structural / nullifier / anchor checks before
// invoking the prover. Two slow tests exercise the full prove→verify
// pipeline against the real ShieldedCircuit.

#include <gtest/gtest.h>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/shielded_validation.h"
#include "primitives/transaction.h"
#include "wallet/shielded_wallet_ops.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define DINERO_GETPID _getpid
#else
#include <unistd.h>
#define DINERO_GETPID getpid
#endif

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::ApplyShieldedBundle;
using shielded::CommitmentTree;
using shielded::ComputeNullifier;
using shielded::Hash;
using shielded::NoteCommitment;
using shielded::NullifierSet;
using shielded::OutputPublicInputs;
using shielded::OutputWitness;
using shielded::PoseidonHash2;
using shielded::ProveOutput;
using shielded::ProveSpend;
using shielded::ShieldedBundle;
using shielded::ShieldedOutput;
using shielded::ShieldedSpend;
using shielded::ShieldedValidationError;
using shielded::SpendPublicInputs;
using shielded::SpendWitness;
using shielded::ValidateShieldedBundle;
using shielded::ValidationContext;

Hash MakeHash(uint8_t seed, uint8_t tail = 0xCD) {
    Hash h{};
    h[0]  = seed;
    h[31] = tail;
    return h;
}

// Phase 2 wave 4: encode a uint64 value into Hash matching the
// circuit's big-endian Scalar layout so the in-circuit range check
// (val < 2^64) is satisfied.
Hash ValueAsHash(uint64_t v) {
    Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

ShieldedSpend MakeSpendStub(uint8_t nf, const Hash& anchor) {
    ShieldedSpend s;
    s.nullifier = MakeHash(nf);
    s.anchor    = anchor;
    s.zk_proof  = std::vector<uint8_t>(64, nf);  // non-empty placeholder
    return s;
}

ShieldedOutput MakeOutputStub(uint8_t cm) {
    ShieldedOutput o;
    o.commitment      = MakeHash(cm);
    o.encrypted_note  = std::vector<uint8_t>(96, cm);
    o.zk_proof        = std::vector<uint8_t>(64, cm);
    return o;
}

std::string TempDbPath() {
    // Portable: pid + ns-timestamp + atomic counter, in the platform's
    // temp directory. Same uniqueness as the previous POSIX
    // mkstemps + close + unlink pattern.
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<unsigned long long>(DINERO_GETPID());
    const auto ts = static_cast<long long>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    char name[96];
    std::snprintf(name, sizeof(name),
                  "dinero_validation_test_%llu_%lld_%llu.db",
                  pid, ts, static_cast<unsigned long long>(seq));
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

class ShieldedValidationFixture : public ::testing::Test {
protected:
    CommitmentTree tree;
    NullifierSet nullifier_set;
    std::string nf_path = TempDbPath();
    // Construct with all consensus inputs explicit. nullifier_set is
    // re-pointed in SetUp() once Open() has prepared the on-disk DB.
    // Phase 1 gate: activation set to 0 so existing tests exercise the
    // post-activation code path. The gate itself is exercised by
    // RejectsNonEmptyBundleBeforeActivation below. tx_sighash stays
    // Hash{} because these structural / nullifier / anchor tests don't
    // construct bundles with aggregated_range_proof, so the binding-sig
    // branch is gated off.
    ValidationContext ctx{
        /*nullifier_set=*/nullptr,
        /*commitment_tree=*/&tree,
        /*block_height=*/100,
        /*transparent_value_delta=*/0,
        /*shielded_activation_height=*/0,
        /*anchor_history=*/nullptr,
        /*tx_sighash=*/Hash{},
    };

    void SetUp() override {
        ASSERT_EQ(nullifier_set.Open(nf_path), NullifierSet::OpenResult::Ok);
        ctx.nullifier_set = &nullifier_set;
    }

    void TearDown() override {
        nullifier_set.Close();
        std::filesystem::remove(nf_path);
    }
};

// ── Cheap rejection paths (no prover invocation) ────────────────────

TEST_F(ShieldedValidationFixture, EmptyBundleIsOk) {
    ShieldedBundle bundle;
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx), ShieldedValidationError::Ok);
}

// ── Phase 1 activation gate ─────────────────────────────────────────

TEST_F(ShieldedValidationFixture, RejectsNonEmptyBundleBeforeActivation) {
    // Activation set above the current block height: any non-empty bundle
    // must be rejected with NotActive *before* structural / nullifier /
    // anchor / proof checks run.
    ctx.block_height                = 100;
    ctx.shielded_activation_height  = 1'000;

    ShieldedBundle bundle;
    bundle.outputs.push_back(MakeOutputStub(0xFE));
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::NotActive);
}

TEST_F(ShieldedValidationFixture, EmptyBundleStillOkBeforeActivation) {
    // Empty bundles short-circuit BEFORE the gate — a v1/v2 transparent
    // tx that happens to use TX_VERSION_SHIELDED with no shielded data
    // (or any pre-activation block with no shielded txs) must not be
    // disrupted by the gate.
    ctx.block_height                = 100;
    ctx.shielded_activation_height  = 1'000;

    ShieldedBundle bundle;
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::Ok);
}

TEST_F(ShieldedValidationFixture, FailsClosedWhenActivationFieldDefaulted) {
    // ForPreActivationTests sets shielded_activation_height = UINT32_MAX
    // (fail-closed). Any non-empty bundle must be rejected with NotActive
    // before structural / nullifier / anchor checks run.
    auto bare = ValidationContext::ForPreActivationTests(
        &nullifier_set, &tree, /*block_height=*/100, /*transparent_value_delta=*/0);

    ShieldedBundle bundle;
    bundle.outputs.push_back(MakeOutputStub(0xAB));
    EXPECT_EQ(ValidateShieldedBundle(bundle, bare),
              ShieldedValidationError::NotActive);
}

TEST_F(ShieldedValidationFixture, SpendWithEmptyProofIsMalformed) {
    ShieldedBundle bundle;
    auto s = MakeSpendStub(0x01, tree.Root());
    s.zk_proof.clear();  // structural failure
    bundle.spends.push_back(s);
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::BundleMalformed);
}

TEST_F(ShieldedValidationFixture, OutputWithEmptyProofIsMalformed) {
    ShieldedBundle bundle;
    auto o = MakeOutputStub(0x02);
    o.zk_proof.clear();
    bundle.outputs.push_back(o);
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::BundleMalformed);
}

TEST_F(ShieldedValidationFixture, DuplicateNullifierWithinBundleRejected) {
    ShieldedBundle bundle;
    bundle.spends.push_back(MakeSpendStub(0x10, tree.Root()));
    bundle.spends.push_back(MakeSpendStub(0x10, tree.Root()));  // same nf
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::NullifierDuplicate);
}

TEST_F(ShieldedValidationFixture, NullifierAlreadyInSetRejected) {
    ASSERT_TRUE(nullifier_set.Insert(MakeHash(0x20), 50));
    ShieldedBundle bundle;
    bundle.spends.push_back(MakeSpendStub(0x20, tree.Root()));
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::NullifierDuplicate);
}

TEST_F(ShieldedValidationFixture, AnchorMismatchRejected) {
    // Tree currently empty → Root() is the empty-tree root.
    Hash bogus_anchor{};
    bogus_anchor[0] = 0xFF;
    ShieldedBundle bundle;
    bundle.spends.push_back(MakeSpendStub(0x30, bogus_anchor));
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::AnchorInvalid);
}

// ── Phase 2 anchor depth window ─────────────────────────────────────

TEST_F(ShieldedValidationFixture, HistoricalAnchorAccepted) {
    // Simulate a chain that was at root R1 at height 100, then advanced
    // to root R2 at height 101 (current). A spend whose anchor is R1
    // (proven against the older snapshot) must still validate as long
    // as R1 is in the history window.
    const Hash old_root_at_100 = MakeHash(0xAA);

    shielded::AnchorHistory history;
    history.RecordRoot(100, old_root_at_100);
    ctx.anchor_history = &history;

    // Tree is currently empty, so current root != old_root_at_100.
    // Without history, this would fail with AnchorInvalid (per the
    // previous test). With history wired in, the anchor check must
    // pass — the validator then proceeds to binding sig + proof
    // checks. We compute a valid binding sig over the bundle so the
    // validator reaches the proof-verify stage; the junk proof bytes
    // get rejected there → ProofInvalid. That's the signal the anchor
    // check passed.
    ShieldedBundle bundle;
    bundle.spends.push_back(MakeSpendStub(0x40, old_root_at_100));
    // Wave 2: validation skips binding-sig check for bundles without
    // aggregated_range_proof, so leaving binding_sig zero-init is fine
    // for these legacy fixtures. Wave 2 path is exercised by the new
    // CrossBundleBalance tests.
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::ProofInvalid);
}

TEST_F(ShieldedValidationFixture, AnchorNotInHistoryStillRejected) {
    // History has SOME roots, but none match the spend's anchor.
    shielded::AnchorHistory history;
    history.RecordRoot(50, MakeHash(0xC1));
    history.RecordRoot(51, MakeHash(0xC2));
    ctx.anchor_history = &history;

    Hash unknown_anchor = MakeHash(0xDE);  // not in history, not current
    ShieldedBundle bundle;
    bundle.spends.push_back(MakeSpendStub(0x42, unknown_anchor));
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::AnchorInvalid);
}

// ── Full pipeline: real proofs, real state ──────────────────────────

// Build a self-consistent (one spend, one output) bundle whose
// proofs verify and whose value_balance is zero (pure transfer).
// Returns the constructed bundle. Mutates `tree` to contain the
// note being spent at index 0.
ShieldedBundle BuildHappyBundle(CommitmentTree& tree_ref) {
    const Hash sk         = MakeHash(0x70, 0xF1);
    const Hash spend_val  = ValueAsHash(100'000'000);  // 1 DIN
    const Hash spend_rand = MakeHash(0x72, 0xF1);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash d{};  // Phase 2 wave 5: zero diversifier for tests.

    const Hash spend_cm = NoteCommitment(d, pk, spend_val, spend_rand);
    const uint64_t leaf_idx = tree_ref.Append(spend_cm);

    SpendWitness sw;
    sw.secret_key  = sk;
    sw.leaf_index  = leaf_idx;
    sw.value       = spend_val;
    sw.randomness  = spend_rand;
    sw.d           = d;
    auto path = tree_ref.GetAuthPath(leaf_idx);
    EXPECT_TRUE(path.has_value());
    sw.merkle_path = path->siblings;

    SpendPublicInputs spi;
    spi.nullifier = ComputeNullifier(sk, leaf_idx);
    spi.anchor    = tree_ref.Root();
    auto spend_proof = ProveSpend(sw, spi, nullptr);
    EXPECT_FALSE(spend_proof.empty());

    // Output: re-commit the same value to the same pk with new randomness.
    OutputWitness ow;
    ow.value      = spend_val;
    ow.public_key = pk;
    ow.randomness = MakeHash(0x73, 0xF1);
    ow.d          = d;

    OutputPublicInputs opi;
    opi.commitment = NoteCommitment(ow.d, ow.public_key, ow.value, ow.randomness);
    auto output_proof = ProveOutput(ow, opi, nullptr);
    EXPECT_FALSE(output_proof.empty());

    ShieldedBundle bundle;
    bundle.value_balance = 0;  // pure transfer
    ShieldedSpend s;
    s.nullifier = spi.nullifier;
    s.anchor    = spi.anchor;
    s.zk_proof  = std::move(spend_proof);
    bundle.spends.push_back(std::move(s));

    ShieldedOutput o;
    o.commitment      = opi.commitment;
    o.encrypted_note  = std::vector<uint8_t>(96, 0xAA);
    o.zk_proof        = std::move(output_proof);
    bundle.outputs.push_back(std::move(o));

    // v0 binding tag: SHA-256 of canonical bytes with binding_sig zeroed.
    // Wave 2: validation skips binding-sig check for bundles without
    // aggregated_range_proof, so leaving binding_sig zero-init is fine
    // for these legacy fixtures. Wave 2 path is exercised by the new
    // CrossBundleBalance tests.
    return bundle;
}

TEST_F(ShieldedValidationFixture, HappyPathBundleAccepted) {
    auto bundle = BuildHappyBundle(tree);
    ctx.transparent_value_delta = 0;
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::Ok);
}

TEST_F(ShieldedValidationFixture, TamperedSpendProofRejected) {
    auto bundle = BuildHappyBundle(tree);
    ASSERT_FALSE(bundle.spends[0].zk_proof.empty());
    bundle.spends[0].zk_proof[10] ^= 0xFF;  // corrupt proof bytes
    // Simulate a sophisticated attacker who also recomputes the
    // binding tag after tampering — proof check is still the
    // backstop, so we get ProofInvalid (not BindingSigInvalid).
    // Wave 2: validation skips binding-sig check for bundles without
    // aggregated_range_proof, so leaving binding_sig zero-init is fine
    // for these legacy fixtures. Wave 2 path is exercised by the new
    // CrossBundleBalance tests.
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::ProofInvalid);
}

TEST_F(ShieldedValidationFixture, ValueBalanceMismatchRejected) {
    auto bundle = BuildHappyBundle(tree);
    bundle.value_balance = 0;
    ctx.transparent_value_delta = 1'000;  // disagree with bundle
    // value_balance change invalidates the binding tag — recompute so
    // the test exercises ValueBalanceMismatch, not BindingSigInvalid.
    // Wave 2: validation skips binding-sig check for bundles without
    // aggregated_range_proof, so leaving binding_sig zero-init is fine
    // for these legacy fixtures. Wave 2 path is exercised by the new
    // CrossBundleBalance tests.
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::ValueBalanceMismatch);
}

// ── Phase 2 bundle-size limits ──────────────────────────────────────

TEST_F(ShieldedValidationFixture, OversizedSpendCountRejected) {
    // Build a bundle with kMaxSpendsPerBundle + 1 spends (junk proofs
    // OK — size check fires before structural / nullifier / proof).
    ShieldedBundle bundle;
    for (size_t i = 0; i <= shielded::kMaxSpendsPerBundle; ++i) {
        // Use distinct nullifier per spend so we'd reach nullifier
        // check at all. Anchor doesn't matter — size check is earlier.
        ShieldedSpend s;
        s.nullifier = MakeHash(static_cast<uint8_t>(i & 0xFF), 0xE1);
        s.nullifier[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        s.anchor    = tree.Root();
        s.zk_proof  = std::vector<uint8_t>(8, 0xAA);
        bundle.spends.push_back(std::move(s));
    }
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::BundleTooLarge);
}

TEST_F(ShieldedValidationFixture, OversizedOutputCountRejected) {
    ShieldedBundle bundle;
    for (size_t i = 0; i <= shielded::kMaxOutputsPerBundle; ++i) {
        ShieldedOutput o;
        o.commitment = MakeHash(static_cast<uint8_t>(i & 0xFF), 0xE2);
        o.commitment[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        o.encrypted_note = std::vector<uint8_t>(16, 0xBB);
        o.zk_proof       = std::vector<uint8_t>(8, 0xCC);
        bundle.outputs.push_back(std::move(o));
    }
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::BundleTooLarge);
}

TEST_F(ShieldedValidationFixture, AtSizeLimitNotRejectedForSize) {
    // Exactly at limit (not over) — size check passes; the bundle
    // then fails *something else* (nullifier-set or proof check)
    // but specifically NOT BundleTooLarge.
    ShieldedBundle bundle;
    for (size_t i = 0; i < shielded::kMaxOutputsPerBundle; ++i) {
        ShieldedOutput o;
        o.commitment = MakeHash(static_cast<uint8_t>(i & 0xFF), 0xE3);
        o.commitment[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        o.encrypted_note = std::vector<uint8_t>(16, 0xBB);
        o.zk_proof       = std::vector<uint8_t>(8, 0xCC);
        bundle.outputs.push_back(std::move(o));
    }
    const auto err = ValidateShieldedBundle(bundle, ctx);
    EXPECT_NE(err, ShieldedValidationError::BundleTooLarge);
}

// ── Phase 3 wave 2 binding-sig tests live in pedersen_tests.cpp ─────
// (the cross-bundle balance test fixture there constructs a real cv +
// rcv + range proof + Schnorr binding sig; legacy SHA-256-tag tests
// were retired with the structural-tag mechanism in Wave 2.)

// ── Phase 3 wave 3: builder ↔ validator full-pipeline parity ────────
// BuildShieldedBundle assembles cv, range proofs, bsk, bvk_commitment,
// binding_sig from raw planned spends/outputs. These tests confirm the
// builder's output round-trips through the FULL ValidateShieldedBundle
// pipeline — including real Spartan ZK proof verification, anchor
// freshness, nullifier checks, value_balance arithmetic, AND the
// Wave 1B/Wave 2 cv path (which the existing HappyPathBundle fixture
// skips because it leaves aggregated_range_proof empty).

TEST_F(ShieldedValidationFixture, BuilderProducesValidatorAcceptedBundle) {
    // 1. Set up a single shielded note in the tree (the spend target).
    const Hash sk         = MakeHash(0x80, 0xF2);
    const Hash spend_val  = ValueAsHash(100'000'000);  // 1 DIN
    const Hash spend_rand = MakeHash(0x81, 0xF2);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash d          = MakeHash(0xA2, 0xC1);

    const Hash spend_cm = NoteCommitment(d, pk, spend_val, spend_rand);
    const uint64_t leaf_idx = tree.Append(spend_cm);

    // 2. Generate a real Spartan spend proof for the note.
    SpendWitness sw;
    sw.secret_key = sk;
    sw.leaf_index = leaf_idx;
    sw.value      = spend_val;
    sw.randomness = spend_rand;
    sw.d          = d;
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());
    sw.merkle_path = path->siblings;

    SpendPublicInputs spi;
    spi.nullifier = ComputeNullifier(sk, leaf_idx);
    spi.anchor    = tree.Root();
    auto spend_proof = ProveSpend(sw, spi, nullptr);
    ASSERT_FALSE(spend_proof.empty());

    // 3. Generate a real Spartan output proof (re-shield the same value
    //    to the same pk, fresh randomness).
    OutputWitness ow;
    ow.value      = spend_val;
    ow.public_key = pk;
    ow.randomness = MakeHash(0x82, 0xF2);
    ow.d          = d;
    OutputPublicInputs opi;
    opi.commitment = NoteCommitment(ow.d, ow.public_key, ow.value, ow.randomness);
    auto output_proof = ProveOutput(ow, opi, nullptr);
    ASSERT_FALSE(output_proof.empty());

    // 4. Hand the proofs to BuildShieldedBundle.
    shielded::PlannedSpend ps;
    ps.nullifier   = spi.nullifier;
    ps.anchor      = spi.anchor;
    ps.value_una   = 100'000'000;
    ps.rcv         = MakeHash(0x83, 0xF2);
    ps.spend_proof = std::move(spend_proof);
    ps.nonce       = MakeHash(0x84, 0xF2);

    shielded::PlannedOutput po;
    po.commitment     = opi.commitment;
    po.value_una      = 100'000'000;
    po.rcv            = MakeHash(0x85, 0xF2);
    po.encrypted_note = std::vector<uint8_t>(96, 0xAA);
    po.output_proof   = std::move(output_proof);
    po.nonce          = MakeHash(0x86, 0xF2);

    Hash tx_sighash = MakeHash(0x87, 0xF2);
    ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle({ps}, {po}, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);

    // 5. Validate against the FULL pipeline — every gate the consensus
    //    layer enforces, including binding sig + range proofs.
    ASSERT_TRUE(shielded::PedersenGeneratorsReady());
    ASSERT_FALSE(bundle.aggregated_range_proof.empty())
        << "builder did not populate aggregated_range_proof — "
        << "validator would silently skip Wave 1B/2 checks";

    ctx.transparent_value_delta = 0;  // pure transfer: vb=0 matches delta
    ctx.tx_sighash              = tx_sighash;
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::Ok);
}

TEST_F(ShieldedValidationFixture, BuilderTamperedSighashRejectedByValidator) {
    // Same setup, but pass a different tx_sighash to the validator —
    // wrap-attack guard must reject (BindingSigInvalid).
    const Hash sk         = MakeHash(0x90, 0xF3);
    const Hash spend_val  = ValueAsHash(100'000'000);
    const Hash spend_rand = MakeHash(0x91, 0xF3);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash d          = MakeHash(0xA2, 0xC1);

    const Hash spend_cm = NoteCommitment(d, pk, spend_val, spend_rand);
    const uint64_t leaf_idx = tree.Append(spend_cm);

    SpendWitness sw;
    sw.secret_key = sk;
    sw.leaf_index = leaf_idx;
    sw.value      = spend_val;
    sw.randomness = spend_rand;
    sw.d          = d;
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());
    sw.merkle_path = path->siblings;

    SpendPublicInputs spi;
    spi.nullifier = ComputeNullifier(sk, leaf_idx);
    spi.anchor    = tree.Root();
    auto spend_proof = ProveSpend(sw, spi, nullptr);
    ASSERT_FALSE(spend_proof.empty());

    OutputWitness ow;
    ow.value      = spend_val;
    ow.public_key = pk;
    ow.randomness = MakeHash(0x92, 0xF3);
    ow.d          = d;
    OutputPublicInputs opi;
    opi.commitment = NoteCommitment(ow.d, ow.public_key, ow.value, ow.randomness);
    auto output_proof = ProveOutput(ow, opi, nullptr);
    ASSERT_FALSE(output_proof.empty());

    shielded::PlannedSpend ps{spi.nullifier, spi.anchor, 100'000'000,
                              MakeHash(0x93, 0xF3),
                              std::move(spend_proof),
                              MakeHash(0x94, 0xF3)};
    shielded::PlannedOutput po{opi.commitment, 100'000'000,
                               MakeHash(0x95, 0xF3),
                               std::vector<uint8_t>(96, 0xAA),
                               std::move(output_proof),
                               MakeHash(0x96, 0xF3)};

    const Hash sighash_signed   = MakeHash(0x97, 0xF3);
    const Hash sighash_validate = MakeHash(0x98, 0xF3);  // ≠ signed
    ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle({ps}, {po}, sighash_signed, bundle),
              shielded::BundleBuildResult::Ok);

    ctx.transparent_value_delta = 0;
    ctx.tx_sighash              = sighash_validate;  // wrap attempt
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx),
              ShieldedValidationError::BindingSigInvalid);
}

// ── Phase 3 wave 3b: shield-side wallet helper ──────────────────────
//
// BuildShieldBundleForTx is the wallet-facing entry point for shield
// txs: given an unsigned transparent envelope, it generates a fresh
// note + Spartan output proof, calls BuildShieldedBundle, and attaches
// the serialized bundle to tx.shielded_bundle_bytes. These tests
// confirm the full chain: wallet helper → bundle bytes → deserialize →
// ValidateShieldedBundle returns Ok with the proper transparent_value_delta.

namespace {

dinero::Transaction MakeShieldEnvelope(uint64_t fee_una) {
    // Synthetic shielded envelope: one transparent input being consumed, no
    // transparent outputs (recipient is shielded), explicit fee. Witness
    // fields stay default — BIP143 sighash is invariant to shielded
    // bundle bytes, so the helper does not require a signed envelope.
    dinero::Transaction tx;
    tx.version = dinero::Transaction::TX_VERSION_SHIELDED;
    tx.lockTime = 0;

    dinero::TxInput in{};
    {
        uint256 txid_raw;
        std::memset(txid_raw.data, 0xAB, 32);
        in.prevout.txid = dinero::TxId(txid_raw);
    }
    in.prevout.vout = 0;
    in.sequence = 0xfffffffe;  // RBF-signaling
    tx.vin.push_back(std::move(in));

    tx.SetExplicitFee(fee_una);
    return tx;
}

}  // namespace

TEST_F(ShieldedValidationFixture, ShieldHelperProducesValidatorAcceptedTx) {
    constexpr uint64_t kShieldValue = 100'000'000;  // 1 DIN
    constexpr uint64_t kFee         = 10'000;

    auto tx = MakeShieldEnvelope(kFee);
    auto built = wallet::shielded_ops::BuildShieldBundleForTx(tx, kShieldValue);
    ASSERT_EQ(built.status, wallet::shielded_ops::OpStatus::Ok)
        << "build failed: " << built.error;
    EXPECT_FALSE(tx.shielded_bundle_bytes.empty());
    EXPECT_EQ(tx.shielded_bundle_bytes.size(), built.bundle_bytes);

    // Deserialize the freshly-attached bundle and run it through the
    // FULL consensus validator (anchor / nullifier / range / binding sig
    // / ZK proofs / value_balance).
    ShieldedBundle decoded;
    ASSERT_EQ(shielded::DeserializeShieldedBundle(tx.shielded_bundle_bytes,
                                                  &decoded),
              shielded::BundleDecodeError::Ok);
    EXPECT_EQ(decoded.spends.size(), 0u);
    EXPECT_EQ(decoded.outputs.size(), 1u);
    EXPECT_EQ(decoded.value_balance, static_cast<int64_t>(kShieldValue));
    EXPECT_EQ(decoded.outputs[0].commitment, built.commitment);

    // Validator's tx_sighash is computed over the same envelope the
    // helper signed against — bundle bytes are not in the sighash, so
    // pre-attach and post-attach sighashes are identical.
    ctx.transparent_value_delta = static_cast<int64_t>(kShieldValue);
    ctx.tx_sighash              = shielded::ComputeShieldedTxSighash(tx);
    EXPECT_EQ(ValidateShieldedBundle(decoded, ctx),
              ShieldedValidationError::Ok);
}

TEST_F(ShieldedValidationFixture, ShieldHelperRejectsZeroValue) {
    auto tx = MakeShieldEnvelope(0);
    auto built = wallet::shielded_ops::BuildShieldBundleForTx(tx, 0);
    EXPECT_EQ(built.status, wallet::shielded_ops::OpStatus::InvalidParams);
    EXPECT_TRUE(tx.shielded_bundle_bytes.empty());
}

TEST_F(ShieldedValidationFixture, ShieldHelperRejectsWrongVersion) {
    auto tx = MakeShieldEnvelope(0);
    tx.version = 2;  // not TX_VERSION_SHIELDED
    auto built = wallet::shielded_ops::BuildShieldBundleForTx(tx, 100);
    EXPECT_EQ(built.status, wallet::shielded_ops::OpStatus::InvalidParams);
    EXPECT_TRUE(tx.shielded_bundle_bytes.empty());
}

// ── Phase 3 wave 3c: unshield-side wallet helper ────────────────────

namespace {

// Build a synthetic v5 unshield envelope: empty vin, single transparent
// recipient vout for `recipient_value`, explicit_fee = `fee_una`.
dinero::Transaction MakeUnshieldEnvelope(uint64_t recipient_value,
                                         uint64_t fee_una,
                                         uint8_t  recipient_seed) {
    dinero::Transaction tx;
    tx.version  = dinero::Transaction::TX_VERSION_SHIELDED;
    tx.lockTime = 0;

    dinero::TxOutput out;
    out.value = dinero::AmountUna::Una(recipient_value);
    // 1-byte stub scriptPubKey — exact bytes vary by recipient_seed so the
    // envelope-binding test can produce two byte-distinct envelopes that
    // are otherwise identical.
    out.scriptPubKey = {0x00, recipient_seed};
    tx.vout.push_back(std::move(out));

    tx.SetExplicitFee(fee_una);
    return tx;
}

}  // namespace

TEST_F(ShieldedValidationFixture, UnshieldHelperProducesValidatorAcceptedTx) {
    constexpr uint64_t kNoteValue = 100'000'000;  // 1 DIN
    constexpr uint64_t kFee       = 10'000;
    constexpr uint64_t kRecipient = kNoteValue - kFee;

    // 1. Plant a note in the wallet-side tree.
    const Hash sk         = MakeHash(0xA0, 0xC1);
    const Hash randomness = MakeHash(0xA1, 0xC1);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    // M3 prover/iOS gate: real received notes carry a decrypted
    // diversifier. The unshield helper must prove against that exact
    // d-bound commitment, not the old zero-diversifier shortcut.
    const Hash d          = MakeHash(0xA2, 0xC1);
    const Hash value_hash = ValueAsHash(kNoteValue);

    const Hash spend_cm = NoteCommitment(d, pk, value_hash, randomness);
    const uint64_t leaf_idx = tree.Append(spend_cm);
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());

    // 2. Wire the pure-helper input.
    wallet::shielded_ops::UnshieldNoteInput note;
    note.secret_key  = sk;
    note.randomness  = randomness;
    note.d           = d;
    note.anchor      = tree.Root();
    note.leaf_index  = leaf_idx;
    note.value_una   = kNoteValue;
    note.merkle_path = path->siblings;

    // 3. Build envelope, attach bundle.
    auto tx = MakeUnshieldEnvelope(kRecipient, kFee, /*recipient_seed=*/0xAA);
    auto built = wallet::shielded_ops::BuildUnshieldBundleForTx(tx, note, kFee);
    ASSERT_EQ(built.status, wallet::shielded_ops::OpStatus::Ok)
        << "build failed: " << built.error;
    EXPECT_FALSE(tx.shielded_bundle_bytes.empty());
    EXPECT_EQ(tx.shielded_bundle_bytes.size(), built.bundle_bytes);

    // 4. Decode + run through full validator.
    ShieldedBundle decoded;
    ASSERT_EQ(shielded::DeserializeShieldedBundle(tx.shielded_bundle_bytes,
                                                  &decoded),
              shielded::BundleDecodeError::Ok);
    EXPECT_EQ(decoded.spends.size(), 1u);
    EXPECT_EQ(decoded.outputs.size(), 0u);
    EXPECT_EQ(decoded.value_balance, -static_cast<int64_t>(kNoteValue));
    EXPECT_EQ(decoded.spends[0].nullifier, built.nullifier);
    EXPECT_EQ(decoded.spends[0].anchor, built.anchor);

    // Validator's transparent_value_delta for this envelope:
    //   in (= 0) - out (= recipient) - fee = -(kRecipient + kFee) = -kNoteValue.
    ctx.transparent_value_delta = -static_cast<int64_t>(kNoteValue);
    ctx.tx_sighash              = shielded::ComputeShieldedTxSighash(tx);
    EXPECT_EQ(ValidateShieldedBundle(decoded, ctx),
              ShieldedValidationError::Ok);
}

// Issue #273 regression: the shielded RPC handlers used a fixed default
// fee (1000 una) chosen before the bundle attaches. Since v6 bundles
// count in BASE serialization, the final GetVirtualSize() — the
// mempool's fee-rate denominator — lands in the kilobytes, so the fixed
// default underpays DEFAULT_MIN_FEE_RATE (1 una/vbyte) and the mempool
// rejects. The fix measures the built tx and raises the fee to
// RequiredFeeForTx(); this test mirrors that two-pass decision logic
// against the real unshield bundle builder. Fails (final fee below the
// mempool floor) if the size-aware computation is dropped back to the
// fixed default.
TEST_F(ShieldedValidationFixture, SizeAwareFeeCoversMempoolFloorForUnshield) {
    constexpr uint64_t kNoteValue      = 100'000'000;  // 1 DIN
    constexpr uint64_t kProvisionalFee = 1000;  // old fixed RPC default
    constexpr double   kMinFeeRate     = 1.0;   // Mempool::DEFAULT_MIN_FEE_RATE

    // Plant a note in the wallet-side tree.
    const Hash sk         = MakeHash(0xA0, 0xF3);
    const Hash randomness = MakeHash(0xA1, 0xF3);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash d          = MakeHash(0xA2, 0xF3);
    const uint64_t leaf_idx =
        tree.Append(NoteCommitment(d, pk, ValueAsHash(kNoteValue), randomness));
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());

    wallet::shielded_ops::UnshieldNoteInput note;
    note.secret_key  = sk;
    note.randomness  = randomness;
    note.d           = d;
    note.anchor      = tree.Root();
    note.leaf_index  = leaf_idx;
    note.value_una   = kNoteValue;
    note.merkle_path = path->siblings;

    // The RPC builds v6 (bundle-committing) txs — the version where the
    // bundle counts in base serialization and blows up the vsize.
    auto make_envelope = [&](uint64_t fee) {
        auto tx = MakeUnshieldEnvelope(kNoteValue - fee, fee, /*seed=*/0xF3);
        tx.version = dinero::Transaction::TX_VERSION_SHIELDED_V2;
        tx.witness_version = 0;
        return tx;
    };

    // Pass 1 (probe): build with the legacy fixed default.
    auto probe = make_envelope(kProvisionalFee);
    auto probe_built =
        wallet::shielded_ops::BuildUnshieldBundleForTx(probe, note, kProvisionalFee);
    ASSERT_EQ(probe_built.status, wallet::shielded_ops::OpStatus::Ok)
        << "probe build failed: " << probe_built.error;

    const auto mempool_floor = [&](const dinero::Transaction& tx) {
        return static_cast<uint64_t>(
            std::ceil(kMinFeeRate * static_cast<double>(tx.GetVirtualSize())));
    };

    // The #273 bug, demonstrated: the fixed default underpays the floor.
    EXPECT_LT(kProvisionalFee, mempool_floor(probe))
        << "expected the v6 bundle to push vsize above the fixed default fee";

    // Decision logic (as the fixed RPC handlers run it): measure, raise.
    const uint64_t required =
        wallet::shielded_ops::RequiredFeeForTx(probe, kMinFeeRate);
    EXPECT_GE(required, mempool_floor(probe));
    const uint64_t final_fee = std::max<uint64_t>(kProvisionalFee, required);

    // Pass 2: rebuild once with the size-adequate fee. The explicit-fee
    // field is fixed-width, so the fee value must not move the vsize.
    auto tx = make_envelope(final_fee);
    auto built = wallet::shielded_ops::BuildUnshieldBundleForTx(tx, note, final_fee);
    ASSERT_EQ(built.status, wallet::shielded_ops::OpStatus::Ok)
        << "final build failed: " << built.error;

    // Regression gate: the final tx satisfies the mempool's criterion
    // fee / vsize >= min_rate.
    EXPECT_GE(final_fee, mempool_floor(tx))
        << "size-aware fee still underpays: fee=" << final_fee
        << " vsize=" << tx.GetVirtualSize();
}

TEST_F(ShieldedValidationFixture, UnshieldHelperRejectsFeeGteValue) {
    const Hash sk         = MakeHash(0xA0, 0xC2);
    const Hash randomness = MakeHash(0xA1, 0xC2);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const uint64_t leaf_idx = tree.Append(NoteCommitment(Hash{}, pk,
                                                          ValueAsHash(100), randomness));
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());

    wallet::shielded_ops::UnshieldNoteInput note;
    note.secret_key  = sk;
    note.randomness  = randomness;
    note.anchor      = tree.Root();
    note.leaf_index  = leaf_idx;
    note.value_una   = 100;
    note.merkle_path = path->siblings;

    // fee_una == note.value_una violates "fee strictly < value"
    auto tx = MakeUnshieldEnvelope(/*recipient_value=*/0, /*fee=*/100, /*seed=*/0x01);
    auto built = wallet::shielded_ops::BuildUnshieldBundleForTx(tx, note, 100);
    EXPECT_EQ(built.status, wallet::shielded_ops::OpStatus::InvalidParams);
    EXPECT_TRUE(tx.shielded_bundle_bytes.empty());
}

TEST_F(ShieldedValidationFixture, UnshieldHelperRejectsWrongVoutShape) {
    const Hash sk         = MakeHash(0xA0, 0xC3);
    const Hash randomness = MakeHash(0xA1, 0xC3);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash value_hash = ValueAsHash(100'000'000);
    const uint64_t leaf_idx = tree.Append(NoteCommitment(Hash{}, pk,
                                                          value_hash, randomness));
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());

    wallet::shielded_ops::UnshieldNoteInput note;
    note.secret_key  = sk;
    note.randomness  = randomness;
    note.anchor      = tree.Root();
    note.leaf_index  = leaf_idx;
    note.value_una   = 100'000'000;
    note.merkle_path = path->siblings;

    // Empty vout: builder should reject (unshield requires exactly 1).
    dinero::Transaction tx_empty;
    tx_empty.version = dinero::Transaction::TX_VERSION_SHIELDED;
    tx_empty.SetExplicitFee(10'000);
    auto rc_empty = wallet::shielded_ops::BuildUnshieldBundleForTx(tx_empty, note, 10'000);
    EXPECT_EQ(rc_empty.status, wallet::shielded_ops::OpStatus::InvalidParams);

    // vout with wrong amount: should reject.
    auto tx_wrong = MakeUnshieldEnvelope(/*recipient_value=*/12345,
                                         /*fee=*/10'000, /*seed=*/0x02);
    auto rc_wrong = wallet::shielded_ops::BuildUnshieldBundleForTx(tx_wrong, note, 10'000);
    EXPECT_EQ(rc_wrong.status, wallet::shielded_ops::OpStatus::InvalidParams);
    EXPECT_TRUE(tx_wrong.shielded_bundle_bytes.empty());
}

// THE critical test: bundle bound to envelope_a must NOT validate against
// envelope_b (different transparent recipient). Closes the dangerous
// "proof valid but pays someone else" attack class.
TEST_F(ShieldedValidationFixture, UnshieldHelperBindsToTransparentEnvelope) {
    constexpr uint64_t kNoteValue = 100'000'000;
    constexpr uint64_t kFee       = 10'000;
    constexpr uint64_t kRecipient = kNoteValue - kFee;

    const Hash sk         = MakeHash(0xB0, 0xD1);
    const Hash randomness = MakeHash(0xB1, 0xD1);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash value_hash = ValueAsHash(kNoteValue);
    const uint64_t leaf_idx = tree.Append(NoteCommitment(Hash{}, pk,
                                                          value_hash, randomness));
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());

    wallet::shielded_ops::UnshieldNoteInput note;
    note.secret_key  = sk;
    note.randomness  = randomness;
    note.anchor      = tree.Root();
    note.leaf_index  = leaf_idx;
    note.value_una   = kNoteValue;
    note.merkle_path = path->siblings;

    // Envelope A: legitimate recipient.
    auto tx_a = MakeUnshieldEnvelope(kRecipient, kFee, /*seed=*/0x55);
    auto built_a = wallet::shielded_ops::BuildUnshieldBundleForTx(tx_a, note, kFee);
    ASSERT_EQ(built_a.status, wallet::shielded_ops::OpStatus::Ok);

    // Envelope B: same value + fee, DIFFERENT scriptPubKey (recipient swap).
    // This is the attack scenario: someone splices the bundle bytes into a
    // tx that pays a different address.
    auto tx_b = MakeUnshieldEnvelope(kRecipient, kFee, /*seed=*/0x66);
    ASSERT_NE(tx_a.vout[0].scriptPubKey, tx_b.vout[0].scriptPubKey);

    ShieldedBundle decoded;
    ASSERT_EQ(shielded::DeserializeShieldedBundle(tx_a.shielded_bundle_bytes,
                                                  &decoded),
              shielded::BundleDecodeError::Ok);

    // Validator with envelope A's sighash → Ok.
    ctx.transparent_value_delta = -static_cast<int64_t>(kNoteValue);
    ctx.tx_sighash              = shielded::ComputeShieldedTxSighash(tx_a);
    EXPECT_EQ(ValidateShieldedBundle(decoded, ctx),
              ShieldedValidationError::Ok);

    // Validator with envelope B's sighash → BindingSigInvalid.
    // Same bundle bytes, different transparent recipient: the binding
    // signature was computed over A's vout and refuses to verify against B's.
    ctx.tx_sighash = shielded::ComputeShieldedTxSighash(tx_b);
    EXPECT_EQ(ValidateShieldedBundle(decoded, ctx),
              ShieldedValidationError::BindingSigInvalid)
        << "CRITICAL: bundle splice across recipients was accepted — "
        << "the binding sig is not envelope-bound for unshield";
}

TEST_F(ShieldedValidationFixture, ShieldHelperBindsToTransparentEnvelope) {
    constexpr uint64_t kShieldValue = 100'000'000;

    auto tx_a = MakeShieldEnvelope(10'000);
    auto built_a = wallet::shielded_ops::BuildShieldBundleForTx(tx_a, kShieldValue);
    ASSERT_EQ(built_a.status, wallet::shielded_ops::OpStatus::Ok);

    // Build a second envelope with a DIFFERENT prevout. Bundle from tx_a
    // must NOT validate against tx_b's sighash — wrap-attack guard.
    auto tx_b = MakeShieldEnvelope(10'000);
    tx_b.vin[0].prevout.vout = 7;  // different prevout

    ShieldedBundle decoded_a;
    ASSERT_EQ(shielded::DeserializeShieldedBundle(tx_a.shielded_bundle_bytes,
                                                  &decoded_a),
              shielded::BundleDecodeError::Ok);

    ctx.transparent_value_delta = static_cast<int64_t>(kShieldValue);
    ctx.tx_sighash              = shielded::ComputeShieldedTxSighash(tx_a);
    EXPECT_EQ(ValidateShieldedBundle(decoded_a, ctx),
              ShieldedValidationError::Ok);

    ctx.tx_sighash = shielded::ComputeShieldedTxSighash(tx_b);
    EXPECT_EQ(ValidateShieldedBundle(decoded_a, ctx),
              ShieldedValidationError::BindingSigInvalid);
}

// ── Apply: state mutation contract ──────────────────────────────────

TEST_F(ShieldedValidationFixture, ApplyAppendsCommitmentsAndNullifiers) {
    ShieldedBundle bundle;
    bundle.outputs.push_back(MakeOutputStub(0xB1));
    bundle.outputs.push_back(MakeOutputStub(0xB2));
    bundle.spends.push_back(MakeSpendStub(0xC1, tree.Root()));

    const uint64_t size_before = tree.Size();
    const auto root_before = tree.Root();
    EXPECT_EQ(nullifier_set.Size(), 0u);

    ApplyShieldedBundle(bundle, &tree, &nullifier_set, 200);

    EXPECT_EQ(tree.Size(), size_before + 2);
    EXPECT_NE(tree.Root(), root_before);
    EXPECT_TRUE(nullifier_set.Contains(MakeHash(0xC1)));
    EXPECT_EQ(nullifier_set.Size(), 1u);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
