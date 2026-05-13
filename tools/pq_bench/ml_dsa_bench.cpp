// ML-DSA-65 verification benchmark harness (Phase 1 of V7_GENESIS_SPEC.md)
//
// Measures verify-only throughput on the host CPU. No consensus integration.
// Results drive PQSchemeRegistry[0x01] `verify_cost_weight` and
// `witness_byte_weight` numbers.
//
// IMPORTANT:
//   - Benchmark MUST run on the weakest fleet node to size consensus correctly.
//     Apple Silicon numbers from this host are NOT sufficient to set mainnet
//     parameters. Ship the binary to the EPYC host, run it there, use THOSE
//     numbers.
//   - Verify is deterministic pure-C clean variant. No AVX2 / NEON tricks.
//   - Pre-generates N keypairs and N signatures up front, then times the
//     verify loop in isolation. Sign/keygen time is reported separately but is
//     not the load-bearing number — consensus only verifies.
//
// Build: via root CMakeLists.txt target `ml_dsa_bench`.
// Run:   ./build-release/ml_dsa_bench [num_iterations=10000] [msg_bytes=64]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "api.h"   // PQClean ML-DSA-65 clean API
}

using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t PK_BYTES  = PQCLEAN_MLDSA65_CLEAN_CRYPTO_PUBLICKEYBYTES;
constexpr size_t SK_BYTES  = PQCLEAN_MLDSA65_CLEAN_CRYPTO_SECRETKEYBYTES;
constexpr size_t SIG_BYTES = PQCLEAN_MLDSA65_CLEAN_CRYPTO_BYTES;

double elapsed_sec(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

void print_header(int iterations, size_t msg_len) {
    std::printf(
        "================================================================\n"
        " Dinero v7 — ML-DSA-65 verify benchmark (PQClean clean variant)\n"
        "================================================================\n"
        " iterations:  %d\n"
        " message len: %zu bytes\n"
        " pubkey:      %zu bytes\n"
        " signature:   %zu bytes\n"
        "----------------------------------------------------------------\n",
        iterations, msg_len, PK_BYTES, SIG_BYTES);
}

} // namespace

int main(int argc, char** argv) {
    int iterations = 10000;
    size_t msg_len = 64;

    if (argc >= 2) iterations = std::atoi(argv[1]);
    if (argc >= 3) msg_len    = static_cast<size_t>(std::atoi(argv[2]));
    if (iterations <= 0) iterations = 10000;
    if (msg_len == 0) msg_len = 64;

    print_header(iterations, msg_len);

    // Stage 1: generate one keypair (reused for all iterations — verify cost
    // is key-independent for ML-DSA).
    std::vector<uint8_t> pk(PK_BYTES), sk(SK_BYTES);
    {
        auto t0 = Clock::now();
        int rc = PQCLEAN_MLDSA65_CLEAN_crypto_sign_keypair(pk.data(), sk.data());
        auto t1 = Clock::now();
        if (rc != 0) {
            std::fprintf(stderr, "keygen failed: rc=%d\n", rc);
            return 1;
        }
        std::printf(" keygen:      %.3f ms (single)\n", elapsed_sec(t0, t1) * 1000.0);
    }

    // Stage 2: generate `iterations` distinct messages + signatures.
    // Each message differs so we exercise the sighash/signature path honestly.
    std::vector<std::vector<uint8_t>> msgs(iterations);
    std::vector<std::vector<uint8_t>> sigs(iterations);
    std::vector<size_t>               siglens(iterations);

    {
        auto t0 = Clock::now();
        for (int i = 0; i < iterations; ++i) {
            msgs[i].resize(msg_len);
            for (size_t j = 0; j < msg_len; ++j) {
                msgs[i][j] = static_cast<uint8_t>((i * 2654435761u + j * 16777619u) & 0xff);
            }
            sigs[i].resize(SIG_BYTES);
            siglens[i] = SIG_BYTES;
            int rc = PQCLEAN_MLDSA65_CLEAN_crypto_sign_signature_ctx(
                sigs[i].data(), &siglens[i],
                msgs[i].data(), msg_len,
                nullptr, 0,
                sk.data());
            if (rc != 0) {
                std::fprintf(stderr, "sign failed at i=%d: rc=%d\n", i, rc);
                return 1;
            }
        }
        auto t1 = Clock::now();
        const double total = elapsed_sec(t0, t1);
        std::printf(" sign batch:  %.3f s total, %.3f ms/sig (wallet-side; not consensus-critical)\n",
                    total, total / iterations * 1000.0);
    }

    // Stage 3: verify-only loop. THIS is the consensus-critical number.
    int ok = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        int rc = PQCLEAN_MLDSA65_CLEAN_crypto_sign_verify_ctx(
            sigs[i].data(), siglens[i],
            msgs[i].data(), msg_len,
            nullptr, 0,
            pk.data());
        if (rc == 0) ++ok;
    }
    auto t1 = Clock::now();
    const double total_verify = elapsed_sec(t0, t1);
    const double per_verify_ms = total_verify / iterations * 1000.0;
    const double verify_per_sec = iterations / total_verify;

    std::printf("----------------------------------------------------------------\n"
                " VERIFY RESULTS\n"
                "   %d / %d signatures verified (must equal iterations)\n"
                "   total:      %.3f s\n"
                "   per verify: %.3f ms\n"
                "   throughput: %.1f verify/s\n"
                "----------------------------------------------------------------\n",
                ok, iterations, total_verify, per_verify_ms, verify_per_sec);

    if (ok != iterations) {
        std::fprintf(stderr, "FAIL: %d verifies failed — benchmark is meaningless\n",
                     iterations - ok);
        return 2;
    }

    // Stage 4: derive provisional consensus parameters.
    //
    // Policy target (per V7_GENESIS_SPEC.md):
    //   - Block verification must fit in 25% of the 120s block interval = 30s.
    //   - Max P2MR spend-inputs per block must be supportable within that budget.
    //
    // Derive "max verifies per block" from this host's throughput, then caveat:
    //   - This host is very likely NOT the weakest fleet node. The EPYC is.
    //   - Mainnet numbers come from the EPYC run, not this one.
    const double budget_seconds = 30.0; // 25% of 120s block interval
    const double max_verifies_per_block_local =
        budget_seconds * verify_per_sec;

    // verify_cost_weight candidate: we want
    //   max_verifies_per_block * verify_cost_weight == MAX_BLOCK_WEIGHT * budget_fraction
    // With MAX_BLOCK_WEIGHT = 8_000_000 and budget_fraction = 0.25 (verify),
    // verify budget in weight units = 2_000_000.
    // Therefore verify_cost_weight = 2_000_000 / max_verifies_per_block
    const uint64_t max_block_weight = 8'000'000ULL;
    const double verify_budget_wu = max_block_weight * 0.25;
    const double verify_cost_weight_local = verify_budget_wu / max_verifies_per_block_local;

    std::printf(" PROVISIONAL CONSENSUS PARAMETERS (LOCAL HOST ONLY)\n"
                "   max verifies / block @ 25%% budget: %.0f\n"
                "   → verify_cost_weight candidate:    %.0f WU\n"
                "   (weakest fleet node benchmark REQUIRED before mainnet set)\n"
                "================================================================\n",
                max_verifies_per_block_local, verify_cost_weight_local);

    return 0;
}
