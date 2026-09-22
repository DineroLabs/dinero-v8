// tests/spike/shielded_v2_bundle_bench.cpp
// THROWAWAY spike code — spec docs/superpowers/specs/2026-09-22-shielded-v2-design.md §7,
// plan docs/superpowers/plans/2026-09-22-shielded-v2-spike.md. Not a ctest, not consensus code.
//
// Task 1: reproduce today's per-proof cost (cv-bound spend) as the baseline row.
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_tx.h"
#include "bundle_circuit_v2.h"
#include "crypto/evp_secp256k1.h"
#include <secp256k1.h>
#include "zk/zkvm/ipa.h"
#include "zk/zkvm/r1cs_spartan.h"
#include "zk/zkvm/transcript.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dinero::consensus::shielded;
using Clock = std::chrono::steady_clock;

#ifndef BUILD_TYPE_STR
#define BUILD_TYPE_STR "unknown"
#endif

struct BenchRow {
    std::string label;
    size_t constraints = 0, variables = 0, proof_bytes = 0;
    double prove_ms = 0, verify_ms = 0;
};

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static void PrintJson(const std::vector<BenchRow>& rows, const char* build_type) {
    std::printf("{\"build\":\"%s\",\"rows\":[", build_type);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        std::printf("%s{\"label\":\"%s\",\"constraints\":%zu,\"variables\":%zu,"
                    "\"proof_bytes\":%zu,\"prove_ms\":%.1f,\"verify_ms\":%.1f}",
                    i ? "," : "", r.label.c_str(), r.constraints, r.variables,
                    r.proof_bytes, r.prove_ms, r.verify_ms);
    }
    std::printf("]}\n");
}

// ── fixture helpers (copied from tests/consensus/test_shielded_cv_binding.cpp) ──
static Hash MakeHash(uint8_t seed, uint8_t tail = 0xCD) {
    Hash h{};
    h[0] = seed;
    h[31] = tail;
    return h;
}
static Hash ValueAsHash(uint64_t v) {
    Hash h{};
    for (int i = 0; i < 8; ++i) h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    return h;
}
static Hash MakeBlind(uint8_t seed) {
    Hash h{};
    h[31] = seed;
    h[30] = 0x11;
    h[20] = 0x22;
    return h;
}
static ValueCommitment Commit(const Hash& blind, uint64_t value) {
    ValueCommitment cv{};
    if (PedersenCommit(blind, value, cv) != PedersenResult::Ok) {
        std::fprintf(stderr, "PedersenCommit failed\n");
        std::exit(2);
    }
    return cv;
}

static double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Baseline: today's cv-bound spend proof (mainnet profile since height 61000).
static BenchRow BaselineSpend() {
    if (!PedersenGeneratorsReady()) {
        std::fprintf(stderr, "Pedersen generators not ready\n");
        std::exit(2);
    }
    const uint64_t value = 100'000'000;
    const Hash sk = MakeHash(0x01, 0x10);
    const Hash d = MakeHash(0x05, 0x10);
    const Hash randomness = MakeHash(0x03, 0x10);
    const Hash pk = PoseidonHash2(sk, Hash{});

    CommitmentTree tree;
    tree.Append(MakeHash(0x10));
    tree.Append(MakeHash(0x11));
    const Hash cm = NoteCommitment(d, pk, ValueAsHash(value), randomness);
    const uint64_t idx = tree.Append(cm);
    const auto path = tree.GetAuthPath(idx);
    if (!path.has_value()) {
        std::fprintf(stderr, "auth path missing\n");
        std::exit(2);
    }

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

    std::vector<double> prove, verify;
    size_t bytes = 0;
    for (int i = 0; i < 6; ++i) {
        // Fresh note per iteration: identical proof bytes would be served by the
        // #770 VerifiedProofCache and hide the real verification cost.
        w.randomness = MakeHash(0x03, static_cast<uint8_t>(0x10 + i));
        const Hash cm_i = NoteCommitment(d, pk, ValueAsHash(value), w.randomness);
        CommitmentTree tree_i;
        tree_i.Append(MakeHash(0x10));
        tree_i.Append(MakeHash(0x11));
        w.leaf_index = tree_i.Append(cm_i);
        w.merkle_path = tree_i.GetAuthPath(w.leaf_index)->siblings;
        pub.nullifier = ComputeNullifier(sk, w.leaf_index);
        pub.anchor = tree_i.Root();
        auto t0 = Clock::now();
        auto proof = ProveSpend(w, pub, nullptr, true, /*cv_bound=*/true);
        const double p = ms_since(t0);
        if (proof.empty()) {
            std::fprintf(stderr, "baseline ProveSpend returned empty\n");
            std::exit(2);
        }
        t0 = Clock::now();
        const bool ok = VerifySpend(proof, pub, nullptr, true, /*cv_bound=*/true);
        const double v = ms_since(t0);
        if (!ok) {
            std::fprintf(stderr, "baseline VerifySpend failed\n");
            std::exit(2);
        }
        if (i) {  // discard warm-up
            prove.push_back(p);
            verify.push_back(v);
        }
        bytes = proof.size();
    }
    return BenchRow{"baseline_spend_cv_bound", 0, 0, bytes, median_of(prove), median_of(verify)};
}


// Task 3: prove/verify the v2 bundle with the existing Spartan+Hyrax (phase-1 estimate).
static BenchRow BenchBundle(size_t n_in, size_t n_out) {
    using namespace dinero::zk::zkvm;
    using spike::BuildBundleCircuitV2; using spike::MakeHonestBundle; using spike::BundleV2;
    secp256k1_context* sctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    std::vector<double> prove, verify;
    size_t bytes = 0, ncons = 0, nvars = 0;
    for (int i = 0; i < 6; ++i) {
        BundleV2 b = MakeHonestBundle(n_in, n_out, 1000 + i);   // fee varies: fresh proof each iteration
        R1CS cs = BuildBundleCircuitV2(b);
        if (!cs.is_satisfied()) { std::fprintf(stderr, "bundle not satisfied\n"); std::exit(2); }
        ncons = cs.num_constraints(); nvars = cs.num_variables();
        const size_t gens_need = std::max<size_t>(4, std::max(HyraxParams::from_n(cs.num_variables()).n_cols,
                                                              HyraxParams::from_n(cs.num_constraints()).n_cols));
        const GeneratorSet& gens = GeneratorSet::cached(gens_need, sctx);
        auto t0 = Clock::now();
        Transcript tp("dinero.shielded.bundle.v2.spike");
        SpartanProof proof = r1cs_spartan_prove(cs, std::vector<Scalar>(cs.num_constraints(), Scalar::zero()),
                                                Scalar::one(), gens, tp, sctx, true);
        std::vector<uint8_t> ser = proof.serialize(sctx);
        const double p = ms_since(t0);
        // Verifier side: rebuild the circuit STRUCTURE from public inputs only (witness zeroed).
        BundleV2 pub_only = b;
        for (auto& sp : pub_only.spends) { sp.ask = sp.nullifier_key = sp.value = sp.randomness = sp.diversifier = Scalar::zero(); sp.leaf_index = 0; sp.siblings = {}; }
        for (auto& o : pub_only.outputs) { o.value = o.public_key = o.randomness = o.diversifier = Scalar::zero(); }
        R1CS vcs = BuildBundleCircuitV2(pub_only);
        t0 = Clock::now();
        SpartanProof parsed;
        if (!SpartanProof::deserialize(ser, parsed, sctx)) { std::fprintf(stderr, "deserialize failed\n"); std::exit(2); }
        Transcript tv("dinero.shielded.bundle.v2.spike");
        const bool ok = r1cs_spartan_verify(parsed, vcs, ncons, nvars, spartan_hash_r1cs_structure(vcs), Scalar::one(), gens, tv, sctx, true);
        const double v = ms_since(t0);
        if (!ok) { std::fprintf(stderr, "bundle verify failed (%zu-in-%zu-out)\n", n_in, n_out); std::exit(2); }
        if (i) { prove.push_back(p); verify.push_back(v); }
        bytes = ser.size();
    }
    return BenchRow{"bundle_" + std::to_string(n_in) + "in" + std::to_string(n_out) + "out", ncons, nvars, bytes, median_of(prove), median_of(verify)};
}


// Task 4: batched verification estimate — 50 two-in-two-out bundles, sequential vs 8 threads.
struct ProvenBundle { spike::BundleV2 pub_only; std::vector<uint8_t> proof; size_t ncons = 0, nvars = 0; };
static ProvenBundle ProveOnce(uint64_t fee_seed) {
    using namespace dinero::zk::zkvm;
    secp256k1_context* sctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    spike::BundleV2 b = spike::MakeHonestBundle(2, 2, 1000 + fee_seed);
    R1CS cs = spike::BuildBundleCircuitV2(b);
    const size_t gens_need = std::max<size_t>(4, std::max(HyraxParams::from_n(cs.num_variables()).n_cols, HyraxParams::from_n(cs.num_constraints()).n_cols));
    const GeneratorSet& gens = GeneratorSet::cached(gens_need, sctx);
    Transcript tp("dinero.shielded.bundle.v2.spike");
    SpartanProof proof = r1cs_spartan_prove(cs, std::vector<Scalar>(cs.num_constraints(), Scalar::zero()), Scalar::one(), gens, tp, sctx, true);
    ProvenBundle pb; pb.proof = proof.serialize(sctx); pb.ncons = cs.num_constraints(); pb.nvars = cs.num_variables();
    pb.pub_only = b;
    for (auto& sp : pb.pub_only.spends) { sp.ask = sp.nullifier_key = sp.value = sp.randomness = sp.diversifier = Scalar::zero(); sp.leaf_index = 0; sp.siblings = {}; }
    for (auto& o : pb.pub_only.outputs) { o.value = o.public_key = o.randomness = o.diversifier = Scalar::zero(); }
    return pb;
}
static bool VerifyOne(const ProvenBundle& pb, secp256k1_context* sctx) {
    using namespace dinero::zk::zkvm;
    R1CS vcs = spike::BuildBundleCircuitV2(pb.pub_only);
    const size_t gens_need = std::max<size_t>(4, std::max(HyraxParams::from_n(pb.nvars).n_cols, HyraxParams::from_n(pb.ncons).n_cols));
    const GeneratorSet& gens = GeneratorSet::cached(gens_need, sctx);
    SpartanProof parsed; if (!SpartanProof::deserialize(pb.proof, parsed, sctx)) return false;
    Transcript tv("dinero.shielded.bundle.v2.spike");
    return r1cs_spartan_verify(parsed, vcs, pb.ncons, pb.nvars, spartan_hash_r1cs_structure(vcs), Scalar::one(), gens, tv, sctx, true);
}
static void BenchBatch(std::vector<BenchRow>& rows) {
    secp256k1_context* sctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    std::vector<ProvenBundle> proofs; for (int i = 0; i < 50; ++i) proofs.push_back(ProveOnce(static_cast<uint64_t>(i)));
    auto t0 = Clock::now();
    for (auto& pb : proofs) if (!VerifyOne(pb, sctx)) { std::fprintf(stderr, "batch verify failed\n"); std::exit(2); }
    rows.push_back(BenchRow{"verify_50_sequential_ms", 0, 0, 0, 0, ms_since(t0)});
    t0 = Clock::now();
    std::atomic<size_t> next{0}; std::vector<std::future<bool>> fs;
    for (int t = 0; t < 8; ++t) fs.push_back(std::async(std::launch::async, [&] {
        secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        bool ok = true; for (size_t i; (i = next++) < proofs.size();) ok = VerifyOne(proofs[i], c) && ok;
        secp256k1_context_destroy(c); return ok; }));
    for (auto& f : fs) if (!f.get()) { std::fprintf(stderr, "parallel batch verify failed\n"); std::exit(2); }
    rows.push_back(BenchRow{"verify_50_parallel8_ms", 0, 0, 0, 0, ms_since(t0)});
}

// Task 2: bundle circuit satisfiability self-test (spec §3.1 relations).
static int SelfTest() {
    using spike::BuildBundleCircuitV2; using spike::MakeHonestBundle; using spike::BundleV2;
    using dinero::zk::zkvm::Scalar;
    int failures = 0;
    auto check = [&](bool c, const char* name) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", name); if (!c) ++failures; };

    BundleV2 honest = MakeHonestBundle(2, 2, 1000);
    check(BuildBundleCircuitV2(honest).is_satisfied(), "honest 2-in-2-out bundle is satisfied");

    BundleV2 bad_balance = honest; bad_balance.outputs[0].value = bad_balance.outputs[0].value + Scalar::one();
    check(!BuildBundleCircuitV2(bad_balance).is_satisfied(), "output value +1 breaks the output commitment binding");

    BundleV2 bad_fee = honest; bad_fee.fee += 1;
    check(!BuildBundleCircuitV2(bad_fee).is_satisfied(), "fee +1 breaks balance");

    BundleV2 bad_anchor = honest; bad_anchor.spends[0].anchor = bad_anchor.spends[0].anchor + Scalar::one();
    check(!BuildBundleCircuitV2(bad_anchor).is_satisfied(), "wrong anchor breaks the Merkle path");

    BundleV2 bad_nf = honest; bad_nf.spends[1].nullifier = bad_nf.spends[1].nullifier + Scalar::one();
    check(!BuildBundleCircuitV2(bad_nf).is_satisfied(), "wrong nullifier breaks the spend");

    BundleV2 big = honest;
    { Scalar two64 = Scalar::one(); for (int k = 0; k < 64; ++k) two64 = two64 + two64;   // 2^64
      big.spends[0].value = big.spends[0].value + two64;
      big.outputs[0].value = big.outputs[0].value + two64; }                            // balance still holds
    check(!BuildBundleCircuitV2(big).is_satisfied(), "65-bit value fails the range check");

    const auto cs = BuildBundleCircuitV2(honest);
    std::printf("  2-in-2-out constraints=%zu variables=%zu\n", cs.num_constraints(), cs.num_variables());
    check(cs.num_constraints() < 60000, "2-in-2-out is under 60k constraints (measured 56,790 with the §10.2 hash-key legs; legacy per-tx total is ~3.3M)");
    return failures == 0 ? 0 : 1;
}


// Owner review 2026-09-22 finding 2: negative cases against the EXACT amended statement.
// Each must be unsatisfiable (no valid witness exists for a party lacking `ask`).
static int Negatives() {
    using namespace dinero::zk::zkvm;
    int failures = 0;
    auto check = [&](const char* label, bool expect_satisfied, const spike::BundleV2& b) {
        const bool sat = spike::BuildBundleCircuitV2(b).is_satisfied();
        std::printf("%-46s satisfied=%d expected=%d %s\n", label, sat, expect_satisfied, sat == expect_satisfied ? "OK" : "FAIL");
        if (sat != expect_satisfied) ++failures;
    };
    const spike::BundleV2 honest = spike::MakeHonestBundle(2, 2, 1000);
    check("honest 2-in-2-out", true, honest);
    { auto b = honest; b.spends[0].ask = b.spends[0].ask + Scalar::one(); check("sender/attacker: wrong ask", false, b); }
    { auto b = honest; b.spends[0].ask = Scalar::zero(); check("full viewer: hk/nfk/d/value/rcm known, ask=0", false, b); }
    { auto b = honest; b.spends[1].nullifier_key = b.spends[1].nullifier_key + Scalar::one(); check("wrong nullifier key (nf and pk both move)", false, b); }
    { auto b = honest; b.spends[0].diversifier = b.spends[0].diversifier + Scalar::one(); check("wrong diversifier", false, b); }
    { auto b = honest; b.outputs[0].value = b.outputs[0].value + Scalar::one(); check("output value +1 (balance)", false, b); }
    { auto b = honest; b.fee += 1; check("public fee +1 (balance)", false, b); }
    { auto b = honest; b.spends[0].leaf_index ^= 1; check("wrong leaf index (path + nf)", false, b); }
    { auto b = honest; b.spends[0].siblings[3] = Hash{}; check("wrong merkle sibling", false, b); }
    std::printf("negatives: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--negatives") return Negatives();
    if (argc > 1 && std::string(argv[1]) == "--selftest") return SelfTest();
    std::vector<BenchRow> rows;
    if (!(argc > 1 && std::string(argv[1]) == "--no-baseline")) rows.push_back(BaselineSpend());
    rows.push_back(BenchBundle(1, 2));
    rows.push_back(BenchBundle(2, 2));
    rows.push_back(BenchBundle(4, 2));
    if (!(argc > 1 && std::string(argv[1]) == "--no-batch")) BenchBatch(rows);
    PrintJson(rows, BUILD_TYPE_STR);
    return 0;
}
