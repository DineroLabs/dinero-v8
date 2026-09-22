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

#include <algorithm>
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

// Task 2: bundle circuit satisfiability self-test (spec §3.1 relations).
static int SelfTest() {
    using spike::BuildBundleCircuitV2; using spike::MakeHonestBundle; using spike::BundleV2;
    using dinero::zk::zkvm::Scalar;
    int failures = 0;
    auto check = [&](bool c, const char* name) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", name); if (!c) ++failures; };

    BundleV2 honest = MakeHonestBundle(2, 2, 1000);
    check(BuildBundleCircuitV2(honest).is_satisfied(), "honest 2-in-2-out bundle is satisfied");

    BundleV2 bad_balance = honest; bad_balance.outputs[0].value = bad_balance.outputs[0].value + Scalar::one();
    check(!BuildBundleCircuitV2(bad_balance).is_satisfied(), "output value +1 breaks balance");

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
    check(cs.num_constraints() < 60000, "2-in-2-out is under 60k constraints (measured 53,038; legacy per-tx total is ~2.6M)");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--selftest") return SelfTest();
    std::vector<BenchRow> rows;
    rows.push_back(BaselineSpend());
    PrintJson(rows, BUILD_TYPE_STR);
    return 0;
}
