// Copyright (c) 2026 Dinero Labs.
//
// Phase 3 wave 1B (Path C) — Pedersen value-commitment tests.
//
// Pins:
//   - Generator V is deterministically derived from kPedersenVDST
//     (idempotent across calls, ready flag = true).
//   - PedersenCommit produces consistent cv bytes for the same
//     (blind, value); different blinds or values flip the bytes.
//   - PedersenCommit is additively homomorphic in value:
//     commit(b1, v1) + commit(b2, v2) = commit(b1+b2, v1+v2)
//     (this is the property that lets Wave 2's bvk equation work).

#include <gtest/gtest.h>

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/range_proof.h"
#include "consensus/shielded/shielded_tx.h"

#include "crypto/evp_secp256k1.h"

#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>
#include <secp256k1_schnorrsig.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::Hash;
using shielded::PedersenCommit;
using shielded::PedersenGeneratorV;
using shielded::PedersenGeneratorsReady;
using shielded::PedersenResult;

Hash MakeBlind(uint8_t seed) {
    Hash h{};
    // Place the seed in the low byte of a small scalar so we stay
    // safely below the curve order. (Real wallet code uses CSPRNG.)
    h[31] = seed;
    return h;
}

TEST(PedersenGeneratorTest, ReadyAfterFirstCall) {
    // Trigger lazy derivation.
    const Hash& v = PedersenGeneratorV();
    EXPECT_TRUE(PedersenGeneratorsReady());
    // Generator is non-trivial — must not be all zeros.
    bool any_nonzero = false;
    for (uint8_t b : v) {
        if (b != 0) {
            any_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(any_nonzero);
}

TEST(PedersenGeneratorTest, IdempotentAcrossCalls) {
    const Hash& v1 = PedersenGeneratorV();
    const Hash& v2 = PedersenGeneratorV();
    EXPECT_EQ(v1, v2);
    // Same memory address — proves the static cache, not a re-derive.
    EXPECT_EQ(&v1, &v2);
}

TEST(PedersenCommitTest, CommitDeterministic) {
    Hash blind = MakeBlind(0x42);
    shielded::ValueCommitment cv_a{};
    shielded::ValueCommitment cv_b{};
    EXPECT_EQ(PedersenCommit(blind, 1'000'000, cv_a), PedersenResult::Ok);
    EXPECT_EQ(PedersenCommit(blind, 1'000'000, cv_b), PedersenResult::Ok);
    EXPECT_EQ(cv_a, cv_b);
}

TEST(PedersenCommitTest, DifferentValueFlipsCv) {
    Hash blind = MakeBlind(0x10);
    shielded::ValueCommitment cv_low{};
    shielded::ValueCommitment cv_high{};
    EXPECT_EQ(PedersenCommit(blind, 1, cv_low), PedersenResult::Ok);
    EXPECT_EQ(PedersenCommit(blind, 2, cv_high), PedersenResult::Ok);
    EXPECT_NE(cv_low, cv_high);
}

TEST(PedersenCommitTest, DifferentBlindFlipsCv) {
    Hash b1 = MakeBlind(0x01);
    Hash b2 = MakeBlind(0x02);
    shielded::ValueCommitment cv1{};
    shielded::ValueCommitment cv2{};
    EXPECT_EQ(PedersenCommit(b1, 100, cv1), PedersenResult::Ok);
    EXPECT_EQ(PedersenCommit(b2, 100, cv2), PedersenResult::Ok);
    EXPECT_NE(cv1, cv2);
}

TEST(PedersenCommitTest, BalanceEquationViaVerifyTally) {
    // The Wave 2 bvk equation relies on Pedersen homomorphism:
    //   commit(b1, v1) + commit(b2, v2) - commit(b1+b2, v1+v2) == 0
    // libsecp256k1-zkp exposes this via `secp256k1_pedersen_verify_tally`,
    // which is exactly the function consensus will call to verify cv
    // sums against `value_balance · V` once Wave 2 lands. This test
    // pins that the property holds with our actual generator V.
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    ASSERT_NE(ctx, nullptr);

    Hash b1 = MakeBlind(0x77);
    Hash b2 = MakeBlind(0x88);
    const uint64_t v1 = 100;
    const uint64_t v2 = 250;

    // Re-derive V generator from canonical DST.
    unsigned char seed[32];
    constexpr const char* kDst = "DIN/v7/shielded/cv/V/v1";
    SHA256(reinterpret_cast<const unsigned char*>(kDst),
           std::strlen(kDst), seed);
    secp256k1_generator gen_v{};
    ASSERT_TRUE(secp256k1_generator_generate(ctx, &gen_v, seed));

    // Two commitments on the "positive" side.
    secp256k1_pedersen_commitment c1{};
    secp256k1_pedersen_commitment c2{};
    ASSERT_TRUE(secp256k1_pedersen_commit(ctx, &c1, b1.data(), v1, &gen_v));
    ASSERT_TRUE(secp256k1_pedersen_commit(ctx, &c2, b2.data(), v2, &gen_v));

    // One commitment on the "negative" side at (b1+b2, v1+v2).
    Hash blind_sum{};
    const unsigned char* blinds[2] = {b1.data(), b2.data()};
    ASSERT_TRUE(secp256k1_pedersen_blind_sum(ctx, blind_sum.data(),
                                             blinds, 2, 2));
    secp256k1_pedersen_commitment c3{};
    ASSERT_TRUE(secp256k1_pedersen_commit(ctx, &c3, blind_sum.data(),
                                          v1 + v2, &gen_v));

    // verify_tally: pos = {c1, c2}, neg = {c3}. Should sum to 0.
    const secp256k1_pedersen_commitment* pos[2] = {&c1, &c2};
    const secp256k1_pedersen_commitment* neg[1] = {&c3};
    EXPECT_TRUE(secp256k1_pedersen_verify_tally(ctx, pos, 2, neg, 1))
        << "Pedersen commitments are NOT additively homomorphic — Wave 2 "
           "bvk reconstruction depends on this";

    // Sanity: tampering one of the values breaks the tally.
    secp256k1_pedersen_commitment c3_wrong{};
    ASSERT_TRUE(secp256k1_pedersen_commit(ctx, &c3_wrong, blind_sum.data(),
                                          v1 + v2 + 1, &gen_v));
    const secp256k1_pedersen_commitment* neg_wrong[1] = {&c3_wrong};
    EXPECT_FALSE(secp256k1_pedersen_verify_tally(ctx, pos, 2, neg_wrong, 1));
}

// ── Range proofs (Wave 1B-B) ────────────────────────────────────────

TEST(RangeProofTest, SignAndVerifyHonestBundle) {
    // Build a one-output bundle, sign a range proof for that cv,
    // package it, and verify.
    Hash blind = MakeBlind(0x10);
    Hash nonce = MakeBlind(0x99);
    nonce[0] = 0xAB;  // distinct from blind to avoid same-byte collision
    shielded::ValueCommitment cv{};
    ASSERT_EQ(PedersenCommit(blind, 1'000'000, cv), PedersenResult::Ok);

    std::vector<uint8_t> proof_bytes;
    ASSERT_EQ(SignRangeProof(blind, nonce, 1'000'000, proof_bytes),
              shielded::RangeProofResult::Ok);
    ASSERT_FALSE(proof_bytes.empty());

    shielded::ShieldedBundle bundle;
    shielded::ShieldedOutput o{};
    o.cv = cv;
    o.zk_proof.push_back(0x01);  // structurally non-empty
    bundle.outputs.push_back(o);
    bundle.aggregated_range_proof =
        shielded::EncodeAggregatedRangeProof({proof_bytes});

    EXPECT_EQ(shielded::VerifyBundleRangeProofs(bundle),
              shielded::RangeProofResult::Ok);
}

TEST(RangeProofTest, TamperedProofRejected) {
    Hash blind = MakeBlind(0x20);
    Hash nonce = MakeBlind(0xCC);
    nonce[0] = 0xDD;
    shielded::ValueCommitment cv{};
    ASSERT_EQ(PedersenCommit(blind, 100, cv), PedersenResult::Ok);

    std::vector<uint8_t> proof_bytes;
    ASSERT_EQ(SignRangeProof(blind, nonce, 100, proof_bytes),
              shielded::RangeProofResult::Ok);
    proof_bytes[10] ^= 0xFF;  // corrupt

    shielded::ShieldedBundle bundle;
    shielded::ShieldedOutput o{};
    o.cv = cv;
    o.zk_proof.push_back(0x01);
    bundle.outputs.push_back(o);
    bundle.aggregated_range_proof =
        shielded::EncodeAggregatedRangeProof({proof_bytes});

    EXPECT_EQ(shielded::VerifyBundleRangeProofs(bundle),
              shielded::RangeProofResult::VerifyFailed);
}

TEST(RangeProofTest, TamperedCvRejected) {
    Hash blind = MakeBlind(0x30);
    Hash nonce = MakeBlind(0xEE);
    nonce[0] = 0xFF;
    shielded::ValueCommitment cv{};
    ASSERT_EQ(PedersenCommit(blind, 50, cv), PedersenResult::Ok);

    std::vector<uint8_t> proof_bytes;
    ASSERT_EQ(SignRangeProof(blind, nonce, 50, proof_bytes),
              shielded::RangeProofResult::Ok);

    shielded::ShieldedBundle bundle;
    shielded::ShieldedOutput o{};
    o.cv = cv;
    o.cv[5] ^= 0x40;  // tamper cv after signing
    o.zk_proof.push_back(0x01);
    bundle.outputs.push_back(o);
    bundle.aggregated_range_proof =
        shielded::EncodeAggregatedRangeProof({proof_bytes});

    auto rc = shielded::VerifyBundleRangeProofs(bundle);
    EXPECT_TRUE(rc == shielded::RangeProofResult::VerifyFailed ||
                rc == shielded::RangeProofResult::CommitmentInvalid)
        << "Expected verify or parse failure on tampered cv, got "
        << static_cast<int>(rc);
}

TEST(RangeProofTest, CountMismatchRejected) {
    Hash blind = MakeBlind(0x40);
    Hash nonce = MakeBlind(0x44);
    nonce[0] = 0x55;
    shielded::ValueCommitment cv{};
    ASSERT_EQ(PedersenCommit(blind, 10, cv), PedersenResult::Ok);

    std::vector<uint8_t> proof_bytes;
    ASSERT_EQ(SignRangeProof(blind, nonce, 10, proof_bytes),
              shielded::RangeProofResult::Ok);

    shielded::ShieldedBundle bundle;
    shielded::ShieldedOutput o{};
    o.cv = cv;
    o.zk_proof.push_back(0x01);
    bundle.outputs.push_back(o);
    // Encode TWO proofs but the bundle only has ONE cv → count mismatch.
    bundle.aggregated_range_proof =
        shielded::EncodeAggregatedRangeProof({proof_bytes, proof_bytes});

    EXPECT_EQ(shielded::VerifyBundleRangeProofs(bundle),
              shielded::RangeProofResult::CountMismatch);
}

TEST(RangeProofTest, EmptyAggregatedBlobOnNonEmptyBundleRejected) {
    Hash blind = MakeBlind(0x50);
    shielded::ValueCommitment cv{};
    ASSERT_EQ(PedersenCommit(blind, 1, cv), PedersenResult::Ok);

    shielded::ShieldedBundle bundle;
    shielded::ShieldedOutput o{};
    o.cv = cv;
    o.zk_proof.push_back(0x01);
    bundle.outputs.push_back(o);
    // No range proof attached.
    bundle.aggregated_range_proof.clear();

    auto rc = shielded::VerifyBundleRangeProofs(bundle);
    EXPECT_TRUE(rc == shielded::RangeProofResult::ParseError ||
                rc == shielded::RangeProofResult::CountMismatch);
}

// ── Wave 2: Schnorr binding sig + cross-bundle balance ──────────────

namespace {

// Build a balanced one-spend / one-output bundle with real cv,
// real range proofs, and a real Schnorr binding sig. The spend
// commits to `spend_value`, the output commits to `output_value`,
// and `value_balance = spend_value - output_value`. Honest sender:
// chooses fresh blinds, computes bsk = blind_spend - blind_output,
// signs the bundle's canonical binding sighash with bsk.
shielded::ShieldedBundle BuildBalancedBundle(uint64_t spend_value,
                                             uint64_t output_value) {
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    EXPECT_NE(ctx, nullptr);

    Hash blind_spend = MakeBlind(0xAA);
    Hash blind_output = MakeBlind(0xBB);
    Hash nonce_spend = MakeBlind(0xCC);
    Hash nonce_output = MakeBlind(0xDD);

    shielded::ValueCommitment cv_spend{};
    shielded::ValueCommitment cv_output{};
    EXPECT_EQ(PedersenCommit(blind_spend, spend_value, cv_spend),
              PedersenResult::Ok);
    EXPECT_EQ(PedersenCommit(blind_output, output_value, cv_output),
              PedersenResult::Ok);

    std::vector<uint8_t> rp_spend;
    std::vector<uint8_t> rp_output;
    EXPECT_EQ(SignRangeProof(blind_spend, nonce_spend, spend_value, rp_spend),
              shielded::RangeProofResult::Ok);
    EXPECT_EQ(SignRangeProof(blind_output, nonce_output, output_value, rp_output),
              shielded::RangeProofResult::Ok);

    shielded::ShieldedBundle bundle;
    // Dinero convention: value_balance = sum(output) - sum(spend) =
    // transparent_in - transparent_out - fee.
    bundle.value_balance = static_cast<int64_t>(output_value) -
                           static_cast<int64_t>(spend_value);

    shielded::ShieldedSpend s{};
    s.nullifier = MakeBlind(0x01);
    s.nullifier[0] = 0xAA;
    s.anchor.fill(0);  // verified separately by anchor-history check
    s.cv = cv_spend;
    s.zk_proof.push_back(0x01);  // structural placeholder
    bundle.spends.push_back(s);

    shielded::ShieldedOutput o{};
    o.commitment = MakeBlind(0x02);
    o.commitment[0] = 0xBB;
    o.cv = cv_output;
    o.zk_proof.push_back(0x01);
    bundle.outputs.push_back(o);

    // Range proofs in canonical order (spends first, then outputs).
    bundle.aggregated_range_proof =
        shielded::EncodeAggregatedRangeProof({rp_spend, rp_output});

    // bsk = blind_spend - blind_output (mod q).
    Hash bsk{};
    const unsigned char* blinds[2] = {blind_spend.data(), blind_output.data()};
    EXPECT_TRUE(secp256k1_pedersen_blind_sum(ctx, bsk.data(),
                                             blinds, 2, /*npositive=*/1));

    // Compute and publish bvk_commitment = bsk·G in pedersen format.
    EXPECT_EQ(shielded::ComputeBvkCommitment(bsk, bundle.bvk_commitment),
              shielded::BindingSigResult::Ok);

    // Sign the canonical binding sighash for this bundle.
    Hash tx_sighash{};  // zero — wrap-attack disabled for tests
    Hash sighash = shielded::ComputeBindingSighash(bundle, tx_sighash);
    EXPECT_EQ(shielded::SignBinding(bsk, sighash, bundle.binding_sig),
              shielded::BindingSigResult::Ok);
    return bundle;
}

}  // namespace

TEST(BindingSigTest, HonestBalancedBundleVerifies) {
    auto bundle = BuildBalancedBundle(100, 100);
    Hash tx_sighash{};
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::Ok);
}

TEST(BindingSigTest, MutatedValueBalanceRejected) {
    auto bundle = BuildBalancedBundle(100, 100);
    bundle.value_balance += 1;  // attacker tries to claim 1 una out of nowhere
    Hash tx_sighash{};
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::SignatureInvalid);
}

TEST(BindingSigTest, MutatedCvRejected) {
    auto bundle = BuildBalancedBundle(100, 100);
    bundle.outputs[0].cv[3] ^= 0x10;  // attacker swaps an output cv
    Hash tx_sighash{};
    auto rc = shielded::VerifyBinding(bundle, tx_sighash);
    EXPECT_TRUE(rc == shielded::BindingSigResult::SignatureInvalid ||
                rc == shielded::BindingSigResult::CommitmentInvalid)
        << "expected verify failure on mutated cv, got " << static_cast<int>(rc);
}

TEST(BindingSigTest, MutatedSighashViaTxRejected) {
    auto bundle = BuildBalancedBundle(50, 50);
    Hash bad_sighash{};
    bad_sighash[0] = 0xFF;  // pretend the bundle is wrapped in a
                            // different transparent envelope (wrap-attack).
    EXPECT_EQ(shielded::VerifyBinding(bundle, bad_sighash),
              shielded::BindingSigResult::SignatureInvalid);
}

TEST(BindingSigTest, NetShieldVerifies) {
    // value_balance = +50 (Dinero convention): transparent flows IN,
    // 50 una shielded output created. No spends.
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    Hash blind_output = MakeBlind(0x91);
    Hash nonce_output = MakeBlind(0x92);
    shielded::ValueCommitment cv_output{};
    ASSERT_EQ(PedersenCommit(blind_output, 50, cv_output), PedersenResult::Ok);

    std::vector<uint8_t> rp_output;
    ASSERT_EQ(SignRangeProof(blind_output, nonce_output, 50, rp_output),
              shielded::RangeProofResult::Ok);

    shielded::ShieldedBundle bundle;
    bundle.value_balance = +50;  // Dinero convention: shield = positive
    shielded::ShieldedOutput o{};
    o.commitment = MakeBlind(0x77);
    o.cv = cv_output;
    o.zk_proof.push_back(0x01);
    bundle.outputs.push_back(o);
    bundle.aggregated_range_proof =
        shielded::EncodeAggregatedRangeProof({rp_output});

    // No spends → bsk = -blind_output.
    Hash bsk{};
    const unsigned char* blinds[1] = {blind_output.data()};
    ASSERT_TRUE(secp256k1_pedersen_blind_sum(ctx, bsk.data(),
                                             blinds, 1, /*npositive=*/0));
    ASSERT_EQ(shielded::ComputeBvkCommitment(bsk, bundle.bvk_commitment),
              shielded::BindingSigResult::Ok);
    Hash tx_sighash{};
    Hash sighash = shielded::ComputeBindingSighash(bundle, tx_sighash);
    ASSERT_EQ(shielded::SignBinding(bsk, sighash, bundle.binding_sig),
              shielded::BindingSigResult::Ok);

    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::Ok);
}

TEST(BindingSigTest, UnbalancedBundleRejected) {
    // **Inflation attack** simulation: spend 100, output 200, claim
    // value_balance = -100 (try to mint 100 from nowhere). bvk
    // reconstruction won't yield a key the attacker can sign for.
    auto bundle = BuildBalancedBundle(100, 200);
    // BuildBalancedBundle sets value_balance correctly to -100 above,
    // so the bundle is honestly *unbalanced for value flow* but the
    // binding sig should still verify (since the math IS balanced
    // when value_balance accurately reports the difference). Now we
    // simulate the actual attack: lie about value_balance.
    bundle.value_balance = 0;  // claim "pure transfer" while really minting 100
    // Re-sign over the lying sighash with the original bsk and
    // re-publish bvk to match. The pedersen_verify_tally rejects:
    // sum(cv_spend) - sum(cv_output) - 0·V - bvk_commit
    //   = (b1·G + 100·V) - (b2·G + 200·V) - bvk_commit
    //   = (b1 - b2)·G - 100·V - bvk_commit
    // bvk_commit = (b1-b2)·G alone, so the V-component leaves a
    // -100·V residue and verify_tally returns 0.
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    Hash blind_spend = MakeBlind(0xAA);
    Hash blind_output = MakeBlind(0xBB);
    Hash bsk{};
    const unsigned char* blinds[2] = {blind_spend.data(), blind_output.data()};
    ASSERT_TRUE(secp256k1_pedersen_blind_sum(ctx, bsk.data(),
                                             blinds, 2, /*npositive=*/1));
    ASSERT_EQ(shielded::ComputeBvkCommitment(bsk, bundle.bvk_commitment),
              shielded::BindingSigResult::Ok);
    Hash tx_sighash{};
    Hash sighash = shielded::ComputeBindingSighash(bundle, tx_sighash);
    ASSERT_EQ(shielded::SignBinding(bsk, sighash, bundle.binding_sig),
              shielded::BindingSigResult::Ok);

    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::SignatureInvalid);
}

// The Pedersen tally is NOT the balance check.
//
// `bvk_commitment` is attacker-supplied wire data, and the tally merely
// DEFINES what it has to equal:
//
//     bvk = sum(cv_spend) - sum(cv_output) + value_balance*V
//
// so an adaptive attacker satisfies it for ANY claimed value_balance simply
// by choosing bvk to match. As a constraint on value it is vacuous.
//
// What actually provides soundness is the BIP340 Schnorr verify. Producing a
// signature requires knowing dlog_G(bvk), and expanding cv = r*G + v*V gives
//
//     bvk = (dr)*G + (sum(v_spend) - sum(v_out) + value_balance)*V
//
// Because V is NUMS-derived with unknown dlog_G(V), a signature can only
// exist when the V-coefficient is zero — i.e. when value_balance is truthful.
// That single line is the entire cross-bundle inflation barrier.
//
// It had no adversarial coverage. UnbalancedBundleRejected above publishes
// the HONEST bvk, so the V-residue makes the tally fail and it returns before
// the Schnorr ever runs — its own comment says so. Both failures return the
// same BindingSigResult::SignatureInvalid, so the return value alone cannot
// distinguish which gate fired. This test therefore verifies the tally
// INDEPENDENTLY, proving the forgery gets past it, and only then asserts
// VerifyBinding still rejects — pinning the rejection on the Schnorr.
TEST(BindingSigTest, AdaptiveBvkPassesTallyButSchnorrStillRejects) {
    ASSERT_TRUE(PedersenGeneratorsReady());
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    ASSERT_NE(ctx, nullptr);

    // Spend 200, create 100: the honest value_balance is 100 - 200 = -100
    // (unshield 100 to transparent). The attacker instead claims 0 — "pure
    // shielded transfer" — while still moving 100 out, i.e. minting 100.
    constexpr uint64_t kSpendValue  = 200;
    constexpr uint64_t kOutputValue = 100;
    constexpr uint64_t kResidue     = kSpendValue - kOutputValue;  // 100

    Hash blind_spend  = MakeBlind(0xAA);
    Hash blind_output = MakeBlind(0xBB);

    shielded::ValueCommitment cv_spend{};
    shielded::ValueCommitment cv_output{};
    ASSERT_EQ(PedersenCommit(blind_spend, kSpendValue, cv_spend), PedersenResult::Ok);
    ASSERT_EQ(PedersenCommit(blind_output, kOutputValue, cv_output), PedersenResult::Ok);

    // bsk = blind_spend - blind_output (the honest blind difference, which the
    // attacker legitimately knows — they chose both blinds).
    Hash bsk{};
    const unsigned char* blinds[2] = {blind_spend.data(), blind_output.data()};
    ASSERT_TRUE(secp256k1_pedersen_blind_sum(ctx, bsk.data(), blinds, 2,
                                             /*npositive=*/1));

    // THE FORGERY. With the lie value_balance' = 0 the tally demands
    //   bvk' = sum(cv_spend) - sum(cv_output) + 0*V
    //        = (blind_spend - blind_output)*G + (200 - 100)*V
    //        = PedersenCommit(bsk, 100)
    // Note the 100*V term: bvk' is NOT bsk*G, so its discrete log base G is
    // unknown to the attacker. That is precisely what the Schnorr will catch.
    shielded::ValueCommitment bvk_forged{};
    ASSERT_EQ(PedersenCommit(bsk, kResidue, bvk_forged), PedersenResult::Ok);

    shielded::ShieldedBundle bundle;
    bundle.value_balance = 0;  // the lie (honest value would be -100)
    shielded::ShieldedSpend s{};
    s.nullifier = MakeBlind(0x01);
    s.anchor    = MakeBlind(0x02);
    s.cv        = cv_spend;
    s.zk_proof.push_back(0x01);
    bundle.spends.push_back(s);
    shielded::ShieldedOutput o{};
    o.commitment = MakeBlind(0x03);
    o.cv         = cv_output;
    o.zk_proof.push_back(0x01);
    bundle.outputs.push_back(o);
    bundle.bvk_commitment = bvk_forged;

    // Attacker signs with the best key they actually hold.
    Hash tx_sighash{};
    Hash sighash = shielded::ComputeBindingSighash(bundle, tx_sighash);
    ASSERT_EQ(shielded::SignBinding(bsk, sighash, bundle.binding_sig),
              shielded::BindingSigResult::Ok);

    // ── Step 1: the tally ACCEPTS the forgery ──────────────────────────
    // Replicated exactly as VerifyBinding builds it: pos = {cv_spend},
    // neg = {cv_output, bvk_commitment}, and no C_vb because vb == 0.
    {
        secp256k1_pedersen_commitment c_spend{}, c_output{}, c_bvk{};
        ASSERT_TRUE(secp256k1_pedersen_commitment_parse(ctx, &c_spend, cv_spend.data()));
        ASSERT_TRUE(secp256k1_pedersen_commitment_parse(ctx, &c_output, cv_output.data()));
        ASSERT_TRUE(secp256k1_pedersen_commitment_parse(ctx, &c_bvk, bvk_forged.data()));
        const secp256k1_pedersen_commitment* pos[1] = {&c_spend};
        const secp256k1_pedersen_commitment* neg[2] = {&c_output, &c_bvk};
        EXPECT_EQ(secp256k1_pedersen_verify_tally(ctx, pos, 1, neg, 2), 1)
            << "the tally must ACCEPT an adaptively-chosen bvk — if this fails "
               "the test is not reaching the Schnorr and proves nothing";
    }

    // ── Step 2: VerifyBinding rejects anyway ───────────────────────────
    // The tally passed, so this rejection can only have come from the
    // Schnorr: no signature exists for a bvk carrying a 100*V component.
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::SignatureInvalid)
        << "SOUNDNESS: an adaptive bvk that satisfies the Pedersen tally must "
           "still be rejected by the binding signature — this is the only "
           "barrier against cross-bundle inflation";
}

// ─── BundleBuilder round-trip tests ─────────────────────────────────────
// BuildShieldedBundle is the wallet-side helper that orchestrates
// every Wave 1B/Wave 2 primitive (cv, range proof, bsk, bvk, binding
// sig) from PlannedSpend / PlannedOutput inputs. These tests confirm
// that the builder's output round-trips through VerifyBundleRangeProofs
// and VerifyBinding without any further hand-massaging.

namespace {
shielded::PlannedSpend MakeSpend(uint8_t seed, uint64_t value) {
    shielded::PlannedSpend s{};
    s.nullifier = MakeBlind(seed);
    s.anchor    = MakeBlind(static_cast<uint8_t>(seed ^ 0xA0));
    s.value_una = value;
    s.rcv       = MakeBlind(static_cast<uint8_t>(seed ^ 0x10));
    s.nonce     = MakeBlind(static_cast<uint8_t>(seed ^ 0x20));
    return s;
}

shielded::PlannedOutput MakeOutput(uint8_t seed, uint64_t value) {
    shielded::PlannedOutput o{};
    o.commitment = MakeBlind(static_cast<uint8_t>(seed ^ 0x40));
    o.value_una  = value;
    o.rcv        = MakeBlind(static_cast<uint8_t>(seed ^ 0x50));
    o.nonce      = MakeBlind(static_cast<uint8_t>(seed ^ 0x60));
    return o;
}
}  // namespace

TEST(BundleBuilderTest, BalancedRoundTrip) {
    std::vector<shielded::PlannedSpend>  spends  = {MakeSpend(0x01, 100)};
    std::vector<shielded::PlannedOutput> outputs = {MakeOutput(0x02, 100)};
    Hash tx_sighash = MakeBlind(0xCC);

    shielded::ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle(spends, outputs, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);
    EXPECT_EQ(bundle.spends.size(), 1u);
    EXPECT_EQ(bundle.outputs.size(), 1u);
    EXPECT_EQ(bundle.value_balance, 0);
    EXPECT_EQ(shielded::VerifyBundleRangeProofs(bundle),
              shielded::RangeProofResult::Ok);
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::Ok);
}

TEST(BundleBuilderTest, NetShieldRoundTrip) {
    // value_balance = sum(out) - sum(spend) = +50 (shield).
    std::vector<shielded::PlannedSpend>  spends  = {MakeSpend(0x11, 50)};
    std::vector<shielded::PlannedOutput> outputs = {MakeOutput(0x12, 100)};
    Hash tx_sighash = MakeBlind(0xDD);

    shielded::ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle(spends, outputs, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);
    EXPECT_EQ(bundle.value_balance, 50);
    EXPECT_EQ(shielded::VerifyBundleRangeProofs(bundle),
              shielded::RangeProofResult::Ok);
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::Ok);
}

TEST(BundleBuilderTest, NetUnshieldRoundTrip) {
    // value_balance = -50 (unshield: pool loses 50 to transparent side).
    std::vector<shielded::PlannedSpend>  spends  = {MakeSpend(0x21, 100)};
    std::vector<shielded::PlannedOutput> outputs = {MakeOutput(0x22, 50)};
    Hash tx_sighash = MakeBlind(0xEE);

    shielded::ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle(spends, outputs, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);
    EXPECT_EQ(bundle.value_balance, -50);
    EXPECT_EQ(shielded::VerifyBundleRangeProofs(bundle),
              shielded::RangeProofResult::Ok);
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::Ok);
}

TEST(BundleBuilderTest, MultiSpendMultiOutputRoundTrip) {
    std::vector<shielded::PlannedSpend>  spends  = {
        MakeSpend(0x31, 30),
        MakeSpend(0x32, 70),
        MakeSpend(0x33, 100),
    };
    std::vector<shielded::PlannedOutput> outputs = {
        MakeOutput(0x34, 80),
        MakeOutput(0x35, 120),
    };
    Hash tx_sighash = MakeBlind(0xFA);

    shielded::ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle(spends, outputs, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);
    // sum(out)=200, sum(in)=200 → value_balance = 0
    EXPECT_EQ(bundle.value_balance, 0);
    EXPECT_EQ(bundle.spends.size(), 3u);
    EXPECT_EQ(bundle.outputs.size(), 2u);
    EXPECT_EQ(shielded::VerifyBundleRangeProofs(bundle),
              shielded::RangeProofResult::Ok);
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash),
              shielded::BindingSigResult::Ok);
}

TEST(BundleBuilderTest, BuilderRejectsTooManySpends) {
    std::vector<shielded::PlannedSpend>  spends(shielded::kMaxSpendsPerBundle + 1,
                                                MakeSpend(0x01, 1));
    std::vector<shielded::PlannedOutput> outputs = {MakeOutput(0x02, 1)};
    Hash tx_sighash{};
    shielded::ShieldedBundle bundle{};
    EXPECT_EQ(shielded::BuildShieldedBundle(spends, outputs, tx_sighash, bundle),
              shielded::BundleBuildResult::TooManySpends);
}

TEST(BundleBuilderTest, BuilderRejectsTamperedTxSighash) {
    // Builder signs over tx_sighash A; verify under tx_sighash B fails.
    std::vector<shielded::PlannedSpend>  spends  = {MakeSpend(0x41, 100)};
    std::vector<shielded::PlannedOutput> outputs = {MakeOutput(0x42, 100)};
    Hash tx_sighash_a = MakeBlind(0xA1);
    Hash tx_sighash_b = MakeBlind(0xB2);

    shielded::ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle(spends, outputs, tx_sighash_a, bundle),
              shielded::BundleBuildResult::Ok);
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash_a),
              shielded::BindingSigResult::Ok);
    EXPECT_EQ(shielded::VerifyBinding(bundle, tx_sighash_b),
              shielded::BindingSigResult::SignatureInvalid);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
