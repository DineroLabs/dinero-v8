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
#include "zk/zkvm/r1cs_verifier_matrices.h"
#include "zk/zkvm/transcript.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <tuple>
#include <mutex>
#include <future>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dinero::consensus::shielded;
using Clock = std::chrono::steady_clock;

// Task 7 research profile: standalone proof without the E term (u == 1, E == 0).
static constexpr bool kOmitErrorTerm = true;

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

// Old design on the same host: one cv-bound OUTPUT proof (live profile for outputs).
static BenchRow BaselineOutput() {
    const uint64_t value = 100'000'000;
    std::vector<double> prove, verify; size_t bytes = 0;
    for (int i = 0; i < 6; ++i) {
        OutputWitness w{};
        w.value = ValueAsHash(value); w.public_key = MakeHash(0x21, static_cast<uint8_t>(i)); w.randomness = MakeHash(0x22, static_cast<uint8_t>(i)); w.d = MakeHash(0x23, 0x10); w.rcv = MakeBlind(static_cast<uint8_t>(0x30 + i));
        OutputPublicInputs pub{};
        pub.commitment = NoteCommitment(w.d, w.public_key, w.value, w.randomness);
        pub.cv = Commit(w.rcv, value);
        auto t0 = Clock::now();
        auto proof = ProveOutput(w, pub, nullptr, true, true);
        const double p = ms_since(t0);
        if (proof.empty()) { std::fprintf(stderr, "baseline output prove failed\n"); std::exit(2); }
        t0 = Clock::now();
        const bool ok = VerifyOutput(proof, pub, nullptr, true, true);
        const double v = ms_since(t0);
        if (!ok) { std::fprintf(stderr, "baseline output verify failed\n"); std::exit(2); }
        if (i) { prove.push_back(p); verify.push_back(v); }
        bytes = proof.size();
    }
    return BenchRow{"old_output_cv_bound", 0, 0, bytes, median_of(prove), median_of(verify)};
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
                                                Scalar::one(), gens, tp, sctx, true, kOmitErrorTerm);
        std::vector<uint8_t> ser = proof.serialize(sctx);
        if (ser.empty()) { std::fprintf(stderr, "prove failed\n"); std::exit(2); }
        const double p = ms_since(t0);
        // Verifier side: rebuild the circuit STRUCTURE from public inputs only (witness zeroed).
        BundleV2 pub_only = b;
        for (auto& sp : pub_only.spends) { sp.ask = sp.nullifier_key = sp.value = sp.randomness = sp.diversifier = Scalar::zero(); sp.leaf_index = 0; sp.siblings = {}; }
        for (auto& o : pub_only.outputs) { o.value = o.public_key = o.randomness = o.diversifier = Scalar::zero(); }
        R1CS vcs = BuildBundleCircuitV2(pub_only);
        const bool timing = std::getenv("DINERO_SPARTAN_TIMING") != nullptr;
        auto tb = Clock::now();
        R1CS vcs_again = BuildBundleCircuitV2(pub_only);
        if (timing) std::fprintf(stderr, "BENCH_PHASE build_vcs     %8.3f ms (n=%zu)\n", ms_since(tb), ncons);
        // The structure hash is a per-shape constant: a node computes it once per (n_in, n_out)
        // and never per proof. It is therefore outside the timed region (measured separately).
        tb = Clock::now();
        const auto chash = spartan_hash_r1cs_structure(vcs);
        if (timing) std::fprintf(stderr, "BENCH_PHASE hash_struct   %8.3f ms (per-shape constant, not per proof)\n", ms_since(tb));
        t0 = Clock::now();
        SpartanProof parsed;
        if (!SpartanProof::deserialize(ser, parsed, sctx, kOmitErrorTerm)) { std::fprintf(stderr, "deserialize failed\n"); std::exit(2); }
        if (timing) std::fprintf(stderr, "BENCH_PHASE deserialize   %8.3f ms\n", ms_since(t0));
        Transcript tv("dinero.shielded.bundle.v2.spike");
        const bool ok = r1cs_spartan_verify(parsed, vcs, ncons, nvars, chash, Scalar::one(), gens, tv, sctx, true, true, kOmitErrorTerm, /*matrix_threads=*/8);
        const double v = ms_since(t0);
        if (!ok) { std::fprintf(stderr, "bundle verify failed (%zu-in-%zu-out)\n", n_in, n_out); std::exit(2); }
        if (i) { prove.push_back(p); verify.push_back(v); }
        bytes = ser.size();
    }
    return BenchRow{"bundle_" + std::to_string(n_in) + "in" + std::to_string(n_out) + "out", ncons, nvars, bytes, median_of(prove), median_of(verify)};
}


// Task 7 (owner review 2026-09-22): the complete warm verification entry point with trusted,
// immutable per-shape verifier data, and a batch with ONE bounded parallelism budget.
//
// ShapeCache: keyed by the full circuit shape and profile (n_in, n_out, omit_error_term), never by
// constraint count. Holds the verifier R1CS structure (zero witness), the structure hash and the
// generator size. Cold initialisation (build + hash) is reported separately; per proof the
// structure is copied and its public-input slots set (R1CS::set_value), which is what a node does.
struct ShapeKey { size_t n_in, n_out; bool omit; bool operator<(const ShapeKey& o) const { return std::tie(n_in, n_out, omit) < std::tie(o.n_in, o.n_out, o.omit); } };
struct ShapeData { dinero::zk::zkvm::R1CS structure; dinero::zk::zkvm::R1CSVerifierMatrices matrices; std::vector<uint8_t> chash; size_t gens_need = 0; double cold_init_ms = 0; double csr_build_ms = 0; };
static std::map<ShapeKey, ShapeData> g_shapes; static std::mutex g_shapes_mu;
static const ShapeData& ShapeFor(size_t n_in, size_t n_out) {
    using namespace dinero::zk::zkvm;
    std::lock_guard<std::mutex> lk(g_shapes_mu);
    const ShapeKey key{n_in, n_out, kOmitErrorTerm};
    auto it = g_shapes.find(key);
    if (it != g_shapes.end()) return it->second;
    auto t0 = Clock::now();
    ShapeData d;
    spike::BundleV2 zero = spike::MakeHonestBundle(n_in, n_out, 1000);
    for (auto& sp : zero.spends) { sp.ask = sp.nullifier_key = sp.value = sp.randomness = sp.diversifier = sp.anchor = sp.nullifier = Scalar::zero(); sp.leaf_index = 0; sp.siblings = {}; }
    for (auto& o : zero.outputs) { o.value = o.public_key = o.randomness = o.diversifier = o.commitment = Scalar::zero(); }
    zero.sighash = Scalar::zero(); zero.fee = 0;
    d.structure = spike::BuildBundleCircuitV2(zero);
    d.chash = spartan_hash_r1cs_structure(d.structure);
    { auto tc = Clock::now(); d.matrices = R1CSVerifierMatrices::FromR1CS(d.structure); d.csr_build_ms = ms_since(tc);
      std::fprintf(stderr, "CSR shape %zu-in-%zu-out: nnz=%zu (+1: %zu, -1: %zu, general: %zu) build=%.1f ms\n", n_in, n_out,
                   d.matrices.nnz_total, d.matrices.nnz_one, d.matrices.nnz_neg_one, d.matrices.nnz_total - d.matrices.nnz_one - d.matrices.nnz_neg_one, d.csr_build_ms); }
    d.gens_need = std::max<size_t>(4, std::max(HyraxParams::from_n(d.structure.num_variables()).n_cols, HyraxParams::from_n(d.structure.num_constraints()).n_cols));
    d.cold_init_ms = ms_since(t0);
    return g_shapes.emplace(key, std::move(d)).first->second;
}
// Public inputs in the exact allocation order of BuildBundleCircuitV2: sighash, fee, (anchor, nullifier)*, commitment*.
static std::vector<dinero::zk::zkvm::Scalar> PublicInputsOf(const spike::BundleV2& b) {
    using dinero::zk::zkvm::Scalar;
    std::vector<Scalar> v{b.sighash, Scalar(b.fee)};
    for (const auto& s : b.spends) { v.push_back(s.anchor); v.push_back(s.nullifier); }
    for (const auto& o : b.outputs) v.push_back(o.commitment);
    return v;
}
struct ProvenBundle { spike::BundleV2 pub; std::vector<uint8_t> proof; size_t n_in = 0, n_out = 0; };
static ProvenBundle ProveOnce(size_t n_in, size_t n_out, uint64_t fee_seed) {
    using namespace dinero::zk::zkvm;
    secp256k1_context* sctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    spike::BundleV2 b = spike::MakeHonestBundle(n_in, n_out, 1000 + fee_seed);
    R1CS cs = spike::BuildBundleCircuitV2(b);
    const GeneratorSet& gens = GeneratorSet::cached(ShapeFor(n_in, n_out).gens_need, sctx);
    Transcript tp("dinero.shielded.bundle.v2.spike");
    SpartanProof proof = r1cs_spartan_prove(cs, std::vector<Scalar>(cs.num_constraints(), Scalar::zero()), Scalar::one(), gens, tp, sctx, true, kOmitErrorTerm);
    ProvenBundle pb; pb.proof = proof.serialize(sctx); pb.n_in = n_in; pb.n_out = n_out; pb.pub = b;
    for (auto& sp : pb.pub.spends) { sp.ask = sp.nullifier_key = sp.value = sp.randomness = sp.diversifier = Scalar::zero(); sp.leaf_index = 0; sp.siblings = {}; }
    for (auto& o : pb.pub.outputs) { o.value = o.public_key = o.randomness = o.diversifier = Scalar::zero(); }
    return pb;
}
// The complete warm entry point for one proof: deserialize, copy the trusted structure and set
// its public inputs, verify. Everything a node pays per proof after cold init and before the
// enclosing transaction/bundle checks (nullifiers, anchors, sighash, cache lookup).
struct VerifyBreakdown { double deser = 0, structure = 0, verify = 0, total = 0; bool ok = false; };
static VerifyBreakdown VerifyFull(const ProvenBundle& pb, secp256k1_context* sctx, size_t matrix_threads) {
    using namespace dinero::zk::zkvm;
    VerifyBreakdown r;
    const auto t0 = Clock::now();
    const ShapeData& shape = ShapeFor(pb.n_in, pb.n_out);
    const GeneratorSet& gens = GeneratorSet::cached(shape.gens_need, sctx);
    SpartanProof parsed; if (!SpartanProof::deserialize(pb.proof, parsed, sctx, kOmitErrorTerm)) return r;
    const auto t1 = Clock::now();
    R1CS vcs = shape.structure;
    const auto inputs = PublicInputsOf(pb.pub);
    for (size_t i = 0; i < inputs.size(); ++i) vcs.set_value(Variable{i + 1}, inputs[i]);
    const auto t2 = Clock::now();
    Transcript tv("dinero.shielded.bundle.v2.spike");
    static const bool no_csr = std::getenv("DINERO_NO_CSR") != nullptr;
    r.ok = r1cs_spartan_verify(parsed, vcs, vcs.num_constraints(), vcs.num_variables(), shape.chash, Scalar::one(), gens, tv, sctx, true, true, kOmitErrorTerm, matrix_threads, no_csr ? nullptr : &shape.matrices);
    const auto t3 = Clock::now();
    auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    r.deser = ms(t0, t1); r.structure = ms(t1, t2); r.verify = ms(t2, t3); r.total = ms(t0, t3);
    return r;
}
static void BenchEntryPoint(std::vector<BenchRow>& rows) {
    secp256k1_context* sctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    // Cold: first use of the 2-in-2-out shape in this process (build + hash), reported once.
    g_shapes.clear();
    const auto t_cold = Clock::now();
    const ShapeData& shape = ShapeFor(2, 2);
    rows.push_back(BenchRow{"cold_init_2in2out_ms(build+hash)", shape.structure.num_constraints(), 0, 0, 0, ms_since(t_cold)});
    std::vector<ProvenBundle> proofs; for (int i = 0; i < 6; ++i) proofs.push_back(ProveOnce(2, 2, 500 + i));
    for (size_t threads : {size_t{1}, size_t{8}}) {
        std::vector<double> total, deser, structure, verify;
        for (size_t i = 0; i < proofs.size(); ++i) {
            const auto r = VerifyFull(proofs[i], sctx, threads);
            if (!r.ok) { std::fprintf(stderr, "entry-point verify failed\n"); std::exit(2); }
            if (i) { total.push_back(r.total); deser.push_back(r.deser); structure.push_back(r.structure); verify.push_back(r.verify); }
        }
        char label[96]; std::snprintf(label, sizeof label, "warm_full_verify_2in2out_threads%zu_ms", threads);
        rows.push_back(BenchRow{label, 0, 0, proofs[0].proof.size(), 0, median_of(total)});
        std::fprintf(stderr, "ENTRY threads=%zu deser=%.2f structure_copy+inputs=%.2f verify=%.2f total=%.2f ms\n", threads, median_of(deser), median_of(structure), median_of(verify), median_of(total));
    }
}
// Batch: 50 fresh proofs, ONE budget of 8 workers for the whole batch, serial matrix path inside
// each worker (matrix_threads = 1), so at most 8 threads exist at any time.
static void BenchBatch(std::vector<BenchRow>& rows) {
    std::vector<ProvenBundle> proofs; for (int i = 0; i < 50; ++i) proofs.push_back(ProveOnce(2, 2, static_cast<uint64_t>(i)));
    (void)ShapeFor(2, 2);
    secp256k1_context* sctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    auto t0 = Clock::now();
    for (auto& pb : proofs) if (!VerifyFull(pb, sctx, 1).ok) { std::fprintf(stderr, "batch verify failed\n"); std::exit(2); }
    rows.push_back(BenchRow{"verify_50_sequential_full_ms", 0, 0, 0, 0, ms_since(t0)});
    for (int workers : {1, 2, 4, 8}) {
        t0 = Clock::now();
        std::atomic<size_t> next{0}; std::vector<std::future<bool>> fs;
        for (int t = 0; t < workers; ++t) fs.push_back(std::async(std::launch::async, [&] {
            secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
            bool ok = true; for (size_t i; (i = next++) < proofs.size();) ok = VerifyFull(proofs[i], c, /*matrix_threads=*/1).ok && ok;
            secp256k1_context_destroy(c); return ok; }));
        for (auto& f : fs) if (!f.get()) { std::fprintf(stderr, "parallel batch verify failed\n"); std::exit(2); }
        char label[80]; std::snprintf(label, sizeof label, "verify_50_workers%d_serial_inner_full_ms", workers);
        rows.push_back(BenchRow{label, 0, 0, 0, 0, ms_since(t0)});
    }
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

// Task 7: soundness of the E-less profile, end to end, plus the cost of reusing a cached
// verifier structure (copy + set_value of the public inputs) versus rebuilding it.
static int ProfileSoundness() {
    using namespace dinero::zk::zkvm;
    secp256k1_context* sctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    int failures = 0;
    auto expect = [&](const char* label, bool cond) { std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", label); if (!cond) ++failures; };
    const spike::BundleV2 honest = spike::MakeHonestBundle(2, 2, 1000);
    R1CS cs = spike::BuildBundleCircuitV2(honest);
    const size_t gens_need = std::max<size_t>(4, std::max(HyraxParams::from_n(cs.num_variables()).n_cols, HyraxParams::from_n(cs.num_constraints()).n_cols));
    const GeneratorSet& gens = GeneratorSet::cached(gens_need, sctx);
    const std::vector<Scalar> zeroE(cs.num_constraints(), Scalar::zero());
    auto pub_only_of = [](spike::BundleV2 b) { for (auto& sp : b.spends) { sp.ask = sp.nullifier_key = sp.value = sp.randomness = sp.diversifier = Scalar::zero(); sp.leaf_index = 0; sp.siblings = {}; } for (auto& o : b.outputs) { o.value = o.public_key = o.randomness = o.diversifier = Scalar::zero(); } return b; };
    auto verify_bytes = [&](const std::vector<uint8_t>& ser, const spike::BundleV2& pub, bool omit) {
        R1CS vcs = spike::BuildBundleCircuitV2(pub_only_of(pub));
        SpartanProof parsed; if (!SpartanProof::deserialize(ser, parsed, sctx, omit)) return false;
        Transcript tv("dinero.shielded.bundle.v2.spike");
        return r1cs_spartan_verify(parsed, vcs, vcs.num_constraints(), vcs.num_variables(), spartan_hash_r1cs_structure(vcs), Scalar::one(), gens, tv, sctx, true, true, omit);
    };
    // 1. honest, E-less: accepted
    Transcript t1("dinero.shielded.bundle.v2.spike");
    auto p1 = r1cs_spartan_prove(cs, zeroE, Scalar::one(), gens, t1, sctx, true, true).serialize(sctx);
    expect("honest E-less proof verifies", !p1.empty() && verify_bytes(p1, honest, true));
    // 2. unsatisfied witness (output value +1, public inputs unchanged): prover runs, verifier rejects
    { auto bad = honest; bad.outputs[0].value = bad.outputs[0].value + Scalar::one();
      R1CS bcs = spike::BuildBundleCircuitV2(bad);
      Transcript t("dinero.shielded.bundle.v2.spike");
      auto pb = r1cs_spartan_prove(bcs, zeroE, Scalar::one(), gens, t, sctx, true, true).serialize(sctx);
      expect("unsatisfied witness: E-less proof rejected", pb.empty() || !verify_bytes(pb, bad, true)); }
    // 3. non-zero E under the E-less profile: prover refuses
    { auto E = zeroE; E[7] = Scalar::one(); Transcript t("dinero.shielded.bundle.v2.spike");
      auto p = r1cs_spartan_prove(cs, E, Scalar::one(), gens, t, sctx, true, true);
      expect("non-zero E: E-less prover refuses", p.comm_W.C.empty() && p.outer_sc.empty()); }
    // 4. cross-profile: a legacy (with-E) proof presented to the E-less verifier, and vice versa
    { Transcript t("dinero.shielded.bundle.v2.spike");
      auto legacy = r1cs_spartan_prove(cs, zeroE, Scalar::one(), gens, t, sctx, true, false).serialize(sctx);
      expect("with-E proof rejected by E-less verifier", !verify_bytes(legacy, honest, true));
      expect("E-less proof rejected by with-E verifier", !verify_bytes(p1, honest, false));
      expect("with-E proof still verifies under legacy profile", verify_bytes(legacy, honest, false)); }
    // 5. tamper Ez_claim in the E-less proof (must be zero): locate by re-serialising with Ez=1
    { SpartanProof parsed; SpartanProof::deserialize(p1, parsed, sctx, true); parsed.Ez_claim = Scalar::one();
      expect("tampered Ez_claim rejected", !verify_bytes(parsed.serialize(sctx), honest, true)); }
    // 6. cached-structure reuse cost: copy + set_value of the public inputs vs full rebuild
    { const auto pub = pub_only_of(honest);
      auto t0 = Clock::now(); R1CS built = spike::BuildBundleCircuitV2(pub); const double build_ms = ms_since(t0);
      t0 = Clock::now(); R1CS copy = built; for (size_t i = 1; i <= copy.num_inputs(); ++i) copy.set_value(Variable{i}, built.get_value(Variable{i})); const double copy_ms = ms_since(t0);
      std::printf("  structure rebuild %.2f ms vs copy+set_value %.2f ms (n=%zu)\n", build_ms, copy_ms, built.num_constraints()); }
    std::printf("profile soundness: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--profile-soundness") return ProfileSoundness();
    if (argc > 1 && std::string(argv[1]) == "--negatives") return Negatives();
    if (argc > 1 && std::string(argv[1]) == "--selftest") return SelfTest();
    std::vector<BenchRow> rows;
    if (!(argc > 1 && std::string(argv[1]) == "--no-baseline")) { rows.push_back(BaselineSpend()); rows.push_back(BaselineOutput()); }
    rows.push_back(BenchBundle(1, 2));
    rows.push_back(BenchBundle(2, 2));
    rows.push_back(BenchBundle(4, 2));
    BenchEntryPoint(rows);
    if (!(argc > 1 && std::string(argv[1]) == "--no-batch")) BenchBatch(rows);
    PrintJson(rows, BUILD_TYPE_STR);
    return 0;
}
