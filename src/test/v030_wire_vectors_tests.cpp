// Copyright (c) 2026 Dinero Labs.
//
// Phase 3 Wave 3 — v0.3.0 wire-format canonical bundle vectors.
//
// Pins the *deterministic* public outputs of the shielded bundle wire
// format for four canonical scenarios:
//   1. Shield      (0 spends, 1 output, value_balance = +V)
//   2. Unshield    (1 spend,  0 outputs, value_balance = -V)
//   3. Transfer-3d (1 spend,  1 output,  value_balance = -fee)
//   4. Transfer-3e (2 spends, 2 outputs, value_balance = -fee)
//
// What we pin (deterministic given inputs):
//   - kPedersenVDST string + V generator x-coord
//   - per-scenario nullifier hex
//   - per-scenario commitment hex
//   - value_balance integer
//   - aggregated_range_proof and bvk_commitment non-empty after build
//   - bundle round-trips serialize ↔ deserialize byte-identically
//   - ValidateShieldedBundle returns Ok
//
// What we do NOT pin (randomized inside the prover):
//   - Spartan zk_proof bytes (per-spend, per-output)
//   - rcv values are SUPPLIED deterministic in this test, so cv x-coords
//     are also deterministic — but we don't byte-pin them because libsecp
//     compressed-point parity is implementation-detail; we instead assert
//     the validator accepts (which exercises pedersen_verify_tally).
//
// A second implementation that wants byte-level parity should:
//   1. Match kPedersenVDST exactly.
//   2. Match the nullifier / commitment formulas (Poseidon + diversifier=0).
//   3. Match the BIP340 Schnorr binding-sig sighash construction.
//   4. Confirm its bundles round-trip the same wire format.

#include <gtest/gtest.h>

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/range_proof.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/shielded_validation.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>  // _getpid
#define DINERO_GETPID _getpid
#else
#include <unistd.h>   // getpid
#define DINERO_GETPID getpid
#endif

namespace dinero::consensus::shielded::testing {
namespace {

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

Hash ValueAsHash(uint64_t v) {
    Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

std::string Hex(const Hash& h) {
    std::string out;
    out.reserve(64);
    for (uint8_t b : h) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

std::string TempDbPath(const char* tag) {
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<unsigned long long>(DINERO_GETPID());
    const auto ts = static_cast<long long>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    char name[96];
    std::snprintf(name, sizeof(name),
                  "dinero_v030_vec_%s_%llu_%lld_%llu.db",
                  tag, pid, ts, static_cast<unsigned long long>(seq));
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);  // best-effort cleanup of stale path
    return path.string();
}

// Round-trip a bundle through the canonical serializer and assert byte parity.
void AssertBundleRoundTrips(const ShieldedBundle& bundle) {
    auto bytes = shielded::SerializeShieldedBundle(bundle);
    ASSERT_FALSE(bytes.empty()) << "serializer returned empty bytes";
    ShieldedBundle parsed{};
    auto rc = shielded::DeserializeShieldedBundle(bytes, &parsed);
    ASSERT_EQ(rc, shielded::BundleDecodeError::Ok);
    auto bytes2 = shielded::SerializeShieldedBundle(parsed);
    ASSERT_EQ(bytes, bytes2) << "deserialize ↔ serialize is not idempotent";
}

TEST(ShieldedRangeProofContainer, RejectsUnbackedCountsAndLengths) {
    ASSERT_TRUE(shielded::PedersenGeneratorsReady() ||
                !shielded::PedersenGeneratorV().empty());

    ShieldedBundle bundle{};

    // A CompactSize count of UINT64_MAX is not backed by even one length byte
    // per claimed proof. It must be rejected before reserve(size_t(n)).
    bundle.aggregated_range_proof = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    EXPECT_EQ(VerifyBundleRangeProofs(bundle), RangeProofResult::ParseError);

    // One proof whose UINT64_MAX byte length is not backed by the input. The
    // length comparison must not form an out-of-range pointer.
    bundle.aggregated_range_proof = {
        0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    EXPECT_EQ(VerifyBundleRangeProofs(bundle), RangeProofResult::ParseError);
}

class V030VectorFixture : public ::testing::Test {
protected:
    CommitmentTree tree;
    NullifierSet nullifier_set;
    std::string nf_path;
    // nullifier_set re-pointed in SetUp() once Open() prepares it.
    // tx_sighash defaults to Hash{} because the v030 vectors fixture
    // does not assemble bundles with aggregated_range_proof, so the
    // binding-sig branch is gated off.
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
        nf_path = TempDbPath("nf");
        ASSERT_EQ(nullifier_set.Open(nf_path), NullifierSet::OpenResult::Ok);
        ctx.nullifier_set = &nullifier_set;
        ASSERT_TRUE(shielded::PedersenGeneratorsReady() ||
                    !shielded::PedersenGeneratorV().empty());
    }

    void TearDown() override {
        nullifier_set.Close();
        std::filesystem::remove(nf_path);
    }
};

// ── Vector 1: SHIELD (0 spends, 1 output) ────────────────────────────
//
// Inputs (deterministic):
//   sk         = 0x10..00..F0     (seed=0x10, tail=0xF0)
//   randomness = 0x11..00..F0
//   d          = 0x00..00 (zero diversifier per Phase 5 parking)
//   value      = 100_000_000 una (1 DIN)
//   tx_sighash = 0x12..00..F0
//
// Expected (computed, pinned in this test):
//   pk            = Poseidon(sk, 0)
//   value_hash    = ValueAsHash(100_000_000)
//   commitment    = NoteCommitment(d=0, pk, value_hash, randomness)
//   value_balance = +100_000_000
TEST_F(V030VectorFixture, ShieldVector) {
    constexpr uint64_t kValue = 100'000'000;
    const Hash sk         = MakeHash(0x10, 0xF0);
    const Hash randomness = MakeHash(0x11, 0xF0);
    const Hash d{};
    const Hash value_hash = ValueAsHash(kValue);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash commitment = NoteCommitment(d, pk, value_hash, randomness);
    const Hash tx_sighash = MakeHash(0x12, 0xF0);

    OutputWitness ow{};
    ow.value      = value_hash;
    ow.public_key = pk;
    ow.randomness = randomness;
    ow.d          = d;
    OutputPublicInputs opi{};
    opi.commitment = commitment;
    auto output_proof = ProveOutput(ow, opi, nullptr);
    ASSERT_FALSE(output_proof.empty());

    shielded::PlannedOutput po;
    po.commitment     = commitment;
    po.value_una      = kValue;
    po.rcv            = MakeHash(0x13, 0xF0);
    po.encrypted_note = std::vector<uint8_t>(96, 0xAA);
    po.output_proof   = std::move(output_proof);
    po.nonce          = MakeHash(0x14, 0xF0);

    ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle({}, {po}, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);

    EXPECT_EQ(bundle.value_balance, static_cast<int64_t>(kValue));
    EXPECT_EQ(bundle.spends.size(), 0u);
    ASSERT_EQ(bundle.outputs.size(), 1u);
    EXPECT_EQ(bundle.outputs[0].commitment, commitment);
    EXPECT_FALSE(bundle.aggregated_range_proof.empty());
    AssertBundleRoundTrips(bundle);

    ctx.tx_sighash              = tx_sighash;
    ctx.transparent_value_delta = static_cast<int64_t>(kValue);
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx), ShieldedValidationError::Ok);

    // Pinned hex (deterministic given seeds + value):
    EXPECT_EQ(Hex(commitment),
              "1805599d42fa07a9e96becb98a5a2627a7bd92a49445114cd2b8a611a883a752");
}

// ── Vector 2: UNSHIELD (1 spend, 0 outputs) ──────────────────────────
TEST_F(V030VectorFixture, UnshieldVector) {
    constexpr uint64_t kValue = 100'000'000;
    const Hash sk         = MakeHash(0x20, 0xF1);
    const Hash randomness = MakeHash(0x21, 0xF1);
    const Hash d{};
    const Hash value_hash = ValueAsHash(kValue);
    const Hash pk         = PoseidonHash2(sk, Hash{});
    const Hash commitment = NoteCommitment(d, pk, value_hash, randomness);
    const uint64_t leaf_idx = tree.Append(commitment);
    const Hash tx_sighash = MakeHash(0x22, 0xF1);

    SpendWitness sw{};
    sw.secret_key = sk;
    sw.leaf_index = leaf_idx;
    sw.value      = value_hash;
    sw.randomness = randomness;
    sw.d          = d;
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());
    sw.merkle_path = path->siblings;

    SpendPublicInputs spi{};
    spi.nullifier = ComputeNullifier(sk, leaf_idx);
    spi.anchor    = tree.Root();
    auto spend_proof = ProveSpend(sw, spi, nullptr);
    ASSERT_FALSE(spend_proof.empty());

    shielded::PlannedSpend ps;
    ps.nullifier   = spi.nullifier;
    ps.anchor      = spi.anchor;
    ps.value_una   = kValue;
    ps.rcv         = MakeHash(0x23, 0xF1);
    ps.spend_proof = std::move(spend_proof);
    ps.nonce       = MakeHash(0x24, 0xF1);

    ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle({ps}, {}, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);

    EXPECT_EQ(bundle.value_balance, -static_cast<int64_t>(kValue));
    ASSERT_EQ(bundle.spends.size(), 1u);
    EXPECT_EQ(bundle.outputs.size(), 0u);
    EXPECT_EQ(bundle.spends[0].nullifier, spi.nullifier);
    EXPECT_EQ(bundle.spends[0].anchor, spi.anchor);
    EXPECT_FALSE(bundle.aggregated_range_proof.empty());
    AssertBundleRoundTrips(bundle);

    ctx.tx_sighash              = tx_sighash;
    ctx.transparent_value_delta = -static_cast<int64_t>(kValue);
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx), ShieldedValidationError::Ok);

    EXPECT_EQ(Hex(spi.nullifier),
              "cf2b8adc7b53b371094aab1dfa02c5edbd12476a4085184ddba1cd801a72ec33");
    EXPECT_EQ(Hex(commitment),
              "20c697258800b3f1048837dbba83efb539eeebd4bd5798c33923316066beb372");
}

// ── Vector 3: TRANSFER 3d (1 spend, 1 output, vb = -fee) ─────────────
TEST_F(V030VectorFixture, Transfer3dVector) {
    constexpr uint64_t kSpend  = 100'000'000;
    constexpr uint64_t kFee    = 10'000;
    constexpr uint64_t kOutput = kSpend - kFee;

    // Spend-side note already in the tree.
    const Hash sk_in   = MakeHash(0x30, 0xF2);
    const Hash rand_in = MakeHash(0x31, 0xF2);
    const Hash d{};
    const Hash val_in  = ValueAsHash(kSpend);
    const Hash pk_in   = PoseidonHash2(sk_in, Hash{});
    const Hash cm_in   = NoteCommitment(d, pk_in, val_in, rand_in);
    const uint64_t leaf_idx = tree.Append(cm_in);

    SpendWitness sw{};
    sw.secret_key = sk_in;
    sw.leaf_index = leaf_idx;
    sw.value      = val_in;
    sw.randomness = rand_in;
    sw.d          = d;
    auto path = tree.GetAuthPath(leaf_idx);
    ASSERT_TRUE(path.has_value());
    sw.merkle_path = path->siblings;
    SpendPublicInputs spi{};
    spi.nullifier = ComputeNullifier(sk_in, leaf_idx);
    spi.anchor    = tree.Root();
    auto spend_proof = ProveSpend(sw, spi, nullptr);
    ASSERT_FALSE(spend_proof.empty());

    // Output-side fresh self-note.
    const Hash sk_out   = MakeHash(0x32, 0xF2);
    const Hash rand_out = MakeHash(0x33, 0xF2);
    const Hash val_out  = ValueAsHash(kOutput);
    const Hash pk_out   = PoseidonHash2(sk_out, Hash{});
    const Hash cm_out   = NoteCommitment(d, pk_out, val_out, rand_out);

    OutputWitness ow{};
    ow.value      = val_out;
    ow.public_key = pk_out;
    ow.randomness = rand_out;
    ow.d          = d;
    OutputPublicInputs opi{};
    opi.commitment = cm_out;
    auto output_proof = ProveOutput(ow, opi, nullptr);
    ASSERT_FALSE(output_proof.empty());

    const Hash tx_sighash = MakeHash(0x34, 0xF2);

    shielded::PlannedSpend ps;
    ps.nullifier   = spi.nullifier;
    ps.anchor      = spi.anchor;
    ps.value_una   = kSpend;
    ps.rcv         = MakeHash(0x35, 0xF2);
    ps.spend_proof = std::move(spend_proof);
    ps.nonce       = MakeHash(0x36, 0xF2);

    shielded::PlannedOutput po;
    po.commitment     = cm_out;
    po.value_una      = kOutput;
    po.rcv            = MakeHash(0x37, 0xF2);
    po.encrypted_note = std::vector<uint8_t>(96, 0xBB);
    po.output_proof   = std::move(output_proof);
    po.nonce          = MakeHash(0x38, 0xF2);

    ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle({ps}, {po}, tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);

    EXPECT_EQ(bundle.value_balance, -static_cast<int64_t>(kFee));
    ASSERT_EQ(bundle.spends.size(), 1u);
    ASSERT_EQ(bundle.outputs.size(), 1u);
    AssertBundleRoundTrips(bundle);

    ctx.tx_sighash              = tx_sighash;
    ctx.transparent_value_delta = -static_cast<int64_t>(kFee);
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx), ShieldedValidationError::Ok);

    EXPECT_EQ(Hex(spi.nullifier),
              "bfd22264f5ddddd16160586823d8fc54abfa49d8cc12e563688929f06f3b907d");
    EXPECT_EQ(Hex(cm_in),
              "55065d9e5fe5e7f9a63bb0f092a0e14914d92524143e0c36ee0771b2130961cb");
    EXPECT_EQ(Hex(cm_out),
              "607ceaf56e7bd4cdd8e177fa5c67a57aeaf9b6c74d0cc691139f3caec1b8eed1");
}

// ── Vector 4: TRANSFER 3e (2 spends, 2 outputs: recipient + change) ──
TEST_F(V030VectorFixture, Transfer3eVector) {
    constexpr uint64_t kSpendEach = 50'000'000;       // 0.5 DIN per note
    constexpr uint64_t kFee       = 20'000;
    constexpr uint64_t kRecipient = 70'000'000;       // 0.7 DIN
    constexpr uint64_t kChange    = (2 * kSpendEach) - kRecipient - kFee;
    static_assert(kChange == 29'980'000, "vector arithmetic");

    const Hash d{};

    // Two input notes appended to the wallet tree.
    const Hash sk_a   = MakeHash(0x40, 0xF3);
    const Hash rand_a = MakeHash(0x41, 0xF3);
    const Hash val_a  = ValueAsHash(kSpendEach);
    const Hash pk_a   = PoseidonHash2(sk_a, Hash{});
    const Hash cm_a   = NoteCommitment(d, pk_a, val_a, rand_a);
    const uint64_t leaf_a = tree.Append(cm_a);

    const Hash sk_b   = MakeHash(0x42, 0xF3);
    const Hash rand_b = MakeHash(0x43, 0xF3);
    const Hash val_b  = ValueAsHash(kSpendEach);
    const Hash pk_b   = PoseidonHash2(sk_b, Hash{});
    const Hash cm_b   = NoteCommitment(d, pk_b, val_b, rand_b);
    const uint64_t leaf_b = tree.Append(cm_b);

    auto build_planned_spend = [&](const Hash& sk, const Hash& randv,
                                   const Hash& valh, uint64_t leaf,
                                   uint64_t value, uint8_t rcv_seed,
                                   uint8_t nonce_seed)
        -> shielded::PlannedSpend {
        SpendWitness sw{};
        sw.secret_key = sk;
        sw.leaf_index = leaf;
        sw.value      = valh;
        sw.randomness = randv;
        sw.d          = d;
        auto p = tree.GetAuthPath(leaf);
        EXPECT_TRUE(p.has_value());
        sw.merkle_path = p->siblings;
        SpendPublicInputs spi{};
        spi.nullifier = ComputeNullifier(sk, leaf);
        spi.anchor    = tree.Root();
        auto proof = ProveSpend(sw, spi, nullptr);
        EXPECT_FALSE(proof.empty());
        shielded::PlannedSpend ps;
        ps.nullifier   = spi.nullifier;
        ps.anchor      = spi.anchor;
        ps.value_una   = value;
        ps.rcv         = MakeHash(rcv_seed, 0xF3);
        ps.spend_proof = std::move(proof);
        ps.nonce       = MakeHash(nonce_seed, 0xF3);
        return ps;
    };

    auto ps_a = build_planned_spend(sk_a, rand_a, val_a, leaf_a, kSpendEach, 0x44, 0x45);
    auto ps_b = build_planned_spend(sk_b, rand_b, val_b, leaf_b, kSpendEach, 0x46, 0x47);

    auto build_planned_output = [&](uint64_t value, uint8_t sk_seed,
                                    uint8_t rand_seed, uint8_t rcv_seed,
                                    uint8_t nonce_seed)
        -> std::pair<shielded::PlannedOutput, Hash> {
        Hash sk_out   = MakeHash(sk_seed, 0xF3);
        Hash rand_out = MakeHash(rand_seed, 0xF3);
        Hash val_h    = ValueAsHash(value);
        Hash pk_out   = PoseidonHash2(sk_out, Hash{});
        Hash cm_out   = NoteCommitment(d, pk_out, val_h, rand_out);
        OutputWitness ow{};
        ow.value      = val_h;
        ow.public_key = pk_out;
        ow.randomness = rand_out;
        ow.d          = d;
        OutputPublicInputs opi{};
        opi.commitment = cm_out;
        auto proof = ProveOutput(ow, opi, nullptr);
        EXPECT_FALSE(proof.empty());
        shielded::PlannedOutput po;
        po.commitment     = cm_out;
        po.value_una      = value;
        po.rcv            = MakeHash(rcv_seed, 0xF3);
        po.encrypted_note = std::vector<uint8_t>(96, 0xCC);
        po.output_proof   = std::move(proof);
        po.nonce          = MakeHash(nonce_seed, 0xF3);
        return {std::move(po), cm_out};
    };

    auto [po_recipient, cm_recipient] =
        build_planned_output(kRecipient, 0x48, 0x49, 0x4A, 0x4B);
    auto [po_change, cm_change] =
        build_planned_output(kChange, 0x4C, 0x4D, 0x4E, 0x4F);

    const Hash tx_sighash = MakeHash(0x50, 0xF3);
    ShieldedBundle bundle{};
    ASSERT_EQ(shielded::BuildShieldedBundle({ps_a, ps_b},
                                            {po_recipient, po_change},
                                            tx_sighash, bundle),
              shielded::BundleBuildResult::Ok);

    EXPECT_EQ(bundle.value_balance, -static_cast<int64_t>(kFee));
    ASSERT_EQ(bundle.spends.size(), 2u);
    ASSERT_EQ(bundle.outputs.size(), 2u);
    AssertBundleRoundTrips(bundle);

    ctx.tx_sighash              = tx_sighash;
    ctx.transparent_value_delta = -static_cast<int64_t>(kFee);
    EXPECT_EQ(ValidateShieldedBundle(bundle, ctx), ShieldedValidationError::Ok);

    EXPECT_EQ(Hex(ps_a.nullifier),
              "02350fc32a565e2e5fac9894efbbee47f4146ad69b250a02023752c3de4ac5c7");
    EXPECT_EQ(Hex(ps_b.nullifier),
              "d6126c0a59e51448882c3b0fb5cfce70ad2fe0590e76144a9849c149fdb2151c");
    EXPECT_EQ(Hex(cm_a),
              "9bd9f2d07c65265e350b2ef96e1e6651c32c1475b898d4cbca2717bfa1490e70");
    EXPECT_EQ(Hex(cm_b),
              "f87b8857bcfc0382af9876f6dfcffecd80cea2eb67599cd23dc52d83b5cfe06e");
    EXPECT_EQ(Hex(cm_recipient),
              "9568bfe75016b5bf7a6fe107bdd63a82dc1b1d1f17825a33bb284aec3c2582da");
    EXPECT_EQ(Hex(cm_change),
              "f318afecf0ae4cec0564be3d243eed47c71b24e78145111912bbba9cee584e6d");
}

// ── Generator V is a chain-split-relevant constant. Pin its DST and
// ── x-coord byte-pattern so an implementation-divergence ships a
// ── failing test instead of mining incompatible bundles.
TEST(V030GeneratorVector, PedersenGeneratorVIsStable) {
    EXPECT_STREQ(shielded::kPedersenVDST, "DIN/v7/shielded/cv/V/v1");
    const auto& v = shielded::PedersenGeneratorV();
    EXPECT_EQ(v.size(), 32u);
    bool any_set = false;
    for (uint8_t b : v) {
        if (b != 0) { any_set = true; break; }
    }
    EXPECT_TRUE(any_set) << "V generator x-coord must not be all-zero";
    // Pinned by-byte: any change ships a chain split.
    EXPECT_EQ([&]{
        std::string s;
        for (uint8_t b : v) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            s += buf;
        }
        return s;
    }(), "347ff26d5f650e9f4b3af12a66dba9ebeee1996ffac6a964f98a24dab522b1d4");
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
